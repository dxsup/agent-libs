#ifndef CYGWING_AGENT
#include <google/protobuf/text_format.h>

#include <grpc_channel_registry.h>

#include "compliance_mgr.h"
#include "infrastructure_state.h"
#include "security_config.h"
#include "statsite_config.h"
#include <utils.h>

using namespace std;
using namespace libsanalyzer;

namespace
{

COMMON_LOGGER();

} // end namespace

compliance_mgr::compliance_mgr(const string &run_root,
			       security_result_handler& result_handler)
	: m_num_grpc_errs(0),
	  m_send_compliance_results(false),
	  m_send_compliance_events(false),
	  m_should_refresh_compliance_tasks(false),
	  m_initialized(false),
	  m_result_handler(result_handler),
	  m_analyzer(NULL),
	  m_cointerface_sock_path("unix:" + run_root + "/cointerface.sock")
{
}

compliance_mgr::~compliance_mgr()
{
	stop_compliance_tasks();
}

void compliance_mgr::init(sinsp_analyzer *analyzer,
			  dragent_configuration *configuration,
			  bool save_errors)
{
	m_analyzer = analyzer;
	m_configuration = configuration;
	m_save_errors = save_errors;

	m_check_periodic_tasks_interval = make_unique<run_on_interval>(1000000000);

	m_comp_events_queue = make_shared<tbb::concurrent_queue<sdc_internal::comp_task_event>>();

	m_grpc_channel = libsinsp::grpc_channel_registry::get_channel(m_cointerface_sock_path);

	m_initialized = true;
}

void compliance_mgr::process_event(sinsp_evt *evt)
{
	if(!m_initialized)
	{
		return;
	}

	uint64_t ts_ns = evt->get_ts();

	m_check_periodic_tasks_interval->run([this]()
        {
		check_tasks();
	}, ts_ns);

}

void compliance_mgr::check_tasks()
{
	check_pending_task_results();

	check_run_tasks_status();

	if(m_should_refresh_compliance_tasks)
	{
		refresh_compliance_tasks();
		m_should_refresh_compliance_tasks = false;
	}
}

void compliance_mgr::set_compliance_calendar(const draiosproto::comp_calendar &calendar,
                                             const bool send_results,
                                             const bool send_events)
{
	LOG_DEBUG("New calendar: %s", calendar.DebugString().c_str());

	m_compliance_calendar = calendar;
	m_send_compliance_results = send_results;
	m_send_compliance_events = send_events;
	request_refresh_compliance_tasks();
}

void compliance_mgr::request_refresh_compliance_tasks()
{
	m_should_refresh_compliance_tasks = true;
}

void compliance_mgr::set_compliance_run(const draiosproto::comp_run &run)
{
	m_compliance_run = run;
}

