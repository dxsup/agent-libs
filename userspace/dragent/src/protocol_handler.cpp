#include <memory>

#include "protocol_handler.h"
#include "configuration.h"
#include "utils.h"
#include "common_logger.h"
#include "configuration_manager.h"

#include "draios.pb.h"
#include "tap.pb.h"
#include "secure.pb.h"
#include "profiling.pb.h"
#include "metric_serializer.h"

#include <Poco/NumberFormatter.h>

using Poco::NumberFormatter;

COMMON_LOGGER();

type_config<bool> protocol_handler::c_compression_enabled(
	true,
	"set to true to compress protobufs sent to the collector",
	"compression",
	"enabled");

type_config<bool> protocol_handler::c_print_protobuf(
	false,
	"set to true to print the protobuf with each flush",
	"protobuf_print");

type_config<bool> protocol_handler::c_audit_tap_debug_only(
	true,
	"set to true to only log audit tap, but not emit",
	"audit_tap",
	"debug_only");

type_config<bool> protocol_handler::c_secure_audit_debug_enabled(
	false,
	"set to true to log secure audit protobufs",
	"secure_audit_streams",
	"debug");

type_config<bool> protocol_handler::c_secure_netsec_debug_enabled(
	false,
	"set to true to log secure netsec protobufs",
	"network_topology",
	"debug");

type_config<bool> protocol_handler::c_secure_profiling_debug_enabled(
	false,
	"set to true to log secure profiling protobufs",
	"falcobaseline", // aka secure_profiling
	"debug");

protocol_handler::protocol_handler(protocol_queue& queue) :
    m_last_loop_ns(0),
    m_queue(queue)
{
}

protocol_handler::~protocol_handler()
{
}

void protocol_handler::set_root_dir(const std::string &root_dir)
{
    if (!dragent::metric_serializer::c_metrics_dir.get_value().empty())
    {
        std::string dir = Poco::Path(root_dir).append(
			dragent::metric_serializer::c_metrics_dir.get_value()).toString();
		m_file_emitter.set_metrics_directory(dir);
    }
}

std::shared_ptr<serialized_buffer> protocol_handler::handle_uncompressed_sample(uint64_t ts_ns,
                          std::shared_ptr<draiosproto::metrics>& metrics,
                          uint32_t flush_interval,
                          std::shared_ptr<protobuf_compressor>& compressor)
{
	ASSERT(metrics);
	m_last_loop_ns = sinsp_utils::get_current_time_ns();

	if(c_print_protobuf.get_value())
	{
		LOG_INFO(metrics->DebugString());
	}

	std::shared_ptr<serialized_buffer> buffer = dragent_protocol::message_to_buffer(
		ts_ns,
		draiosproto::message_type::METRICS,
		*metrics,
		compressor);

	buffer->flush_interval = flush_interval;

	if(!buffer)
	{
		LOG_ERROR("NULL converting message to buffer");
		return nullptr;
	}

	return buffer;
}

uint64_t protocol_handler::get_last_loop_ns() const
{
	return m_last_loop_ns;
}

void protocol_handler::security_mgr_policy_events_ready(uint64_t ts_ns, draiosproto::policy_events *events)
{
	if(c_print_protobuf.get_value())
	{
		LOG_INFO(std::string("Security Events:") + events->DebugString());
	}

	// It would be better to plumb through the negotiated value, but this is
	// what we get for now
	protocol_compression_method compression =
	        c_compression_enabled.get_value() ?
	                protocol_compression_method::GZIP :
	                protocol_compression_method::NONE;
	std::shared_ptr<protobuf_compressor> compressor =
	        protobuf_compressor_factory::get(compression);

	std::shared_ptr<serialized_buffer> buffer = dragent_protocol::message_to_buffer(
		ts_ns,
		draiosproto::message_type::POLICY_EVENTS,
		*events,
		compressor);

	if(!buffer)
	{
		LOG_ERROR("NULL converting message to buffer");
		return;
	}

	LOG_INFO("sec_evts len=" + NumberFormatter::format(buffer->buffer.size())
			   + ", ne=" + NumberFormatter::format(events->events_size()));

	if(!m_queue.put(buffer, protocol_queue::BQ_PRIORITY_MEDIUM))
	{
		LOG_INFO("Queue full, discarding sample");
	}
}

