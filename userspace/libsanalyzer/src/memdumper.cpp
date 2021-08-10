#define __STDC_FORMAT_MACROS

#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "analyzer.h"
#include "analyzer_int.h"
#include "common_logger.h"
#include "draios.pb.h"
#include "utils.h"

#include <fcntl.h>
#include <iostream>
#include <memdumper.h>
#include <sinsp.h>
#include <sinsp_int.h>
#include <sys/mman.h>

COMMON_LOGGER();

using namespace std;

extern sinsp_evttables g_infotables;

sinsp_memory_dumper::sinsp_memory_dumper(sinsp* inspector)
	: m_inspector(inspector)
	, m_file_id(0)
	, m_f(NULL)
	, m_cf(NULL)
	, m_disabled(false)
	, m_disabled_by_autodisable(false)
	, m_last_autodisable_ns(0)
	, m_switches_to_go(0)
	, m_delayed_switch_states_needed(false)
	, m_delayed_switch_states_ready(false)
	, m_processed_events_between_switch_states(0)
	, m_autodisable_threshold_reached_count(0)
{
}

void sinsp_memory_dumper::init(uint64_t bufsize,
                               uint64_t max_disk_size,
                               uint64_t max_init_attempts,
                               bool autodisable,
                               uint64_t capture_headers_percentage_threshold,
                               uint64_t time_between_switch_states_ms,
                               uint64_t re_enable_interval_minutes)
{
	LOG_INFO("memdumper: initializing memdumper, bufsize=%" PRIu64 ", max_disk_size=%" PRIu64,
	         bufsize,
	         max_disk_size);

	m_autodisable = autodisable;
	m_capture_headers_percentage_threshold = capture_headers_percentage_threshold;
	m_min_time_between_switch_states_ns =
	    (uint64_t)time_between_switch_states_ms * (uint64_t)1000000;
	m_re_enable_interval_ns = re_enable_interval_minutes * 60 * 1000000000;
	m_dump_buffer_headers_size = 0;

	// Try to allocate a shared memory region of this size
	// immediately. If we can't, log an error and disable
	// memdumper. (The memdumper itself allocates memory
	// across several memory regions but in aggregate the
	// amount is the same).
	string name = "/dragent-mem-test";
	shm_unlink(name.c_str());

	int shm_fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IRWXU);
	if (shm_fd == -1)
	{
		string err = "Could not open shm file %s: %s. Disabling memdump";
		LOG_ERROR(err.c_str(), name.c_str(), strerror_r(errno, m_errbuf, sizeof(m_errbuf)));
		m_disabled = true;
	}
	else
	{
		int rc = EINTR;
		uint64_t attempts;

		// posix_fallocate can return EINTR, in which case we
		// should retry. Don't try more than the configured
		// number of times, though.
		for (attempts = 1; rc == EINTR && attempts <= max_init_attempts; attempts++)
		{
			rc = posix_fallocate(shm_fd, 0, bufsize);
		}

		if (rc != 0)
		{
			string retstr = string(" after ") + to_string(attempts) + " attempts";
			string err = "Could not allocate %" PRIu64
			             " bytes of shared memory for memdump%s: %s (%d). Disabling memdump";
			LOG_ERROR(err.c_str(),
			          bufsize,
			          (rc == EINTR ? retstr.c_str() : ""),
			          strerror_r(rc, m_errbuf, sizeof(m_errbuf)),
			          rc);
			m_disabled = true;
		}
		::close(shm_fd);
		shm_unlink(name.c_str());
	}

	if (m_disabled)
	{
		return;
	}

	m_max_disk_size = max_disk_size;

	//
	// Let the inspector know that we're dumping
	//
	m_inspector->m_is_dumping = true;

	//
	// Initialize the buffers. In the common case, we use 2 memory
	// buffers. There is a possibility of using 3 when there
	// are simultaneous readers and writers of the memory
	// buffer. Hence the dividing by 3.
	//
	m_bsize = bufsize / 3;

	// 2 states
	for (uint32_t i = 0; i < 2; i++)
	{
		string errstr;
		string name = "/dragent-memdumper-" + to_string(m_file_id++);
		std::shared_ptr<sinsp_memory_dumper_state> state =
		    make_shared<sinsp_memory_dumper_state>(m_inspector, m_bsize, name);
		m_states.emplace_back(state);

		if (!state->open(errstr))
		{
			LOG_ERROR("memdump: could not open memdumer state %s: %s. Memory dump disabled",
			          name.c_str(),
			          errstr.c_str());
			m_disabled = true;
		}
	}
	m_active_state = m_states.begin();

	// No reader right now
	m_reader_state = m_states.rend();
	m_reader_active = false;
	m_f = NULL;
}

