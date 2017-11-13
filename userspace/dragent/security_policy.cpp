#include <string>

#include <Poco/UUIDGenerator.h>

#include "logger.h"

#include "security_mgr.h"
#include "security_policy.h"

using namespace std;

security_policy::security_policy(security_mgr *mgr,
				 dragent_configuration *configuration,
				 const draiosproto::policy &policy,
				 shared_ptr<coclient> &coclient)
	: m_mgr(mgr),
	  m_configuration(configuration),
	  m_policy(policy),
	  m_coclient(coclient),
	  m_has_outstanding_actions(false)
{
	m_print.SetSingleLineMode(true);
	m_metrics.reset();
}

security_policy::~security_policy()
{
}

bool security_policy::process_event(sinsp_evt *evt)
{
	draiosproto::policy_event *event;

	if(!m_policy.enabled())
	{
		m_metrics.incr(evt_metrics::EVM_POLICY_DISABLED);
		return false;
	}

	if(m_evttypes[evt->get_type()] &&
	   (event = match_event(evt)) != NULL)
	{
		g_log->debug("Event matched policy: " + name());

		// Perform the actions associated with the
		// policy. The actions will add their action
		// results to the policy event as they complete.
		if(perform_actions(evt, event))
		{
			g_log->debug("perform_actions() returned true, not testing later policies");
			return true;
		}
	}

	return false;
}

std::string security_policy::to_string()
{
	string tmp;
	m_print.PrintToString(m_policy, &tmp);
	return tmp;
}

const std::string &security_policy::name()
{
	return m_policy.name();
}

void security_policy::log_metrics()
{
	g_log->debug("Policy event counts: (" + name() + "): " + m_metrics.to_string());
}

void security_policy::reset_metrics()
{
	m_metrics.reset();
}

bool security_policy::has_action(const draiosproto::action_type &atype)
{
	for(auto &action : m_policy.actions())
	{
		if(action.type() == atype)
		{
			return true;
		}
	}

	return false;
}

void security_policy::note_action_complete(actions_state &astate)
{
	if(--astate.m_num_remaining_actions == 0)
	{
		m_has_outstanding_actions = true;
	}
}

draiosproto::action_result *security_policy::has_action_result(draiosproto::policy_event *evt,
							       const draiosproto::action_type &atype)
{
	for(int i=0; i<evt->action_results_size(); i++)
	{
		draiosproto::action_result *aresult = evt->mutable_action_results(i);
		if(aresult->type() == atype)
		{
			return aresult;
		}
	}

	return NULL;
}