void protocol_handler::security_mgr_throttled_events_ready(uint64_t ts_ns,
							     draiosproto::throttled_policy_events *tevents,
							     uint32_t total_throttled_count)
{
	if(c_print_protobuf.get_value())
	{
		LOG_INFO(std::string("Throttled Security Events:") + tevents->DebugString());
	}

	protocol_compression_method compression =
	        c_compression_enabled.get_value() ?
	                protocol_compression_method::GZIP :
	                protocol_compression_method::NONE;
	std::shared_ptr<protobuf_compressor> compressor =
	        protobuf_compressor_factory::get(compression);

	std::shared_ptr<serialized_buffer> buffer = dragent_protocol::message_to_buffer(
		ts_ns,
		draiosproto::message_type::THROTTLED_POLICY_EVENTS,
		*tevents,
		compressor);

	if(!buffer)
	{
		LOG_ERROR("NULL converting message to buffer");
		return;
	}

	LOG_INFO("sec_evts len=" + NumberFormatter::format(buffer->buffer.size())
			   + ", nte=" + NumberFormatter::format(tevents->events_size())
			   + ", tcount=" + NumberFormatter::format(total_throttled_count));

	if(!m_queue.put(buffer, protocol_queue::BQ_PRIORITY_LOW))
	{
		LOG_INFO("Queue full, discarding sample");
	}
}

void protocol_handler::security_mgr_comp_results_ready(uint64_t ts_ns, const draiosproto::comp_results *results)
{
	if(c_print_protobuf.get_value())
	{
		LOG_INFO(std::string("Compliance Results:") + results->DebugString());
	}

	protocol_compression_method compression =
	        c_compression_enabled.get_value() ?
	                protocol_compression_method::GZIP :
	                protocol_compression_method::NONE;
	std::shared_ptr<protobuf_compressor> compressor =
	        protobuf_compressor_factory::get(compression);

	std::shared_ptr<serialized_buffer> buffer = dragent_protocol::message_to_buffer(
		ts_ns,
		draiosproto::message_type::COMP_RESULTS,
		*results,
		compressor);

	if(!buffer)
	{
		LOG_ERROR("NULL converting message to buffer");
		return;
	}

	LOG_INFO("sec_comp_results len=" + NumberFormatter::format(buffer->buffer.size())
			   + ", ne=" + NumberFormatter::format(results->results_size()));

	if(!m_queue.put(buffer, protocol_queue::BQ_PRIORITY_LOW))
	{
		LOG_INFO("Queue full, discarding sample");
	}
}

void protocol_handler::audit_tap_data_ready(uint64_t ts_ns, const tap::AuditLog *audit_log)
{
	if(c_print_protobuf.get_value() || c_audit_tap_debug_only.get_value())
	{
		LOG_INFO(std::string("Audit tap data:") + audit_log->DebugString());
	}
	std::shared_ptr<protobuf_compressor> compressor = gzip_protobuf_compressor::get(Z_DEFAULT_COMPRESSION);

	std::shared_ptr<serialized_buffer> buffer = dragent_protocol::message_to_buffer(
		ts_ns,
		draiosproto::message_type::AUDIT_TAP,
		*audit_log,
		compressor /* compression always enabled */);

	if(!buffer)
	{
		LOG_ERROR("NULL converting audit_tap message to buffer");
		return;
	}

	LOG_INFO("audit_tap len=" + NumberFormatter::format(buffer->buffer.size())
			   + ", np=" + NumberFormatter::format(audit_log->newprocessevents().size())
		           + ", pe=" + NumberFormatter::format(audit_log->processexitevents().size())
		           + ", c=" + NumberFormatter::format(audit_log->connectionevents().size())
			   + ", e=" + NumberFormatter::format(audit_log->environmentvariables().size())
			   );

	if(c_audit_tap_debug_only.get_value())
	{
		return;
	}

	if(!m_queue.put(buffer, protocol_queue::BQ_PRIORITY_MEDIUM))
	{
		LOG_INFO("Queue full, discarding sample");
	}
}

std::shared_ptr<serialized_buffer> protocol_handler::handle_log_report(uint64_t ts_ns,
					 const draiosproto::dirty_shutdown_report& report)
{

	protocol_compression_method compression =
	        c_compression_enabled.get_value() ?
	                protocol_compression_method::GZIP :
	                protocol_compression_method::NONE;
	std::shared_ptr<protobuf_compressor> compressor =
	        protobuf_compressor_factory::get(compression);

	std::shared_ptr<serialized_buffer> report_serialized = dragent_protocol::message_to_buffer(
		ts_ns,
		draiosproto::message_type::DIRTY_SHUTDOWN_REPORT,
		report,
		compressor);

	if(!report_serialized)
	{
		LOG_ERROR("NULL converting message to buffer");
		return nullptr;
	}
	return report_serialized;
}

