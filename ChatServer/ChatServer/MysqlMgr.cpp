#include "MysqlMgr.h"

MysqlMgr::~MysqlMgr() {
}

MysqlMgr::MysqlMgr() {
}

int MysqlMgr::RegUser(const std::string& name, const std::string& email,
                      const std::string& pwd, const std::string& icon)
{
	return _dao.RegUserTransaction(name, email, pwd, icon);
}

bool MysqlMgr::CheckEmail(const std::string& name, const std::string& email)
{
	return _dao.CheckEmail(name, email);
}

bool MysqlMgr::UpdatePwd(const std::string& name, const std::string& newpwd)
{
	return _dao.UpdatePwd(name, newpwd);
}

bool MysqlMgr::CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo)
{
	return _dao.CheckPwd(email, pwd, userInfo);
}

bool MysqlMgr::GetUser(int uid, UserInfo& userInfo)
{
	return _dao.GetUser(uid, userInfo);
}

bool MysqlMgr::GetUser(const std::string& name, UserInfo& userInfo)
{
	return _dao.GetUser(name, userInfo);
}

bool MysqlMgr::Ping()
{
	return _dao.Ping();
}

int MysqlMgr::AddFriendApply(int from_uid, int to_uid, const std::string& desc, const std::string& back_name)
{
	return _dao.AddFriendApply(from_uid, to_uid,desc,back_name);
}

int MysqlMgr::AuthFriendApply(int from_uid, int to_uid, const std::string& back_name)
{
	return _dao.AuthFriendApply(from_uid, to_uid, back_name);
}

bool MysqlMgr::GetFriendList(int uid, std::vector<std::shared_ptr<UserInfo>>& friends)
{
	return _dao.GetFriendList(uid, friends);
}

bool MysqlMgr::GetApplyList(int uid, std::vector<std::shared_ptr<ApplyInfo>>& applies)
{
	return _dao.GetApplyList(uid, applies);
}

bool MysqlMgr::IsFriend(int uid1, int uid2)
{
	return _dao.IsFriend(uid1, uid2);
}

int64_t MysqlMgr::GetOrCreateThread(int uid1, int uid2)
{
	return _dao.GetOrCreateThread(uid1, uid2);
}

int64_t MysqlMgr::GetThreadId(int uid1, int uid2)
{
	return _dao.GetThreadId(uid1,uid2);
}

int64_t MysqlMgr::SaveMessage(int64_t thread_id, int from_uid, int to_uid,
	const std::string& content, int msg_type)
{
	return _dao.SaveMessage(thread_id, from_uid, to_uid, content, msg_type);
}

bool MysqlMgr::GetHistoryMsgs(int64_t thread_id, int count, int64_t before_msg_id, std::vector<std::shared_ptr<ChatMsgInfo>>& msgs)
{
	return _dao.GetHistoryMsgs(thread_id, count, before_msg_id, msgs);
}

bool MysqlMgr::IsThreadMember(int64_t thread_id, int uid)
{
	return _dao.IsThreadMember(thread_id, uid);
}

bool MysqlMgr::GetOfflineMsgs(int uid, std::vector<std::shared_ptr<ChatMsgInfo>>& msgs)
{
	return _dao.GetOfflineMsgs(uid, msgs);
}

bool MysqlMgr::MarkOfflineMsgsDelivered(int uid)
{
	return _dao.MarkOfflineMsgsDelivered(uid);
}

bool MysqlMgr::MarkMsgDelivered(int64_t msg_id)
{
	return _dao.MarkMsgDelivered(msg_id);
}