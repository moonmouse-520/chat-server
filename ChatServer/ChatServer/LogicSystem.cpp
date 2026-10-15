#include "LogicSystem.h"
#include "CSession.h"
#include "UserMgr.h"
#include <json.hpp>         
#include <iostream>
#include "MysqlMgr.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <thread>
LogicSystem::LogicSystem()
	: _b_stop(false)
{
	RegisterCallBacks();
	_worker_thread = std::thread(&LogicSystem::DealMsg, this);   // 构造即起线程
}

LogicSystem::~LogicSystem()
{
	_b_stop = true;
	_consume.notify_all();                    // 叫醒还在 wait 的线程
	if (_worker_thread.joinable())
	{
		_worker_thread.join();
	}
	std::cout << "[logic] worker thread joined" << std::endl;
}

void LogicSystem::RegisterCallBacks()
{
	// 消息号 -> 处理函数
	_fun_callbacks.insert(std::make_pair(
		MSG_CHAT_LOGIN,
		std::bind(&LogicSystem::LoginHandler, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
	_fun_callbacks.insert(std::make_pair(
		ID_SEARCH_USER_REQ,
		std::bind(&LogicSystem::SearchHandler,this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
	_fun_callbacks.insert(std::make_pair(
		ID_ADD_FRIEND_REQ,
		std::bind(&LogicSystem::AddFriendApplyHandler, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
	_fun_callbacks.insert(std::make_pair(
		ID_AUTH_FRIEND_REQ, 
		std::bind(&LogicSystem::AuthFriendHandler, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
	_fun_callbacks.insert(std::make_pair(
		ID_LOAD_CHAT_THREAD_REQ,
		std::bind(&LogicSystem::LoadChatThreadHandler, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
	_fun_callbacks.insert(std::make_pair(
		ID_TEXT_CHAT_MSG_REQ,
		std::bind(&LogicSystem::SendChatMsgHandler, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
	_fun_callbacks.insert(std::make_pair(
		ID_LOAD_CHAT_MSG_REQ,
		std::bind(&LogicSystem::LoadChatMsgHandler, this,
			std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
}

void LogicSystem::PostMsgToQue(std::shared_ptr<CSession> session, unsigned short msgid, std::string body)
{
	std::unique_lock<std::mutex> lock(_mutex);

	if (_msg_que.size() >= MAX_RECVQUE)
	{
		// 队列满 = 逻辑线程已经跟不上生产速度。再塞只会把内存吃光, 所以丢弃并记账
		std::cout << "[logic] recv queue full(" << MAX_RECVQUE << "), drop msgid=" << msgid << std::endl;
		return;
	}

	_msg_que.push(std::make_shared<MsgInfo>(session, msgid, std::move(body)));
	lock.unlock();                 // 先解锁再 notify, 少一次"被叫醒后抢不到锁"的无谓唤醒
	_consume.notify_one();
}

void LogicSystem::DealMsg()
{
	std::cout << "[logic] worker thread start, tid=" << std::this_thread::get_id() << std::endl;

	for (;;)
	{
		std::unique_lock<std::mutex> lock(_mutex);

		// 谓词版 wait: 被叫醒后自己再确认一次条件, 免疫"虚假唤醒"
		_consume.wait(lock, [this] { return _b_stop || !_msg_que.empty(); });

		if (_b_stop && _msg_que.empty())
		{
			break;                 // 要停机了, 但先把队列里剩的处理完(不丢消息)
		}
		if (_msg_que.empty())
		{
			continue;              // 防御: 谓词已经保证非空
		}

		auto msg = _msg_que.front();
		_msg_que.pop();
		lock.unlock();             //  出锁再处理: 业务里可能有慢查询, 持锁跑会把 io 线程堵在入队上

		auto it = _fun_callbacks.find(msg->_msgid);
		if (it == _fun_callbacks.end())
		{
			std::cout << "[logic] no handler for msgid=" << msg->_msgid << std::endl;
			continue;
		}

		try {
			it->second(msg->_session, msg->_msgid, msg->_body);
		}
		catch (const std::exception& e) {
			// 一个 handler 崩了不能带走整个服务: 记下来, 继续处理下一条消息
			std::cout << "[logic] handler threw: " << e.what()
				<< " (msgid=" << msg->_msgid << ")" << std::endl;
		}
	}

	std::cout << "[logic] worker thread exit" << std::endl;
}

void LogicSystem::LoginHandler(std::shared_ptr<CSession> session,
	const unsigned short& msgid, const std::string& body)
{
	// 客户端发来的是 QJsonDocument::Indented 序列化的 JSON:
	//     {\n    "uid": 1002,\n    "token": "xxx"\n}\n     
	nlohmann::json parsed;
	try
	{
		parsed = nlohmann::json::parse(body);
	}
	catch (const std::exception& e)
	{
		std::cout << "[logic] login: bad json: " << e.what() << std::endl;
		nlohmann::json rsp;
		rsp["error"] = Error_Json;                 // 1001
		session->Send(rsp.dump(), static_cast<unsigned short>(MSG_CHAT_LOGIN_RSP));
		return;
	}

	if (!parsed.contains("uid") || !parsed.contains("token"))
	{
		std::cout << "[logic] login: missing field" << std::endl;
		nlohmann::json rsp;
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(MSG_CHAT_LOGIN_RSP));
		return;
	}

	int uid = parsed["uid"].get<int>();
	std::string token = parsed["token"].get<std::string>();

	// 打印线程 id: 这一行和 [session] recv 那行会出现在不同线程上, 是"业务已切到逻辑线程"的证据
	std::cout << "[logic] login uid=" << uid << " token=" << token
		<< " session=" << session->GetSessionId()
		<< " tid=" << std::this_thread::get_id() << std::endl;

	// 握手完成 = 这条连接从此代表这个用户
	session->SetUserId(uid);

	// 记进 UserMgr, 以后给别人发消息/通知下线都靠它找连接
	//    真实项目这里还应该做两件事(留到后面):
	//      · 校验 token(Redis 里 utoken_<uid> 是否还在)
	//      · 同一 uid 重复登录 → 给旧连接发 ID_NOTIFY_OFF_LINE_REQ(1021) 把它踢下线
	UserMgr::GetInstance()->SetUserSession(uid, session);

	// 回包。客户端只检查 error, 为 0 就切到聊天界面
	nlohmann::json rsp;
	rsp["error"] = 0;
	rsp["uid"] = uid;
	rsp["token"] = token;
	// name 先不回: 客户端 UserMgr 里 SetName 那几行还是注释状态, 等做好友列表时再从 MySQL 取
	session->Send(rsp.dump(), static_cast<unsigned short>(MSG_CHAT_LOGIN_RSP));		// 回包。客户端只检查 error, 为 0 就切到聊天界面
	//   先发 1006 再补投: 客户端拿到 1006 才会切到聊天界面并注册好 1019 的处理器,
	//   顺序反了的话, 离线消息可能在客户端还没准备好时就到了。
	DeliverOfflineMsgs(session, uid);
}

void LogicSystem::SearchHandler(std::shared_ptr<CSession> session, const unsigned short& msgid, const std::string& body)
{
	nlohmann::json parse;
	try
	{
		parse = nlohmann::json::parse(body);
	}
	catch (const std::exception& e)
	{
		std::cout << "[logic] search: bad json: " << e.what() << std::endl;
		nlohmann::json rsp;
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_SEARCH_USER_RSP));
		return;
	}

	if (!parse.contains("uid") || !parse.contains("searchkey")) {
		nlohmann::json rsp;
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_SEARCH_USER_RSP));
		return;
	}

	int self_uid = parse["uid"].get<int>();
	std::string search_key = parse["searchkey"].get<std::string>();

	bool is_digit = !search_key.empty() &&
		std::all_of(search_key.begin(), search_key.end(), 
			[](unsigned char c) {
				return std::isdigit(c) != 0;
			});

	UserInfo userInfo;
	bool found = (is_digit) ?
		MysqlMgr::GetInstance()->GetUser(std::atoi(search_key.c_str()), userInfo) :
		MysqlMgr::GetInstance()->GetUser(search_key, userInfo);

	std::cout << "[logic] search key=" << search_key << " by uid=" << self_uid
			  << " tid=" << std::this_thread::get_id()
			  << (found ? " -> found" : " -> not found") << std::endl;

	nlohmann::json rsp;
	if (!found) {
		rsp["error"] = ErrorCodes::UidInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_SEARCH_USER_RSP));
		return;
	}

	rsp["error"] = 0;
	rsp["uid"] = userInfo.uid;
	rsp["name"] = userInfo.name;
	rsp["nick"] = userInfo.nick;
	rsp["desc"] = userInfo.desc;
	rsp["sex"] = userInfo.sex;
	rsp["icon"] = userInfo.icon;

	session->Send(rsp.dump(), static_cast<unsigned short>(ID_SEARCH_USER_RSP));
}

void LogicSystem::AddFriendApplyHandler(std::shared_ptr<CSession> session, const unsigned short& msgid, const std::string& body)
{
	//确定申请人设计: from_uid 从【session 上取】(登录时绑的), 不读客户端 JSON 里的 uid
	int from_uid = session->GetUserId();
	nlohmann::json rsp;
	if (from_uid == 0) {
		//没登录就申请, 身份不可信
		std::cout << "[logic] addfriend from unlogined session " << session->GetSessionId() << std::endl;
		rsp["error"] = TokenInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_ADD_FRIEND_RSP));
		return;
	}
	nlohmann::json parsed;
	try
	{
		parsed = nlohmann::json::parse(body);
	}
	catch (const std::exception& e)
	{
		std::cout << "[logic] addfriend: bad json: " << e.what() << std::endl;
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_ADD_FRIEND_RSP));
		return;
	}
	if (!parsed.contains("applyuid")) {
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_ADD_FRIEND_RSP));
		return;
	}

	int to_uid = parsed["applyuid"].get<int>();
	// ★ 可选字段(教程里的字段名):
	//   bakname = 我给"对方"起的备注名(客户端会先用对方名字预填)
	//   desc    = 申请附言(申请时写的那句话)
	//   两个都可以不传 —— 备注空了服务端会用对方昵称兜底(见下面)
	std::string back_name = parsed.value("bakname", std::string());
	std::string apply_desc = parsed.value("desc", std::string());
	//不能加自己
	if (from_uid == to_uid) {
		rsp["error"] = UidInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_ADD_FRIEND_RSP));
		return;
	}

	//目标必须真实存在, 否则 friend_apply 里会留下指向空气的记录
	UserInfo target;
	if (!MysqlMgr::GetInstance()->GetUser(to_uid, target)) {
		std::cout << "[logic] addfriend: target_uid:" << to_uid << " not exist" << std::endl;
		rsp["error"] = UidInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_ADD_FRIEND_RSP));
		return;
	}

	if (back_name.empty()) {
		// 客户端没填备注: 用对方昵称(没昵称就用用户名)当默认备注,
		// 否则"备注优先"的显示逻辑会因为空串退化成"没有备注"
		back_name = target.nick.empty() ? target.name : target.nick;
	}

	//落库
	int ret = MysqlMgr::GetInstance()->AddFriendApply(from_uid, to_uid, apply_desc, back_name);
	std::cout << "[logic] addfriend " << from_uid << " -> " << to_uid
		<< " ret=" << ret << " tid=" << std::this_thread::get_id() << std::endl;

	switch (ret)
	{
	case 0: rsp["error"] = 0; break;
	case 1: rsp["error"] = ApplyDuplicate; break;
	case 2: rsp["error"] = AlreadyFriend; break;
	default:rsp["error"] = UidInvalid; break;
	}
	session->Send(rsp.dump(), static_cast<unsigned short>(ID_ADD_FRIEND_RSP));

	if (ret != 0) return; //失败(重复/已是好友), 就不用通知对方了

	// ============ 跨用户推送: 对方在线才推 ============
	// 这一步是 UserMgr 存在的全部意义: 从 uid 找到"他当前的连接"
	auto to_session = UserMgr::GetInstance()->GetUser(to_uid);
	if (to_session == nullptr) {
		// 离线是常态, 不是错误: 申请已经落库, 等他登录后拉取
		std::cout << "[logic] uid " << to_uid << " offline, apply saved to db only" << std::endl;
		return;
	}

	UserInfo applyer;
	if (!MysqlMgr::GetInstance()->GetUser(from_uid, applyer)) {
		return;
	}

	nlohmann::json notify;
	notify["error"] = 0;
	notify["applyuid"] = from_uid;
	notify["name"] = applyer.name;
	notify["nick"] = applyer.nick;
	notify["desc"] = applyer.desc;
	notify["sex"] = applyer.sex;
	notify["icon"] = applyer.icon;

	to_session->Send(notify.dump(), static_cast<unsigned short>(ID_NOTIFY_ADD_FRIEND_REQ));
	std::cout << "[logic] notify uid " << to_uid << " (session "
		<< to_session->GetSessionId() << ") apply from " << from_uid << std::endl;
}

void LogicSystem::AuthFriendHandler(std::shared_ptr<CSession> session, const unsigned short& msgid, const std::string& body)
{
	int to_uid = session->GetUserId();
	nlohmann::json rsp;
	if (to_uid == 0) {
		std::cout << "[logic] authfriend from unlogined session:" << session->GetSessionId() << std::endl;
		rsp["error"] = TokenInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_AUTH_FRIEND_RSP));
		return;
	}

	nlohmann::json parsed;
	try
	{
		parsed = nlohmann::json::parse(body);
	}
	catch (const std::exception& e)
	{
		std::cout << "[logic] authfriend: bad json: " << e.what() << std::endl;
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_AUTH_FRIEND_RSP));
		return;
	}

	// 申请人的 uid 必须由客户端告诉我们(服务端不知道"界面上这条申请"是谁发的),
	// 但它不是随便信的: DAO 里那句 UPDATE ... WHERE status = 0 会兜底 ——
	// 库里没有这条待处理申请, 影响行数就是 0, 直接失败。
	if (!parsed.contains("applyuid")) {
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_AUTH_FRIEND_RSP));
		return;
	}
	int from_uid = parsed.value("applyuid",0);
	// ★ 可选字段: back = 我(同意方)给申请人起的备注名(客户端会先用申请人名字预填)
	std::string back_name = parsed.value("back", std::string());
	if (back_name.empty()) {
		// 没填就退化成申请人昵称
		UserInfo applyUser;
		if (MysqlMgr::GetInstance()->GetUser(from_uid, applyUser)) {
			back_name = applyUser.nick.empty() ? applyUser.name : applyUser.nick;
		}
	}
	if (from_uid == to_uid) {
		rsp["error"] = UidInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_AUTH_FRIEND_RSP));
		return;
	}
	int ret = MysqlMgr::GetInstance()->AuthFriendApply(from_uid, to_uid, back_name);
	std::cout << "[logic] authfriend applyuid=" << from_uid << " agreed by " << to_uid
		<< " ret=" << ret << " tid=" << std::this_thread::get_id() << std::endl;
	
	if (ret != 0) {
		// 区分两种情况: 已经是好友(1015) 和 申请不存在/已被处理(1016)
		if (MysqlMgr::GetInstance()->IsFriend(from_uid, to_uid)) {
			rsp["error"] = AlreadyFriend;			// 1015
		}
		else {
			rsp["error"] = (ret == 1) ? ApplyExpired : UidInvalid;
		}
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_AUTH_FRIEND_RSP));
		return;
	}

	UserInfo friendInfo;
	if (MysqlMgr::GetInstance()->GetUser(from_uid, friendInfo)) {
		rsp["error"] = 0;
		rsp["uid"] = friendInfo.uid;
		rsp["name"] = friendInfo.name;
		rsp["nick"] = friendInfo.nick;
		rsp["desc"] = friendInfo.desc;
		rsp["sex"] = friendInfo.sex;
		rsp["icon"] = friendInfo.icon;
	}
	else
	{
		rsp["error"] = UidInvalid;
	}
	session->Send(rsp.dump(), static_cast<unsigned short>(ID_AUTH_FRIEND_RSP));

	// ============ 推 A(在线才推): 告诉他 B 同意了, 并把 B 的资料给他 ============
	auto from_session = UserMgr::GetInstance()->GetUser(from_uid);
	if (from_session == nullptr) {
		std::cout << "[logic] uid " << from_uid << " offline, auth result saved to db only" << std::endl;
		return;
	}

	UserInfo selfInfo;
	if (!MysqlMgr::GetInstance()->GetUser(from_uid, selfInfo)) {
		return;
	}

	nlohmann::json notify;
	notify["error"] = 0;
	notify["uid"] = selfInfo.uid;
	notify["name"] = selfInfo.name;
	notify["nick"] = selfInfo.nick;
	notify["desc"] = selfInfo.desc;
	notify["sex"] = selfInfo.sex;
	notify["icon"] = selfInfo.icon;

	from_session->Send(notify.dump(), static_cast<unsigned short>(ID_NOTIFY_AUTH_FRIEND_REQ));
	std::cout << "[logic] notify uid " << from_uid << " (session "
		<< from_session->GetSessionId() << ") auth ok by " << to_uid << std::endl;
}