void compliance_mgr::refresh_compliance_tasks()
{
	LOG_DEBUG("Checking for new compliance tasks from calendar: %s",
	          m_compliance_calendar.DebugString().c_str());

	std::set<uint64_t> new_tasks;

	// The calendar might refer to tasks that are not enabled or
	// tasks that don't match the scope of this agent or the
	// containers it runs. So we create a separate calendar just
	// for the tasks that should actually run.
	sdc_internal::comp_start start;

	start.set_machine_id(m_configuration->machine_id());
	start.set_customer_id(m_configuration->m_customer_id);
	start.set_include_desc(security_config::instance().get_include_desc_in_compliance_results());
	start.set_send_failed_results(security_config::instance().get_compliance_send_failed_results());
	start.set_save_temp_files(security_config::instance().get_compliance_save_temp_files());
	start.set_metrics_statsd_port(libsanalyzer::statsite_config::instance().get_udp_port());

	for(auto &task : m_compliance_calendar.tasks())
	{
		if(!task.enabled())
		{
			continue;
		}

		// Check the scope of the task. Unlike other
		// policies, where we have an event with an associated
		// container id, we need to register this scope with the
		// infrastructure_state object so it can reevaluate the scope
		// as containers come and go.
		infrastructure_state::reg_id_t reg = "compliance_tasks:" + task.name();

		if(m_analyzer)
		{
			m_analyzer->mutable_infra_state()->register_scope(reg,
			                                                  true,
			                                                  true,
			                                                  task.scope_predicates());

			// For now, do a single check of the registered scope and only
			// start the compliance modules if the scope matches. Later,
			// we'll want to periodically check and start/stop modules.
			if(!m_analyzer->infra_state()->check_registered_scope(reg))
			{
				LOG_INFO("Not starting compliance task (scope doesn't match)");
				continue;
			}
		}

		draiosproto::comp_task *run_task = start.mutable_calendar()->add_tasks();

		*run_task = task;

		// If the task is a kube-bench task and if the agent
		// is configured to run a specific variant, pass the
		// variant as a param.
		if(security_config::instance().get_compliance_kube_bench_variant() != "")
		{
			draiosproto::comp_task_param *param = run_task->add_task_params();
			param->set_key("variant");
			param->set_val(security_config::instance().get_compliance_kube_bench_variant());
		}

		new_tasks.insert(task.id());
	}

	if(new_tasks == m_cur_compliance_tasks)
	{
		LOG_INFO("Compliance tasks unchanged, not doing anything");
		return;
	}

	// If here, the set of tasks differ. Stop any existing tasks.
	stop_compliance_tasks();

	m_cur_compliance_tasks = new_tasks;

	LOG_DEBUG("New compliance tasks size: %zu", new_tasks.size());

	if(new_tasks.size() > 0)
	{
		start_compliance_tasks(start);
	}
}

void compliance_mgr::start_compliance_tasks(sdc_internal::comp_start &start)
{
	LOG_DEBUG("Starting compliance tasks: %s", start.DebugString().c_str());

	// Start a thread that does the RPC and writes to the queue
	auto work = [](std::shared_ptr<grpc::Channel> chan,
		       shared_comp_event_queue queue,
		       sdc_internal::comp_start start)
        {
		grpc::ClientContext context;
		std::unique_ptr<sdc_internal::ComplianceModuleMgr::Stub> stub = sdc_internal::ComplianceModuleMgr::NewStub(chan);
		std::unique_ptr<grpc::ClientReader<sdc_internal::comp_task_event>> reader(stub->Start(&context, start));

		sdc_internal::comp_task_event ev;

		while(reader->Read(&ev))
		{
			queue->push(ev);
		}

		grpc::Status status = reader->Finish();

		return status;
	};

	m_start_tasks_future = std::async(std::launch::async, work, m_grpc_channel, m_comp_events_queue, start);
}

void compliance_mgr::run_compliance_tasks(draiosproto::comp_run &run)
{
	LOG_DEBUG("Running compliance tasks: %s", run.DebugString().c_str());

	auto work =
		[](std::shared_ptr<grpc::Channel> chan,
		   draiosproto::comp_run run)
                {
			std::unique_ptr<sdc_internal::ComplianceModuleMgr::Stub> stub = sdc_internal::ComplianceModuleMgr::NewStub(chan);
			grpc::ClientContext context;
			grpc::Status status;
			sdc_internal::comp_run_result res;

			status = stub->RunTasks(&context, run, &res);
			if(!status.ok())
			{
				res.set_successful(false);
				res.set_errstr(status.error_message());
			}

			return res;
		};

	m_run_tasks_future = std::async(std::launch::async, work, m_grpc_channel, run);
}

