#include "ConfigMgr.h"

#include <iostream>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
ConfigMgr::ConfigMgr()
{
	try
	{
		auto exe_dir = boost::dll::program_location().parent_path();
		boost::filesystem::path config_path = exe_dir / "config.ini";
		std::cout << "Config path: " << config_path << std::endl;

		boost::property_tree::ptree pt;
		boost::property_tree::read_ini(config_path.string(), pt);

		for (const auto& section_pair : pt) {
			const ::std::string& section_name = section_pair.first;
			const boost::property_tree::ptree& section_tree = section_pair.second;
			std::map<std::string, std::string> secton_config;
			for (const auto& key_value_pair : section_tree) {
				const std::string& key = key_value_pair.first;
				const std::string& value = key_value_pair.second.get_value<std::string>();
				secton_config[key] = value;
			}

			SectionInfo sectionInfo;
			sectionInfo._section_datas = secton_config;
			_config_map[section_name] = sectionInfo;
		}

		// 输出所有的section和key-value对
		for (const auto& section_entry : _config_map) {
			const std::string& section_name = section_entry.first;
			SectionInfo section_config = section_entry.second;
			std::cout << "[" << section_name << "]" << std::endl;
			for (const auto& key_value_pair : section_config._section_datas) {
				std::cout << key_value_pair.first << "=" << key_value_pair.second << std::endl;
			}
		}
	}
	catch (const std::exception& e)
	{
		std::cerr << "read config failed: " << e.what() << std::endl;
	}
	
}