void LogicSystem::LoadChatThreadHandler(std::shared_ptr<CSession> session, const unsigned short& msgid, const std::string& body)
{
	int uid = session->GetUserId();
	nlohmann::json rsp;
	if (uid == 0) {
		std::cout << "[logic] load from unlogined session" << session->GetSessionId() << std::endl;
		rsp["error"] = TokenInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_LOAD_CHAT_THREAD_RSP));
		return;
	}

	std::vector<std::shared_ptr<UserInfo>> friends;
	std::vector<std::shared_ptr<ApplyInfo>> applies;

	if (!MysqlMgr::GetInstance()->GetFriendList(uid, friends) ||
		!MysqlMgr::GetInstance()->GetApplyList(uid, applies)) 
	{
		rsp["error"] = LOAD_CHAT_FAILED;  // 1013
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_LOAD_CHAT_THREAD_RSP));
		return;
	}

	nlohmann::json friendArr = nlohmann::json::array();
	for (auto& f : friends) {
		nlohmann::json item;
		item["uid"] = f->uid;
		item["name"] = f->name;
		item["nick"] = f->nick;
		item["desc"] = f->desc;
		item["sex"] = f->sex;
		item["icon"] = f->icon;
		item["back"] = f->back;          // 备注名, 客户端列表优先显示它
		item["thread_id"] = MysqlMgr::GetInstance()->GetThreadId(uid, f->uid);
		friendArr.emplace_back(std::move(item));
	}

	nlohmann::json applyArr = nlohmann::json::array();
	for (auto& a : applies) {
		nlohmann::json item;
		item["uid"] = a->_uid;
		item["name"] = a->_name;
		item["nick"] = a->_nick;
		item["desc"] = a->_desc;
		item["sex"] = a->_sex;
		item["icon"] = a->_icon;
		item["status"] = a->_status;
		applyArr.emplace_back(std::move(item));
	}

	rsp["error"] = 0;
	rsp["friends"] = friendArr;
	rsp["apply_list"] = applyArr;

	session->Send(rsp.dump(), static_cast<unsigned short>(ID_LOAD_CHAT_THREAD_RSP));
	std::cout << "[logic] load uid=" << uid << " friends=" << friends.size()
		<< " applies=" << applies.size() << " tid=" << std::this_thread::get_id() << std::endl;
}

