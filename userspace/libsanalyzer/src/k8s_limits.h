#pragma once

#include "filter_limits.h"

namespace draiosproto {
class container_group;
}

class k8s_limits : public user_configured_limits
{
public:
	using sptr_t = std::shared_ptr<k8s_limits>;

	explicit k8s_limits(filter_vec_t&& filters,
		   uint32_t max_entries = user_configured_limits::MAX_ENTRIES,
		   uint64_t expire_seconds = user_configured_limits::EXPIRE_SECONDS);

	k8s_limits();

	static sptr_t build(filter_vec_t filters,
			   bool log_enabled,
			   uint32_t max_entries = user_configured_limits::MAX_ENTRIES,
			   uint64_t expire_seconds = user_configured_limits::EXPIRE_SECONDS);

	void purge_tags(draiosproto::container_group& congroup);

	DEFINE_LOG("K8S");
	DEFINE_LOG_ENABLED(k8s_limits);
private:
	bool m_filter_vec_is_empty;
};