void compliance_mgr::stop_compliance_tasks()
{
	if(!m_start_tasks_future.valid())
	{
		return;
	}

	auto work =
		[](std::shared_ptr<grpc::Channel> chan)
                {
			sdc_internal::comp_stop stop;

			std::unique_ptr<sdc_internal::ComplianceModuleMgr::Stub> stub = sdc_internal::ComplianceModuleMgr::NewStub(chan);
			grpc::ClientContext context;
			grpc::Status status;
			sdc_internal::comp_stop_result res;

			status = stub->Stop(&context, stop, &res);
			if(!status.ok())
			{
				res.set_successful(false);
				res.set_errstr(status.error_message());
			}

			return res;
		};

	std::future<sdc_internal::comp_stop_result> stop_future = std::async(std::launch::async, work, m_grpc_channel);

	// Wait up to 10 seconds for the stop to complete.
	if(stop_future.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
	{
		LOG_ERROR("Did not receive response to Compliance Stop() call within 10 seconds");
		return;
	}
	else
	{
		sdc_internal::comp_stop_result res = stop_future.get();
		if(!res.successful())
		{
			LOG_DEBUG("Compliance Stop() call returned error %s",
			          res.errstr().c_str());
		}
	}
}

bool compliance_mgr::get_future_runs(sdc_internal::comp_get_future_runs &req, sdc_internal::comp_future_runs &res, std::string &errstr)
{
	// This does a blocking RPC without a separate thread or
	// future. But it's only used for testing.

	std::unique_ptr<sdc_internal::ComplianceModuleMgr::Stub> stub = sdc_internal::ComplianceModuleMgr::NewStub(m_grpc_channel);

	grpc::ClientContext context;
	grpc::Status status;

	status = stub->GetFutureRuns(&context, req, &res);

	if(!status.ok())
	{
		errstr = status.error_message();
		return false;
	}

	return true;
}

void compliance_mgr::check_pending_task_results()
{
	// First check the status of the future. This is returned when
	// the start completes (either due to an error or due to being
	// stopped)
	if(m_start_tasks_future.valid() &&
	   m_start_tasks_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		grpc::Status res = m_start_tasks_future.get();

		if(!res.ok())
		{
			LOG_ERROR("Could not start compliance tasks (%s),"
			          " trying again in %" PRIu64 " seconds",
			          res.error_message().c_str(),
			          security_config::instance().get_compliance_refresh_interval() / 1000000000);
		}
		else
		{
			LOG_DEBUG("Compliance Start GRPC completed");
		}
	}

	// Now try to read any pending compliance messages from the queue
	sdc_internal::comp_task_event cevent;

	while(m_comp_events_queue->try_pop(cevent))
	{
		LOG_DEBUG("Response from compliance start: cevent=%s",
		          cevent.DebugString().c_str());

		if(!cevent.init_successful())
		{
			LOG_ERROR("Could not initialize compliance task %s (%s), trying again in %" PRIu64 " seconds",
			          cevent.task_name().c_str(),
			          cevent.errstr().c_str(),
			          security_config::instance().get_compliance_refresh_interval() / 1000000000);

			m_num_grpc_errs++;

			if(m_save_errors)
			{
				m_task_errors[cevent.task_name()].push_back(cevent.errstr());
			}
		}


		if(m_send_compliance_events)
		{
			for(int i=0; i<cevent.events().events_size(); i++)
			{
				// XXX/mstemm need to fill this in once we've decided on a message format.
			}
		}

		if(m_send_compliance_results)
		{
			if(cevent.results().results_size() > 0)
			{
				m_result_handler.security_mgr_comp_results_ready(sinsp_utils::get_current_time_ns(),
				                                                 &(cevent.results()));
			}
		}
	}
}

void compliance_mgr::check_run_tasks_status()
{
	if(m_run_tasks_future.valid() &&
	   m_run_tasks_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		sdc_internal::comp_run_result res = m_run_tasks_future.get();

		if(!res.successful())
		{
			LOG_ERROR("Could not run compliance tasks (%s)",
			          res.errstr().c_str());
		}
	}

	if(!m_compliance_run.task_ids().empty())
	{
		run_compliance_tasks(m_compliance_run);

		// Reset to empty message
		m_compliance_run = draiosproto::comp_run();
	}
}

#endif // CYGWING_AGENT