void LogicSystem::SendChatMsgHandler(std::shared_ptr<CSession> session, const unsigned short& msgid, const std::string& body)
{
	int from_uid = session->GetUserId();
	nlohmann::json rsp;
	if (from_uid == 0) {
		rsp["error"] = TokenInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
		return;
	}
	nlohmann::json parsed;
	try
	{
		parsed = nlohmann::json::parse(body);
	}
	catch (const std::exception& e)
	{
		std::cout << "[logic] chatmsg: bad json: " << e.what() << std::endl;
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
		return;
	}

	if (!parsed.contains("to_uid") || !parsed.contains("content")) {
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
		return;
	}

	int to_uid = parsed.value("to_uid", 0);
	std::string content = parsed.value("content", std::string());
	int msg_type = parsed.value("msg_type", 1);
	if (to_uid == from_uid) {
		rsp["error"] = UidInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
		return;
	}

	if (content.empty()) {
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
		return;
	}

	// 只有好友之间才能发 —— 这是"谁能给谁发消息"的授权检查, 必须服务端做
	int fr = MysqlMgr::GetInstance()->IsFriend(from_uid, to_uid);
	if (fr == 0) {
		std::cout << "[logic] chatmsg denied: " << from_uid << " is not friend of " << to_uid << std::endl;
		rsp["error"] = NotFriend;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
		return;
	}
	else if (fr < 0) {
		std::cout << "[logic] create chat failed" << std::endl;
		rsp["error"] = CREATE_CHAT_FAILED;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
		return;
	}

	int64_t thread_id = MysqlMgr::GetInstance()->GetOrCreateThread(from_uid, to_uid);
	if (thread_id <= 0) {
		rsp["error"] = CREATE_CHAT_FAILED;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
		return;
	}

	int64_t msg_id = MysqlMgr::GetInstance()->SaveMessage(thread_id, from_uid, to_uid, content, msg_type);
	if (msg_id <= 0) {
		rsp["error"] = CREATE_CHAT_FAILED;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
		return;
	}

	rsp["error"] = 0;
	rsp["msg_id"] = msg_id;
	rsp["thread_id"] = thread_id;
	session->Send(rsp.dump(), static_cast<unsigned short>(ID_TEXT_CHAT_MSG_RSP));
	std::cout << "[logic] chatmsg " << from_uid << " -> " << to_uid
		<< " thread=" << thread_id << " msg=" << msg_id
		<< " tid=" << std::this_thread::get_id() << std::endl;

	//对方在线就推1019
	auto to_session = UserMgr::GetInstance()->GetUser(to_uid);
	if (to_session == nullptr) {
		std::cout << "[logic] uid " << to_uid << " offline, msg saved(status=0), 登录后拉取" << std::endl;
		return;
	}

	nlohmann::json notify;
	notify["error"] = 0;
	notify["msg_id"] = msg_id;
	notify["thread_id"] = thread_id;
	notify["from_uid"] = from_uid;
	notify["to_uid"] = to_uid;
	notify["content"] = content;
	notify["msg_type"] = msg_type;
	to_session->Send(notify.dump(), static_cast<unsigned short>(ID_NOTIFY_TEXT_CHAT_MSG_REQ));
	MysqlMgr::GetInstance()->MarkMsgDelivered(msg_id);			//不标记的话, 对方下次登录会被"离线补投"再发一遍(重复消息)
}