bool security_policy::perform_actions(sinsp_evt *evt, draiosproto::policy_event *event)
{
	m_outstanding_actions.emplace_back(event, m_policy.actions().size());
	actions_state &astate = m_outstanding_actions.back();

	sinsp_threadinfo *tinfo = evt->get_thread_info();
	sinsp_container_info container_info;
	string container_id;
	uint64_t pid = 0;

	if(tinfo)
	{
		container_id = tinfo->m_container_id;
		pid = tinfo->m_pid;
	}

	for(auto &action : m_policy.actions())
	{
		draiosproto::action_result *result = astate.m_event->add_action_results();
		result->set_type(action.type());
		result->set_successful(true);

		coclient::response_cb_t callback = [result, &astate, this] (bool successful, google::protobuf::Message *response_msg)
		{
			sdc_internal::docker_command_result *res = (sdc_internal::docker_command_result *) response_msg;
			if(!successful)
			{
				result->set_successful(false);
				result->set_errmsg("RPC Not successful");
			}

			if(!res->successful())
			{
				result->set_successful(false);
				result->set_errmsg("Could not perform docker command: " + res->errstr());
			}

			note_action_complete(astate);

			string tmp;
			m_print.PrintToString(*result, &tmp);
			g_log->debug(string("Docker cmd action result: ") + tmp);
		};

		string tmp;
		bool apply_scope = false;
		string errstr;

		switch(action.type())
		{
		case draiosproto::ACTION_CAPTURE:

			result->set_token(Poco::UUIDGenerator().createRandom().toString());
			if (action.capture().has_is_limited_to_container())
			{
				apply_scope = action.capture().is_limited_to_container();
			}

			if(!m_mgr->start_capture(evt->get_ts(),
						 m_policy.name(),
						 result->token(),
						 (action.capture().has_filter() ? action.capture().filter() : ""),
						 action.capture().before_event_ns(),
						 action.capture().after_event_ns(),
						 apply_scope,
						 container_id,
						 pid,
						 errstr))
			{
				result->set_successful(false);
				result->set_errmsg(errstr);
			}
			else
			{
				// We had at least one capture action
				// that was successful, so we must
				// send the policy event immediately.
				astate.m_send_now = true;
			}

			note_action_complete(astate);

			m_print.PrintToString(*result, &tmp);
			g_log->debug(string("Capture action result: ") + tmp);

			break;
		case draiosproto::ACTION_PAUSE:
			m_coclient->perform_docker_cmd(sdc_internal::PAUSE, container_id, callback);
			break;
		case draiosproto::ACTION_STOP:
			m_coclient->perform_docker_cmd(sdc_internal::STOP, container_id, callback);
			break;
		default:
			string errstr = string("Policy Action ") + std::to_string(action.type()) + string(" not implemented yet");
			result->set_successful(false);
			result->set_errmsg(errstr);
			g_log->debug(errstr);
		}
	}

	if(astate.m_num_remaining_actions == 0)
	{
		m_has_outstanding_actions = true;
	}

	return true;
}


void security_policy::check_outstanding_actions(uint64_t ts_ns)
{
	if (!m_has_outstanding_actions)
	{
		return;
	}

	auto no_outstanding_actions = [ts_ns, this] (actions_state &act)
	{
		if(act.m_num_remaining_actions == 0)
		{
			bool accepted = m_mgr->accept_policy_event(ts_ns, act.m_event, act.m_send_now);

			const draiosproto::action_result *aresult;

			if((aresult = has_action_result(act.m_event.get(), draiosproto::ACTION_CAPTURE)) &&
			   aresult->successful())
			{
				string token = aresult->token();

				if(token.empty())
				{
					g_log->error("Could not find capture token for policy event that had capture action?");
				}
				else
				{
					if(accepted)
					{
						// If one of the actions was a capture, when
						// we scheduled the capture we deferred
						// actually sending the capture data. Start
						// sending the data now.
						m_mgr->start_sending_capture(token);
					}
					else
					{
						// The policy event was throttled, so we
						// should stop the capture without sending
						// anything.
						m_mgr->stop_capture(token);
					}
				}
			}
			return true;
		}

		return false;
	};

	m_outstanding_actions.erase(remove_if(m_outstanding_actions.begin(),
					      m_outstanding_actions.end(),
					      no_outstanding_actions),
				    m_outstanding_actions.end());

	m_has_outstanding_actions = false;
}

bool security_policy::match_scope(sinsp_evt *evt)
{
	sinsp_threadinfo *tinfo = evt->get_thread_info();
	std::string container_id = (tinfo ? tinfo->m_container_id : "");
	sinsp_analyzer *analyzer = m_mgr->analyzer();
	std::string machine_id = analyzer->get_configuration_read_only()->get_machine_id();

	if(!m_policy.container_scope() && !m_policy.host_scope()) {
		// This should never occur. Err on the side of allowing the policy to run.
		g_log->error("Impossible scope with host/container_scope == false. Allowing policy anyway.");
		return true;
	}

	return analyzer->infra_state()->match_scope(container_id, machine_id, m_policy);
}

