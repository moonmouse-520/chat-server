#pragma once
#include "const.h"
#include "Singleton.h"
#include "CSession.h"

#include <iostream>
#include <memory>
#include <mutex>
#include <unordered_map>

class UserMgr : public Singleton<UserMgr>
{
	friend class Singleton<UserMgr>;
public:
	~UserMgr() = default;
	void SetUserSession(int uid, std::shared_ptr<CSession> session) {
		std::lock_guard<std::mutex> lock(_session_mutex);
		_uid_to_session[uid] = session;
		std::cout << "[usermgr] bind uid:" << uid << " session:" << session->GetSessionId() << std::endl;
	}
	
	std::shared_ptr<CSession> GetUser(int uid) {
		std::lock_guard<std::mutex> lock(_session_mutex);
		auto it = _uid_to_session.find(uid);
		return it == _uid_to_session.end() ? nullptr : it->second;
	}

	void RemoveUser(int uid) {
		std::lock_guard<std::mutex> lock(_session_mutex);
		auto it = _uid_to_session.find(uid);
		if (it == _uid_to_session.end()) {
			return;
		}

		_uid_to_session.erase(it);
		std::cout << "[usermgr] unbind uid=" << uid << std::endl;
	}
private:
	UserMgr() = default;
	std::unordered_map<int, std::shared_ptr<CSession>> _uid_to_session;
	std::mutex _session_mutex;
};