sinsp_memory_dumper::~sinsp_memory_dumper()
{
	if (m_f != NULL)
	{
		fclose(m_f);
	}

	if (m_cf != NULL)
	{
		fclose(m_cf);
	}
}

void sinsp_memory_dumper::close()
{
	switch_states(0);
	m_inspector->m_is_dumping = false;
}

// Read as much of the shared memory buffer held in state as possible
// using the provided inspector.
bool sinsp_memory_dumper::read_membuf_using_inspector(
    sinsp& inspector,
    const std::shared_ptr<sinsp_memory_dumper_state>& state,
    std::unique_ptr<sinsp_memory_dumper_job>& job)
{
	int32_t res;
	sinsp_evt* ev;

	// Flush the dumper state, which also returns the number of
	// bytes written so far. Don't read past this point.
	uint64_t dumper_bytes_written = state->flush();

	// Force a seek to 0 and back to our position to ensure that
	// no cached read data is kept--we may have read data from the
	// file that was stale and updated by the flush we just did.
	uint64_t bytes_read = inspector.get_bytes_read();
	inspector.fseek(0);
	inspector.fseek(bytes_read);

	LOG_DEBUG("memdumper: reading %s from pos %lu to %lu",
	          state->m_shm_name.c_str(),
	          inspector.get_bytes_read(),
	          dumper_bytes_written);

	while (inspector.get_bytes_read() < dumper_bytes_written)
	{
		res = inspector.next(&ev);

		if (res == SCAP_EOF || job->m_state == sinsp_memory_dumper_job::ST_STOPPPED)
		{
			break;
		}
		else if (res == SCAP_TIMEOUT)
		{
			continue;
		}
		else if (res != SCAP_SUCCESS && res != SCAP_TIMEOUT)
		{
			//
			// There was an error. Just stop here and log the error
			//
			job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
			job->m_lasterr = "apply_job_filter error reading events from file " +
			                 state->m_shm_name + ": " + inspector.getlasterr();
			ASSERT(false);
			return false;
		}

		if (job->m_start_time != 0 && ev->get_ts() < job->m_start_time)
		{
			continue;
		}

		// Not using sinsp_memory_dumper_job::dump() here,
		// because we know the start/stop time are within
		// range, have given the inspector a filter, and the
		// inspector has determined whether or not the event
		// qualifies.
		job->m_n_events++;
		job->m_dumper->dump(ev);
	}

	return true;
}

