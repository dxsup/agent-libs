#include "analyzer_thread.h"
#include "process_manager.h"

#include <gtest.h>

class test_helper
{
public:
	static void insert_app_check(THREAD_TYPE* ainfo, std::string value)
	{
		GET_AGENT_THREAD(ainfo)->m_app_checks_found.insert(value);
	}
};

TEST(process_manager_test, app_checks_always_send_config)
{
	std::string some_yaml = R"(
process:
  flush_filter:
    - include:
        all

app_checks_always_send: true
)";
	yaml_configuration config_yaml(some_yaml);
	ASSERT_EQ(0, config_yaml.errors().size());
	process_manager::c_always_send_app_checks.init(config_yaml);
	process_manager::c_process_filter.init(config_yaml);

	process_manager my_manager;

#ifdef USE_AGENT_THREAD
	thread_analyzer_info tinfo(nullptr, nullptr);
#else
	sinsp_threadinfo tinfo;
	tinfo.m_ainfo = new thread_analyzer_info(nullptr, nullptr);
#endif
	bool matches = false;
	bool generic_match = false;
	matches =
	    my_manager.get_flush_filter().matches(NULL, &tinfo, NULL, NULL, &generic_match, NULL, NULL);
	EXPECT_EQ(matches, true);
	EXPECT_EQ(generic_match, true);

	test_helper::insert_app_check(&tinfo, "some app check");
	matches =
	    my_manager.get_flush_filter().matches(NULL, &tinfo, NULL, NULL, &generic_match, NULL, NULL);
	EXPECT_EQ(matches, true);
	EXPECT_EQ(generic_match, false);

#ifndef USE_AGENT_THREAD
	delete tinfo.m_ainfo;
#endif
}