// 拉某个会话的历史消息
void LogicSystem::LoadChatMsgHandler(std::shared_ptr<CSession> session,
	const unsigned short& msgid, const std::string& body)
{
	int uid = session->GetUserId();
	nlohmann::json rsp;
	if (uid == 0) {
		rsp["error"] = TokenInvalid;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_LOAD_CHAT_MSG_RSP));
		return;
	}

	nlohmann::json parsed;
	try {
		parsed = nlohmann::json::parse(body);
	}
	catch (const std::exception& e) {
		std::cout << "[logic] loadmsg: bad json: " << e.what() << std::endl;
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_LOAD_CHAT_MSG_RSP));
		return;
	}
	if (!parsed.contains("thread_id")) {
		rsp["error"] = Error_Json;
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_LOAD_CHAT_MSG_RSP));
		return;
	}
	// 用 value + 默认值(缺字段不抛异常, 见第 10 步那个"缺字段打崩服务端"的教训)
	int64_t thread_id = parsed.value("thread_id", static_cast<int64_t>(0));
	int count = parsed.value("count", 20);
	if (count <= 0 || count > 100) {
		count = 20;      // 上限保护: 不然有人要 100 万条, 一次把库和内存拉爆
	}
	int64_t before_msg_id = parsed.value("before_msg_id", static_cast<int64_t>(0));
	//   越权校验: 这个会话必须是我的。
	//   不做这一步, 任何人登录后猜到/试出一个 thread_id 就能读到别人的私聊记录
	//   —— 这就是典型的"水平越权"(IDOR), 服务端必须每一条数据都验归属。
	if (!MysqlMgr::GetInstance()->IsThreadMember(thread_id, uid)) {
		std::cout << "[logic] loadmsg denied: uid " << uid
			<< " is not member of thread " << thread_id << std::endl;
		rsp["error"] = ThreadInvalid;      // 1018
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_LOAD_CHAT_MSG_RSP));
		return;
	}

	std::vector<std::shared_ptr<ChatMsgInfo>> msgs;
	if (!MysqlMgr::GetInstance()->GetHistoryMsgs(thread_id, count, before_msg_id, msgs)) {
		rsp["error"] = LOAD_CHAT_FAILED;   // 1013
		session->Send(rsp.dump(), static_cast<unsigned short>(ID_LOAD_CHAT_MSG_RSP));
		return;
	}

	nlohmann::json arr = nlohmann::json::array();
	for (auto& m : msgs) {
		nlohmann::json item;
		item["msg_id"] = m->_msg_id;
		item["thread_id"] = m->_thread_id;
		item["from_uid"] = m->_from_uid;
		item["to_uid"] = m->_to_uid;
		item["content"] = m->_content;
		item["msg_type"] = m->_msg_type;
		item["create_time"] = m->_create_time;
		arr.emplace_back(item);
	}

	rsp["error"] = 0;
	rsp["thread_id"] = thread_id;
	rsp["messages"] = arr;
	//本批里最小的 msg_id->客户端下次就用它继续往前翻; 0 表示"已经到头了"
	rsp["next_before_id"] = msgs.empty() ? static_cast<int64_t>(0) : msgs.front()->_msg_id;

	session->Send(rsp.dump(), static_cast<unsigned short>(ID_LOAD_CHAT_MSG_RSP));

	std::cout << "[logic] loadmsg thread=" << thread_id << " uid=" << uid
		<< " count=" << msgs.size() << " tid=" << std::this_thread::get_id() << std::endl;
}