void sinsp_memory_dumper::apply_job_filter(const shared_ptr<sinsp_memory_dumper_state>& state,
                                           std::unique_ptr<sinsp_memory_dumper_job>& job,
                                           lazy_scoped_lock* membuf_mtx)
{
	if (!state->is_open() || state->m_dumper->written_events() == 0)
	{
		return;
	}

	// If the timerange of this memory buffer doesn't overlap with
	// the timerange of the job, return immediately
	if (job->m_start_time != 0 && state->m_end_ts < job->m_start_time)
	{
		return;
	}

	sinsp inspector;
	inspector.set_hostname_and_port_resolution_mode(false);
	inspector.set_internal_events_mode(true);

	// Open the shared memory segment again so we can read from
	// the beginning.
	int fd = shm_open(state->m_shm_name.c_str(), O_RDONLY, 0);
	if (fd == -1)
	{
		job->m_lasterr = "Could not open shared memory region " + state->m_shm_name +
		                 " for reading: " + strerror(errno);
		job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
		return;
	}

	// Flush state to disk so an inspector reading the
	// same shm file will have its initial state.
	state->flush();

	try
	{
		inspector.fdopen(fd);
	}
	catch (exception& e)
	{
		job->m_lasterr = "inspector could not open shared memory region. inspector_err=" +
		                 inspector.getlasterr() + " e=" + e.what() +
		                 " nevt=" + to_string((*m_active_state)->m_dumper->written_events());
		job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
		::close(fd);
		return;
	}

	if (job->m_filterstr != "")
	{
		inspector.set_filter(job->m_filterstr);
	}

	if (job->m_dumper == NULL)
	{
		job->m_dumper = new sinsp_dumper(&inspector);
		try
		{
			job->m_dumper->open(job->m_filename, false, true);
		}
		catch (exception& e)
		{
			job->m_lasterr = "inspector could not open dump file " + job->m_filename +
			                 ". inspector_err=" + inspector.getlasterr() + " e=" + e.what();
			job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
			inspector.close();
			::close(fd);
			return;
		}
	}

	if (!read_membuf_using_inspector(inspector, state, job))
	{
		inspector.close();
		return;
	}

	// Now check the offset and read again. If we're currently
	// reading the active state, lock the membuf mutex now, so no
	// additional events can again until unlocked (by the caller
	// of add_job()).
	{
		Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);
		if (membuf_mtx && (*m_active_state)->m_shm_name == state->m_shm_name)
		{
			LOG_DEBUG("memdumper: Approaching end of state=%s, locking membuf mutex",
			          state->m_shm_name.c_str());
			membuf_mtx->lock();
		}
	}

	if (!read_membuf_using_inspector(inspector, state, job))
	{
		// When returning a failure, don't keep the mutex locked.
		if (membuf_mtx)
		{
			membuf_mtx->unlock();
		}
		inspector.close();
		return;
	}

	inspector.close();
}

std::unique_ptr<sinsp_memory_dumper_job> sinsp_memory_dumper::add_job(uint64_t ts,
                                                      const std::string& filename,
                                                      const std::string& filter,
                                                      uint64_t delta_time_past_ns,
                                                      uint64_t delta_time_future_ns,
                                                      lazy_scoped_lock* membuf_mtx)
{
	struct timeval tm;
	gettimeofday(&tm, NULL);

	std::unique_ptr<sinsp_memory_dumper_job> job = make_unique<sinsp_memory_dumper_job>();

	job->m_start_time = delta_time_past_ns != 0 ? ts - delta_time_past_ns : 0;
	job->m_end_time = ts + delta_time_future_ns;
	job->m_filename = filename;

	if (filter != "")
	{
		job->m_filterstr = filter;

		try
		{
			sinsp_filter_compiler compiler(m_inspector, filter);
			job->m_filter = compiler.compile();
		}
		catch (exception& e)
		{
			job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
			job->m_lasterr = "error compiling capture job filter (" + filter + "). e=" + e.what();
			return job;
		}
	}

	{
		Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);
		m_reader_state = m_states.rbegin();
		m_reader_active = true;
	}

	while (m_reader_state != m_states.rend())
	{
		apply_job_filter(*m_reader_state, job, membuf_mtx);
		{
			Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);
			++m_reader_state;
		}
	}

	m_reader_active = false;

	// It's possible (although unlikely) that while reading
	// through the memory buffers it was necessary to create a
	// temporary third buffer. In this case, remove the oldest
	// buffer.
	{
		Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);
		while (m_states.size() > 2)
		{
			LOG_DEBUG("memdumper: Removing temporary additional state while reader was active");
			m_states.pop_back();
		}
	}

	// If process_event was waiting for a delayed state switch,
	// allow it now.
	if (m_delayed_switch_states_needed)
	{
		m_delayed_switch_states_ready = true;
	}

	//
	// If no capture in the future is required, the job can stop here
	//
	if (delta_time_future_ns == 0)
	{
		job->m_state = sinsp_memory_dumper_job::ST_DONE_OK;
	}

	return job;
}

