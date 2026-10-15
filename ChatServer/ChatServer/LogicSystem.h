#pragma once
#include "const.h"
#include "Singleton.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

class CSession;

// 队列里的一条消息: 谁发的 + 消息号 + body
struct MsgInfo
{
	MsgInfo(std::shared_ptr<CSession> session, unsigned short msgid, std::string body)
		: _session(std::move(session)), _msgid(msgid), _body(std::move(body)) {
	}

	std::shared_ptr<CSession> _session;
	unsigned short _msgid;
	std::string _body;
};

class LogicSystem : public Singleton<LogicSystem>
{
	friend class Singleton<LogicSystem>;
public:
	~LogicSystem();

	// io 线程只调用这一个函数: 入队就返回, 绝不在这里做业务
	void PostMsgToQue(std::shared_ptr<CSession> session, unsigned short msgid, std::string body);

private:
	LogicSystem();
	void DealMsg();                       // 逻辑线程的函数体
	void RegisterCallBacks();             // 消息号 -> 处理函数
	void LoginHandler(std::shared_ptr<CSession> session,
		const unsigned short& msgid, const std::string& body);
	void SearchHandler(std::shared_ptr<CSession> session,
		const unsigned short& msgid, const std::string& body);
	void AddFriendApplyHandler(std::shared_ptr<CSession> session,
		const unsigned short& msgid, const std::string& body);
	void AuthFriendHandler(std::shared_ptr<CSession> session,
		const unsigned short& msgid, const std::string& body);
	void LoadChatThreadHandler(std::shared_ptr<CSession> session,
		const unsigned short& msgid, const std::string& body);
	void SendChatMsgHandler(std::shared_ptr<CSession> session,
		const unsigned short& msgid, const std::string& body);
	void LoadChatMsgHandler(std::shared_ptr<CSession> session,
		const unsigned short& msgid, const std::string& body);
	void DeliverOfflineMsgs(std::shared_ptr<CSession> session, int uid);

	std::thread _worker_thread;
	std::queue<std::shared_ptr<MsgInfo>> _msg_que;
	std::mutex _mutex;
	std::condition_variable _consume;     // 队列空时让逻辑线程睡, 不空转烧 CPU
	std::atomic<bool> _b_stop;
	std::map<unsigned short,
		std::function<void(std::shared_ptr<CSession>, const unsigned short&, const std::string&)>> _fun_callbacks;
};