void LogicSystem::DeliverOfflineMsgs(std::shared_ptr<CSession> session, int uid)
{
	std::vector<std::shared_ptr<ChatMsgInfo>> msgs;
	if (!MysqlMgr::GetInstance()->GetOfflineMsgs(uid, msgs)) {
		std::cout << "[logic] get offline msgs failed, uid=" << uid << std::endl;
		return;
	}
	if (msgs.empty()) {
		return;
	}
	// 连续调 Send: 每条都进写队列, 由那个 session 的 WriteLoop 按顺序发出去
	const size_t kMaxPerBatch = 100;
	size_t pushed = 0;
	for (auto& m : msgs) {
		if (pushed >= kMaxPerBatch) {
			std::cout << "[logic] 离线消息超过 " << kMaxPerBatch
				<< " 条, 本批先推这些, 其余下次登录继续" << std::endl;
			break;
		}
		nlohmann::json notify;
		notify["error"] = 0;
		notify["msg_id"] = m->_msg_id;
		notify["thread_id"] = m->_thread_id;
		notify["from_uid"] = m->_from_uid;
		notify["to_uid"] = m->_to_uid;
		notify["content"] = m->_content;
		notify["msg_type"] = m->_msg_type;
		notify["create_time"] = m->_create_time;
		session->Send(notify.dump(), static_cast<unsigned short>(ID_NOTIFY_TEXT_CHAT_MSG_REQ));
		MysqlMgr::GetInstance()->MarkMsgDelivered(m->_msg_id);   // 只标记"推出去的"
		++pushed;
	}

	if (!MysqlMgr::GetInstance()->MarkOfflineMsgsDelivered(uid)) {
		std::cout << "[logic] mark offline delivered failed, uid=" << uid << std::endl;
		return;
	}
	std::cout << "[logic] delivered " << msgs.size() << " offline msgs to uid " << uid
		<< " tid=" << std::this_thread::get_id() << std::endl;
}