void protocol_handler::secure_audit_data_ready(uint64_t ts_ns, const secure::Audit *secure_audit)
{
	if(c_secure_audit_debug_enabled.get_value())
	{
		LOG_INFO(std::string("Secure Audit data:") + secure_audit->DebugString());
	}

	std::shared_ptr<protobuf_compressor> compressor = gzip_protobuf_compressor::get(Z_DEFAULT_COMPRESSION);

	std::shared_ptr<serialized_buffer> buffer = dragent_protocol::message_to_buffer(
		ts_ns,
		draiosproto::message_type::SECURE_AUDIT,
		*secure_audit,
		compressor /* compression always enabled */);

	if(!buffer)
	{
		LOG_ERROR("NULL converting secure_audit message to buffer");
		return;
	}

	LOG_INFO("secure_audit len=" + NumberFormatter::format(buffer->buffer.size())
			   + ", conn=" + NumberFormatter::format(secure_audit->connections().size())
			   + ", cmd=" + NumberFormatter::format(secure_audit->executed_commands().size())
			   + ", ke=" + NumberFormatter::format(secure_audit->k8s_audits().size())
			   + ", file=" + NumberFormatter::format(secure_audit->file_accesses().size())
			   );

	if(!m_queue.put(buffer, protocol_queue::BQ_PRIORITY_MEDIUM))
	{
		LOG_INFO("Queue full, discarding sample");
	}
}

void protocol_handler::secure_profiling_data_ready(uint64_t ts_ns, const secure::profiling::fingerprint *secure_profiling_fingerprint)
{
	if(c_secure_profiling_debug_enabled.get_value())
	{
		LOG_INFO(std::string("Secure Profiling Fingerprint data:") + secure_profiling_fingerprint->DebugString());
	}

	std::shared_ptr<protobuf_compressor> compressor = gzip_protobuf_compressor::get(Z_DEFAULT_COMPRESSION);

	std::shared_ptr<serialized_buffer> buffer = dragent_protocol::message_to_buffer(
		ts_ns,
		draiosproto::message_type::SECURE_PROFILING_FINGERPRINT,
		*secure_profiling_fingerprint,
		compressor /* compression always enabled */);

	if(!buffer)
	{
		LOG_ERROR("NULL converting secure_profiling_fingerprint message to buffer");
		return;
	}

	LOG_INFO("secure_profiling_fingerprint len=" + NumberFormatter::format(buffer->buffer.size())
			   + ", progs=" + NumberFormatter::format(secure_profiling_fingerprint->progs().size())
			   + ", container=" + NumberFormatter::format(secure_profiling_fingerprint->container().size())
			   );

	if(!m_queue.put(buffer, protocol_queue::BQ_PRIORITY_MEDIUM))
	{
		LOG_INFO("Queue full, discarding secure profiling fingerprint sample");
	}
}

void protocol_handler::secure_netsec_data_ready(uint64_t ts_ns, const secure::K8SCommunicationSummary *k8s_communication_summary)
{
	if(c_secure_netsec_debug_enabled.get_value())
	{
		LOG_INFO("Secure Network Communication data:" + k8s_communication_summary->DebugString());
	}

	std::shared_ptr<protobuf_compressor> compressor = gzip_protobuf_compressor::get(Z_DEFAULT_COMPRESSION);

	std::shared_ptr<serialized_buffer> buffer = dragent_protocol::message_to_buffer(
		ts_ns,
		draiosproto::message_type::SECURE_NETSEC_SUMMARY,
		*k8s_communication_summary,
		compressor /* compression always enabled */);

	if(!buffer)
	{
		LOG_ERROR("NULL converting secure_netsec_communication message to buffer");
		return;
	}

	LOG_INFO("secure_netsec_communication len=" + NumberFormatter::format(buffer->buffer.size()));

	if(!m_queue.put(buffer, protocol_queue::BQ_PRIORITY_MEDIUM))
	{
		LOG_INFO("Queue full, discarding secure network communication sample");
	}
}

void protocol_handler::transmit(draiosproto::message_type type, 
                                const google::protobuf::Message& message,
                                protocol_queue::item_priority priority,
                                uint64_t ts_ns)
{
	const bool compression_enabled = c_compression_enabled.get_value();
	protocol_compression_method compression = compression_enabled
	        ? protocol_compression_method::GZIP
	        : protocol_compression_method::NONE;

	std::shared_ptr<protobuf_compressor> compressor;
	if (compression == protocol_compression_method::NONE)
	{
		compressor = null_protobuf_compressor::get();
	}
	else
	{
		compressor = gzip_protobuf_compressor::get(Z_DEFAULT_COMPRESSION);
	}
	if (ts_ns == 0)
	{
		ts_ns = sinsp_utils::get_current_time_ns();
	}

	std::shared_ptr<serialized_buffer> item = 
	        dragent_protocol::message_to_buffer(ts_ns, type, message, compressor);

	if (!item)
	{
		LOG_ERROR("NULL converting message to item");
		return;
	}

	if (!m_queue.put(item, priority))
	{
		LOG_ERROR("Queue is full, discarding message %d", type);
		return;
	}

    if (!dragent::metric_serializer::c_metrics_dir.get_value().empty())
	{
		m_file_emitter.emit_message_to_file(message);
	}
}
