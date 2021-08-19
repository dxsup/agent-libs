#include "draios.pb.h"
#include "libsanalyzer/src/internal_metrics.h"

#include "Poco/Message.h"

#include <gtest.h>
#include <iostream>
#include <string>

#include <google/protobuf/util/message_differencer.h>
using namespace Poco;
using Poco::Message;

const char* allstr();
const char* logstr();

class test_helper
{
public:
	static bool send_all(internal_metrics& im,
	                     draiosproto::statsd_info* statsd_info,
	                     uint64_t max_queue_size)
	{
		return im.send_all(statsd_info, max_queue_size);
	}
};


// Test fixture
class internal_metrics_test : public ::testing::Test
{
protected:
	draiosproto::statsd_info expected;
	internal_metrics im;

	virtual void SetUp()
	{
		fill_expected();
	}
	virtual void TearDown() {}

	void fill_expected(bool all=false)
	{
		expected.Clear();
		decltype(expected.mutable_statsd_metrics()->Add()) new_metric;

		if(all)
		{
#define SOME(list)
#define ALL(list) list
#include "expected.inc"
		}
		else
		{
#define SOME(list) list
#define ALL(list)
#include "expected.inc"
		}

		new_metric = expected.mutable_statsd_metrics()->Add();
		fill_system_info(*new_metric);

	}

	void fill_system_info(draiosproto::statsd_metric& metric)
	{
		metric.set_name("host.uname");
		metric.set_type(draiosproto::STATSD_GAUGE);
		metric.set_value(1);
		for(const auto& tag : im.get_system_info())
		{
			auto* newtag = metric.mutable_tags()->Add();
			newtag->set_key(tag.first);
			newtag->set_value(tag.second);
		}
	}
};


