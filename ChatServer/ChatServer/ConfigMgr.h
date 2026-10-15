#pragma once
#include "const.h"

#include <map>
#include <string>

struct SectionInfo
{
	SectionInfo() {}
	~SectionInfo() { _section_datas.clear(); }

	SectionInfo(const SectionInfo& src) {
		_section_datas = src._section_datas;
	}

	SectionInfo& operator= (const SectionInfo& src)
	{
		if (&src == this) {
			return *this;
		}

		this->_section_datas = src._section_datas;
		return *this;
	}

	std::map<std::string, std::string> _section_datas;
	const std::string& operator[](const std::string& key) const {
		static const std::string empty;
		auto it = _section_datas.find(key);
		return it == _section_datas.end() ? empty : it->second;
	};
};
class ConfigMgr
{
public:
	~ConfigMgr() {
		_config_map.clear();
	}

	const SectionInfo& operator[](const std::string& section) const {
		static const SectionInfo empty;
		auto it = _config_map.find(section);
		return it == _config_map.end() ? empty : it->second;
	}

	static ConfigMgr& Inst() {
		static ConfigMgr cfg_mgr;
		return cfg_mgr;
	}

	ConfigMgr(const ConfigMgr& src) {
		_config_map = src._config_map;
	}

	ConfigMgr& operator= (const ConfigMgr& src) {
		if (&src == this) {
			return *this;
		}

		_config_map = src._config_map;
		return *this;
	}
private:
	ConfigMgr();

	std::map<std::string, SectionInfo> _config_map;
};