falco_security_policy::falco_security_policy(security_mgr *mgr,
					     dragent_configuration *configuration,
					     const draiosproto::policy &policy,
					     sinsp *inspector,
					     shared_ptr<falco_engine> &falco_engine,
					     shared_ptr<coclient> &coclient)
	: security_policy(mgr,
			  configuration,
			  policy,
			  coclient),
	  m_falco_engine(falco_engine),
	  m_formatters(inspector)
{

	// Use the name and tags filter to create a ruleset. We'll use
	// this ruleset to run only the subset of rules we're
	// interested in.
	string all_rules = ".*";

	// We *only* want those rules selected by name/tags, so first disable all rules.
	m_falco_engine->enable_rule(all_rules, false, m_policy.name());

	if(policy.falco_details().rule_filter().has_name())
	{
		m_rule_filter = policy.falco_details().rule_filter().name();
		m_falco_engine->enable_rule(m_rule_filter, true, m_policy.name());
	}

	for(auto tag : policy.falco_details().rule_filter().tags())
	{
		m_tags.insert(tag);
	}

	m_falco_engine->enable_rule_by_tag(m_tags, true, m_policy.name());

	m_ruleset_id = m_falco_engine->find_ruleset_id(m_policy.name());

	m_falco_engine->evttypes_for_ruleset(m_evttypes, m_policy.name());
}

falco_security_policy::~falco_security_policy()
{
}

bool falco_security_policy::check_conditions(sinsp_evt *evt)
{
	if(!m_falco_engine)
	{
		m_metrics.incr(evt_metrics::EVM_NO_FALCO_ENGINE);
		return false;
	}

	if((evt->get_info_flags() & EF_DROP_FALCO) != 0)
	{
		m_metrics.incr(evt_metrics::EVM_EF_DROP_FALCO);
		return false;
	}

	if (m_policy.scope_predicates().size() > 0 && !match_scope(evt))
	{
		m_metrics.incr(evt_metrics::EVM_SCOPE_MISS);
		return false;
	}

	return true;
}

draiosproto::policy_event *falco_security_policy::match_event(sinsp_evt *evt)
{
	if(!check_conditions(evt))
	{
		return NULL;
	}

	// Check to see if this policy has any outstanding
	// actions that are now complete. If so, send the
	// policy event messages for each.
	check_outstanding_actions(evt->get_ts());

	try {
		unique_ptr<falco_engine::rule_result> res = m_falco_engine->process_event(evt, m_ruleset_id);
		if(res)
		{
			draiosproto::policy_event *event = new draiosproto::policy_event();
			draiosproto::falco_event_detail *fdetail = event->mutable_falco_details();
			sinsp_threadinfo *tinfo = evt->get_thread_info();
			string output;

			g_log->debug("Event matched falco policy: rule=" + res->rule);

			event->set_timestamp_ns(evt->get_ts());
			event->set_policy_id(m_policy.id());
			if(tinfo && !tinfo->m_container_id.empty())
			{
				event->set_container_id(tinfo->m_container_id);
			}

			fdetail->set_rule(res->rule);

			m_formatters.tostring(evt, res->format, &output);
			fdetail->set_output(output);

			m_metrics.incr(evt_metrics::EVM_MATCHED);
			event->set_sinsp_events_dropped(m_mgr->analyzer()->recent_sinsp_events_dropped());
			return event;
		}
	}
	catch (falco_exception& e)
	{
		g_log->error("Error processing event against falco engine: " + string(e.what()));
	}

	m_metrics.incr(evt_metrics::EVM_FALCO_MISS);
	return NULL;
}

std::string falco_security_policy::to_string()
{
	string tmp;

	m_fstr = security_policy::to_string();

	m_fstr += " rule_filter=\"" + m_rule_filter + "\" tags=[";
	for(auto it = m_tags.begin(); it != m_tags.end(); it++)
	{
		m_fstr += (it == m_tags.begin() ? "" : ",");
		m_fstr += *it;
	}

	m_fstr += "]";

	return m_fstr;
}