TEST_F(internal_metrics_test, metrics)
{
	ASSERT_EQ(0u, im.logs());

	ASSERT_EQ(-1, im.get_process());
	ASSERT_EQ(-1, im.get_thread());
	ASSERT_EQ(-1, im.get_container());
	ASSERT_EQ(-1, im.get_javaproc());
	ASSERT_EQ(-1, im.get_appcheck());
	ASSERT_FALSE(im.get_mesos_autodetect());
	ASSERT_FALSE(im.get_mesos_detected());
	ASSERT_EQ(-1, im.get_fp());
	ASSERT_EQ(-1, im.get_fl());
	ASSERT_EQ(-1, im.get_sr());
	ASSERT_EQ(-1, im.get_analyzer_cpu_percentage());

	ASSERT_EQ(-1, im.get_n_evts());
	ASSERT_EQ(-1, im.get_n_drops());
	ASSERT_EQ(-1, im.get_n_drops_buffer());
	ASSERT_EQ(-1, im.get_n_preemptions());

	ASSERT_EQ(-1, im.get_agent_cpu());
	ASSERT_EQ(-1, im.get_agent_memory());
	ASSERT_EQ(-1, im.get_java_cpu());
	ASSERT_EQ(-1, im.get_java_memory());
	ASSERT_EQ(-1, im.get_appcheck_cpu());
	ASSERT_EQ(-1, im.get_appcheck_memory());
	ASSERT_EQ(-1, im.get_mountedfs_reader_cpu());
	ASSERT_EQ(-1, im.get_mountedfs_reader_memory());
	ASSERT_EQ(-1, im.get_statsite_forwarder_cpu());
	ASSERT_EQ(-1, im.get_statsite_forwarder_memory());
	ASSERT_EQ(-1, im.get_cointerface_cpu());
	ASSERT_EQ(-1, im.get_cointerface_memory());

	Message msg;
	msg.setPriority(Message::PRIO_FATAL);
	im.notify(msg.getPriority());
	msg.setPriority(Message::PRIO_CRITICAL);
	im.notify(msg.getPriority());
	msg.setPriority(Message::PRIO_ERROR);
	im.notify(msg.getPriority());
	msg.setPriority(Message::PRIO_WARNING);
	im.notify(msg.getPriority());
	msg.setPriority(Message::PRIO_NOTICE);
	im.notify(msg.getPriority());
	msg.setPriority(Message::PRIO_INFORMATION);
	im.notify(msg.getPriority());
	msg.setPriority(Message::PRIO_DEBUG);
	im.notify(msg.getPriority());
	msg.setPriority(Message::PRIO_TRACE);
	im.notify(msg.getPriority());
	EXPECT_EQ(4u, im.logs());

	draiosproto::statsd_info info;
	ASSERT_TRUE(test_helper::send_all(im, &info, 0));

	EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(info, expected))
		<< "Info:\n" << info.DebugString()
		<< "\n\nExpected:\n" << expected.DebugString();

	EXPECT_EQ(0u, im.logs());

	im.set_process(999);
	im.set_thread(999);
	im.set_container(999);
	im.set_javaproc(999);
	im.set_appcheck(999);
	im.set_mesos_autodetect(true);
	im.set_mesos_detected(true);
	im.set_fp(999);
	im.set_fl(999);
	im.set_sr(999);
	im.set_analyzer_cpu_percentage(999);

	im.set_n_evts(999);
	im.set_n_drops(998);
	im.set_n_drops_buffer(997);
	im.set_n_preemptions(996);

	EXPECT_EQ(999, im.get_process());
	EXPECT_EQ(999, im.get_thread());
	EXPECT_EQ(999, im.get_container());
	EXPECT_EQ(999, im.get_javaproc());
	EXPECT_EQ(999, im.get_appcheck());

	EXPECT_TRUE(im.get_mesos_autodetect());
	EXPECT_TRUE(im.get_mesos_detected());
	EXPECT_EQ(999, im.get_fp());
	EXPECT_EQ(999, im.get_fl());
	EXPECT_EQ(999, im.get_sr());
	EXPECT_EQ(999, im.get_analyzer_cpu_percentage());

	EXPECT_EQ(999, im.get_n_evts());
	EXPECT_EQ(998, im.get_n_drops());
	EXPECT_EQ(997, im.get_n_drops_buffer());
	EXPECT_EQ(996, im.get_n_preemptions());

	im.set_agent_cpu(999);
	im.set_agent_memory(999);
	im.set_java_cpu(999);
	im.set_java_memory(999);
	im.set_appcheck_cpu(999);
	im.set_appcheck_memory(999);
	im.set_mountedfs_reader_cpu(999);
	im.set_mountedfs_reader_memory(999);
	im.set_statsite_forwarder_cpu(999);
	im.set_statsite_forwarder_memory(999);
	im.set_cointerface_cpu(999);
	im.set_cointerface_memory(999);

	EXPECT_EQ(999, im.get_agent_cpu());
	EXPECT_EQ(999, im.get_agent_memory());
	EXPECT_EQ(999, im.get_java_cpu());
	EXPECT_EQ(999, im.get_java_memory());
	EXPECT_EQ(999, im.get_appcheck_cpu());
	EXPECT_EQ(999, im.get_appcheck_memory());
	EXPECT_EQ(999, im.get_mountedfs_reader_cpu());
	EXPECT_EQ(999, im.get_mountedfs_reader_memory());
	EXPECT_EQ(999, im.get_statsite_forwarder_cpu());
	EXPECT_EQ(999, im.get_statsite_forwarder_memory());
	EXPECT_EQ(999, im.get_cointerface_cpu());
	EXPECT_EQ(999, im.get_cointerface_memory());

	info.Clear();
	fill_expected(true);
	ASSERT_TRUE(test_helper::send_all(im, &info, 995));
	EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(info, expected))
		<< "Info:\n" << info.DebugString()
		<< "\n\nExpected:\n" << expected.DebugString();

	EXPECT_EQ(0u, im.logs());

	// gauges remain intact after send
	EXPECT_EQ(999, im.get_process());
	EXPECT_EQ(999, im.get_thread());
	EXPECT_EQ(999, im.get_container());
	EXPECT_EQ(999, im.get_javaproc());
	EXPECT_EQ(999, im.get_appcheck());
	EXPECT_TRUE(im.get_mesos_autodetect());
	EXPECT_TRUE(im.get_mesos_detected());
	EXPECT_EQ(999, im.get_fp());
	EXPECT_EQ(999, im.get_fl());
	EXPECT_EQ(999, im.get_sr());
	EXPECT_EQ(999, im.get_analyzer_cpu_percentage());

	EXPECT_EQ(999, im.get_n_evts());
	EXPECT_EQ(998, im.get_n_drops());
	EXPECT_EQ(997, im.get_n_drops_buffer());
	EXPECT_EQ(996, im.get_n_preemptions());

	EXPECT_EQ(999, im.get_agent_cpu());
	EXPECT_EQ(999, im.get_agent_memory());
	EXPECT_EQ(999, im.get_java_cpu());
	EXPECT_EQ(999, im.get_java_memory());
	EXPECT_EQ(999, im.get_appcheck_cpu());
	EXPECT_EQ(999, im.get_appcheck_memory());
	EXPECT_EQ(999, im.get_mountedfs_reader_cpu());
	EXPECT_EQ(999, im.get_mountedfs_reader_memory());
	EXPECT_EQ(999, im.get_statsite_forwarder_cpu());
	EXPECT_EQ(999, im.get_statsite_forwarder_memory());
	EXPECT_EQ(999, im.get_cointerface_cpu());
	EXPECT_EQ(999, im.get_cointerface_memory());

	info.Clear();
	draiosproto::statsd_metric* metric =
	    im.write_metric(&info, "xyz", draiosproto::STATSD_GAUGE, -1);
	EXPECT_EQ(nullptr, metric);
	metric = im.write_metric(&info, "xyz", draiosproto::STATSD_GAUGE, 1);
	ASSERT_FALSE(nullptr == metric);
	const char* mstr = "name: \"xyz\"\ntype: STATSD_GAUGE\nvalue: 1\n";
	EXPECT_EQ(metric->DebugString(), mstr);
}