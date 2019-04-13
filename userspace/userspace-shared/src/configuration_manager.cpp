#include "configuration_manager.h"
#include <assert.h>
#include <Poco/NumberFormatter.h>

std::map<std::string, configuration_unit*>* configuration_manager::config_map;

void configuration_manager::init_config(const yaml_configuration& raw_config)
{
	for (const auto& config : get_map())
	{
		config.second->init(raw_config);
	}
}

void configuration_manager::print_config(const log_delegate& logger)
{
	for (const auto& config : get_map())
	{
		logger(config.second->to_string());
	}
}

void configuration_manager::register_config(configuration_unit* config)
{
	if (config == nullptr || config->get_key_string().size() == 0)
	{
		assert(false);
		return;
	}

	if (get_map().find(config->get_key_string()) != get_map().end())
	{
		assert(false);
		return;
	}

	get_map().emplace(config->get_key_string(), config);
}

std::map<std::string, configuration_unit*>& configuration_manager::get_map()
{
	if (config_map == nullptr)
	{
		config_map = new std::map<std::string, configuration_unit*>();
	}

	return *config_map;
}


template<>
std::string type_config<bool>::to_string() const
{
	return get_key_string() + ": " + (m_data ? "true" : "false");
}

template<>
std::string type_config<uint64_t>::to_string() const
{
	return get_key_string() + ": " + Poco::NumberFormatter::format(m_data);
}

configuration_unit::configuration_unit(const std::string& key,
				       const std::string& subkey,
				       const std::string& subsubkey,
				       const std::string& description)
	:m_key(key),
	 m_subkey(subkey),
	 m_subsubkey(subsubkey),
	 m_description(description)
{
	if (m_subkey.empty())
	{
		m_keystring = m_key;
	}
	else if (m_subsubkey.empty())
	{
		m_keystring = m_key + "." + m_subkey;
	}
	else
	{
		m_keystring = m_key + "." + m_subkey + "." + m_subsubkey;
	}

	configuration_manager::register_config(this);
}

const std::string& configuration_unit::get_key_string() const
{
	return m_keystring;
}
