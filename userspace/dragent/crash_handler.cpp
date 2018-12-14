#include "crash_handler.h"

#ifndef CYGWING_AGENT
#include <execinfo.h>
#endif

#include "logger.h"
#include "sinsp_worker.h"

DRAGENT_LOGGER();

string crash_handler::m_crashdump_file;
const sinsp_worker* crash_handler::m_sinsp_worker = NULL;

static const int g_crash_signals[] = 
{
	SIGSEGV,
	SIGABRT,
	SIGFPE,
	SIGILL,
	SIGBUS
};

void crash_handler::run(int sig)
{
	if(g_log)
	{
		int fd = open(m_crashdump_file.c_str(), O_WRONLY|O_APPEND);
		if(fd != -1)
		{
			char line[128];
			snprintf(line, sizeof(line), "Received signal %d\n", sig);
			log_crashdump_message(fd, line);

#ifndef CYGWING_AGENT
			void *array[NUM_FRAMES];
			int frames = backtrace(array, NUM_FRAMES);

			snprintf(line, sizeof(line), "Backtrace frames: %d\n", frames);
			log_crashdump_message(fd, line);

			backtrace_symbols_fd(array, frames, fd);
#endif

			if(m_sinsp_worker && m_sinsp_worker->get_last_loop_ns())
			{
				log_crashdump_message(fd, "Memory report:\n");

				char buf[1024];
				m_sinsp_worker->get_inspector()->m_analyzer->generate_memory_report(buf, sizeof(buf));
				log_crashdump_message(fd, buf);
			}

			close(fd);

#ifndef CYGWING_AGENT
			backtrace_symbols_fd(array, frames, 1);
#endif
		}
		else
		{
			ASSERT(false);
		}
	}

	signal(sig, SIG_DFL);
	raise(sig);
}

void crash_handler::log_crashdump_message(const char* message)
{
	size_t len;
	size_t mlen = strlen(message);

	int fd = open(m_crashdump_file.c_str(), O_WRONLY|O_APPEND);
	if(fd != -1)
	{
		if ((len = write(fd, message, mlen)) != mlen)
		{
			if(len == -1)
			{
				LOG_ERROR("Could not write crash dump message: %s", strerror(errno));
			}
			else
			{
				LOG_ERROR("Incomplete write when writing crash dump message (%lu of %lu written)", len, mlen);
			}
		}
		close(fd);
	}
	else
	{
		ASSERT(false);
	}

	if ((len = write(1, message, mlen)) != mlen)
	{
		if(len == -1)
		{
			LOG_ERROR("Could not write crash dump message: %s", strerror(errno));
		}
		else
		{
			LOG_ERROR("Incomplete write when writing crash dump message (%lu of %lu written)", len, mlen);
		}
	}

	close(fd);
}

void crash_handler::log_crashdump_message(int fd, const char* message)
{
	size_t len;
	size_t mlen = strlen(message);

	// We expect fd to represent a file as compared to a network
	// connection, where an incomplete write might occur due to,
	// say, a socket buffer being full. So all incomplete writes
	// are considered errors.
	if ((len = write(fd, message, strlen(message))) != mlen)
	{
		if(len == -1)
		{
			LOG_ERROR("Could not write crash dump message: %s", strerror(errno));
		}
		else
		{
			LOG_ERROR("Incomplete write when writing crash dump message (%lu of %lu written)", len, mlen);
		}
	}
}

bool crash_handler::initialize()
{
	stack_t stack;

	memset(&stack, 0, sizeof(stack));
	stack.ss_sp = malloc(SIGSTKSZ);
	stack.ss_size = SIGSTKSZ;

	if(sigaltstack(&stack, NULL) == -1)
	{
		free(stack.ss_sp);
		return false;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);

	for(uint32_t j = 0; j < sizeof(g_crash_signals) / sizeof(g_crash_signals[0]); ++j)
	{
		sigaddset(&sa.sa_mask, g_crash_signals[j]);
	}

	sa.sa_handler = crash_handler::run;
	sa.sa_flags = SA_ONSTACK;

	for(uint32_t j = 0; j < sizeof(g_crash_signals) / sizeof(g_crash_signals[0]); ++j)
	{
		if(sigaction(g_crash_signals[j], &sa, NULL) != 0)
		{
			return false;
		}
	}

	void *array[NUM_FRAMES];
#ifndef CYGWING_AGENT
	backtrace(array, NUM_FRAMES);
#endif

	return true;
}