#pragma once
#include "const.h"
#include "MysqlDao.h"
#include "Singleton.h"

// ============================================================
// MysqlMgr —— 服务层（单例）
//   分层：LogicSystem（业务） → MysqlMgr（服务） → MysqlDao（SQL/连接池）
//   接口和教程保持一致，后续 day12/day13 可以直接照抄教程的业务代码
// ============================================================
class MysqlMgr : public Singleton<MysqlMgr>
{
	friend class Singleton<MysqlMgr>;
public:
	~MysqlMgr();

	// 返回 > 0 : 新 uid；0 : 用户名/邮箱已存在；-1 : 出错
	int RegUser(const std::string& name, const std::string& email,
	            const std::string& pwd,
	            const std::string& icon = ":/res/head_1.jpg");

	bool CheckEmail(const std::string& name, const std::string& email);
	bool UpdatePwd(const std::string& name, const std::string& newpwd);
	bool CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo);
	bool GetUser(int uid, UserInfo& userInfo);
	bool GetUser(const std::string& name, UserInfo& userInfo);
	bool Ping(); // 连通性自检
	int AddFriendApply(int from_uid, int to_uid, const std::string& desc, const std::string& back_name);
	int AuthFriendApply(int from_uid, int to_uid, const std::string& back_name);
	bool GetFriendList(int uid, std::vector<std::shared_ptr<UserInfo>>& friends);
	bool GetApplyList(int uid, std::vector<std::shared_ptr<ApplyInfo>>& applies);
	bool IsFriend(int uid1, int uid2);																			//是不是好友
	int64_t GetOrCreateThread(int uid1, int uid2);																//去/建一对用户的会话, 返回threadid (<= 0 代表失败)
	int64_t GetThreadId(int uid1, int uid2);
	int64_t SaveMessage(int64_t thread_id, int from_id, int to_id, const std::string& content, int msg_type);	//存一条消息, 返回msg_id (<= 0 代表失败)
	bool GetHistoryMsgs(int64_t thread_id, int count, int64_t before_msg_id, std::vector<std::shared_ptr<ChatMsgInfo>>& msgs);			// 会话历史(取最近 count 条, 返回时按时间正序)
	bool IsThreadMember(int64_t thread_id, int uid);															// 这个会话是不是我的(拉历史前的越权校验)
	bool GetOfflineMsgs(int uid, std::vector<std::shared_ptr<ChatMsgInfo>>& msgs);								// 我还没收到的消息(status = 0)
	bool MarkOfflineMsgsDelivered(int uid);																		// 把"我还没收到的"整批标记成已投递
	bool MarkMsgDelivered(int64_t msg_id);																			// 单条标记已投递(在线推送成功后用)


private:
	MysqlMgr();

	MysqlDao _dao;
};
