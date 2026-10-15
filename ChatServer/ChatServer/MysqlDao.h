#pragma once
#include "const.h"
#include <iostream>
#include <memory>
#include <string>
#include <cstdint>
#include <jdbc/mysql_driver.h>
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/cppconn/exception.h>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
// 当前时间戳（秒）
int64_t NowSeconds();

// ============================================================
// SqlConnection —— 池化的"一根连接"
//   用 unique_ptr<sql::Connection> 管生命周期 → 析构自动断连
// ============================================================
class SqlConnection {
public:
	SqlConnection(sql::Connection* con, int64_t lasttime) : _con(con), _last_oper_time(lasttime) {}
	std::unique_ptr<sql::Connection> _con;
	int64_t _last_oper_time;   // 上次使用时间（秒），保活线程用它判断要不要 ping
};

// ============================================================
// MySqlPool —— 连接池（queue + mutex + condvar + 保活线程）
// ============================================================
class MySqlPool {
public:
	MySqlPool(const std::string& url, const std::string& user, const std::string& pass,
	          const std::string& schema, int poolSize);
	~MySqlPool();

	// 借一根连接（★ 最多等 3 秒，超时返回 nullptr —— 不让业务线程永久卡死）
	std::unique_ptr<SqlConnection> getConnection();

	// 还一根连接
	void returnConnection(std::unique_ptr<SqlConnection> con);

	// 停机：唤醒所有等待者 + join 保活线程
	void Close();

	size_t Size();

private:
	std::unique_ptr<SqlConnection> createOne();   // 新建一根连接（失败返回 nullptr）
	void checkConnection();                       // 保活线程体：补足数量 + ping 空闲连接

	std::string url_, user_, pass_, schema_;
	int poolSize_;

	std::queue<std::unique_ptr<SqlConnection>> pool_;
	std::mutex mutex_;
	std::condition_variable cond_;
	std::atomic<bool> b_stop_;
	std::thread _check_thread;
};

struct UserInfo {
	std::string name;
	std::string pwd;
	int uid = 0;
	std::string email;

	std::string nick;
	std::string desc;
	int sex = 0;
	std::string icon;
	std::string back;
};

struct ApplyInfo {
	ApplyInfo(int uid, std::string name, std::string desc, std::string icon,
		std::string nick, int sex, int status)
		: _uid(uid), _name(std::move(name)), _desc(std::move(desc)), _icon(std::move(icon)),
		_nick(std::move(nick)), _sex(sex), _status(status) {
	}
	int _uid;
	std::string _name;
	std::string _icon;
	std::string _desc;
	std::string _nick;
	int _sex;
	int _status;
};

struct ChatMsgInfo {
	ChatMsgInfo(int64_t msg_id, int64_t thread_id, int from_uid, int to_uid,
		std::string content, int msg_type, std::string create_time)
		: _msg_id(msg_id), _thread_id(thread_id), _from_uid(from_uid), _to_uid(to_uid),
		_content(std::move(content)), _msg_type(msg_type), _create_time(std::move(create_time)) {
	}
	int64_t _msg_id;
	int64_t _thread_id;
	int _from_uid;
	int _to_uid;
	std::string _content;
	int _msg_type;
	std::string _create_time;
};

// ============================================================
// MysqlDao —— 数据访问层（SQL 全写在这里）
// ============================================================
class MysqlDao {
public:
	MysqlDao();
	~MysqlDao();

	// 注册：查重 + 发号 + 插入，一个事务完成
	//   返回 > 0 : 新的 uid ｜ 0 : 用户名或邮箱已存在 ｜ -1 : 出错
	int RegUserTransaction(const std::string& name, const std::string& email,
	                       const std::string& pwd, const std::string& icon);
	bool CheckEmail(const std::string& name, const std::string& email);
	bool UpdatePwd(const std::string& name, const std::string& newpwd);
	bool CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo);
	bool GetUser(int uid, UserInfo& userInfo);
	bool GetUser(const std::string& name, UserInfo& userInfo);
	bool Ping();																								// 连通性自检（启动时调一次，fail fast）
		
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
	std::unique_ptr<MySqlPool> pool_;
};