void sinsp_memory_dumper::check_autodisable(uint64_t evt_ts_ns, uint64_t sys_ts_ns)
{
	if (m_autodisable)
	{
		if ((sys_ts_ns - m_last_switch_state_ns) < (m_min_time_between_switch_states_ns))
		{
			LOG_WARNING("sinsp_memory_dumper: min_time_between_switch_states_ms - current: %" PRIu64
			            " expected > %" PRIu64,
			            (sys_ts_ns - m_last_switch_state_ns) / 1000000,
			            m_min_time_between_switch_states_ns / 1000000);
			m_autodisable_threshold_reached_count++;
		}
		else if ((((m_dump_buffer_headers_size * 100) / (*m_active_state)->m_bufsize)) >
		         m_capture_headers_percentage_threshold)
		{
			LOG_WARNING(
			    "sinsp_memory_dumper: m_capture_headers_percentage_threshold - current: %" PRIu64
			    " expected < %" PRIu64,
			    (((m_dump_buffer_headers_size * 100) / (*m_active_state)->m_bufsize)),
			    m_capture_headers_percentage_threshold);
			m_autodisable_threshold_reached_count++;
		}
		else
		{
			m_autodisable_threshold_reached_count = 0;
		}

		if (m_autodisable_threshold_reached_count >= 10)
		{
			m_disabled = true;
			m_disabled_by_autodisable = true;
			// since the 'process_event' uses event timestamp to re-enable the memdumper
			// we want to avoid overflows due to evt_ts_ns < sys_ts_ns
			m_last_autodisable_ns = evt_ts_ns;
			m_autodisable_threshold_reached_count = 0;
			LOG_ERROR(
			    "sinsp_memory_dumper: disabling memdumper - too frequent switch_states detected");
		}
	}
}

void sinsp_memory_dumper::switch_states(uint64_t ts)
{
	uint64_t sys_ts_ns = sinsp_utils::get_current_time_ns();
	check_autodisable(ts, sys_ts_ns);

	m_last_switch_state_ns = sys_ts_ns;

	Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);

	LOG_DEBUG("memdumper: switching memory buffer states");

	// If a delayed switch was needed, it's no longer needed. Log
	// an error with the number of missed events.

	if (m_delayed_switch_states_needed)
	{
		LOG_WARNING("memdumper: missed %lu events waiting for new job creation to finish",
		            m_delayed_switch_states_missed_events);
		m_delayed_switch_states_needed = false;
		m_delayed_switch_states_ready = false;
		m_delayed_switch_states_missed_events = 0;
	}

	//
	// Save the capture to disk
	//
	if (m_cf)
	{
		//		flush_state_to_disk(m_cf, m_active_state, false);
		m_cur_dump_size += m_bsize;
		m_switches_to_go--;

		bool toobig = (m_cur_dump_size >= m_max_disk_size);

		if (m_switches_to_go == 0 || toobig)
		{
			fclose(m_cf);
			m_cf = NULL;

			if (toobig)
			{
				LOG_INFO("memdumper: dump closed because too big, m_max_disk_size=%d",
				         m_max_disk_size);
			}
			else
			{
				LOG_INFO("memdumper: dump closed");
			}
		}
	}

	// If a reader is going through the states, create a new state
	// and put it at the front. However, never create more than 3
	// states. If there are already 3 states, simply skip event
	// processing until the reader has read all the states and
	// brought the total down to 2.

	// Otherwise, take the last state and put it at the front.
	if (m_reader_active)
	{
		if (m_states.size() < 3)
		{
			LOG_DEBUG("memdumper: creating temporary additional state while reader is active");
			string name = "/dragent-memdumper-" + to_string(m_file_id++);
			m_states.emplace_front(
			    make_shared<sinsp_memory_dumper_state>(m_inspector, m_bsize, name));
		}
		else
		{
			LOG_WARNING("memdumper: stopping event processing while new job creation is active");
			m_delayed_switch_states_needed = true;
			m_delayed_switch_states_ready = false;
			m_delayed_switch_states_missed_events = 0;
		}
	}
	else
	{
		shared_ptr<sinsp_memory_dumper_state> st = m_states.back();
		m_states.pop_back();
		m_states.push_front(st);
	}

	m_active_state = m_states.begin();

	// Reopen the first state
	string errstr;
	if (!(*m_active_state)->open(errstr))
	{
		LOG_ERROR("memdumper: could not reopen swapped state: %s. Memory dump disabled",
		          errstr.c_str());
		m_disabled = true;
		return;
	}
}
