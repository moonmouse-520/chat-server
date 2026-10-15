#pragma once
#include "const.h"
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
// 当前时间戳（秒）
int64_t NowSeconds();

// ============================================================
// SqlConnection —— 池化的"一根连接"（教程同款）
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

// ============================================================
// UserInfo —— 登录/改密用（day12+ 会用到，先定义好）
// ============================================================
struct UserInfo {
	std::string name;
	std::string pwd;
	int uid = 0;
	std::string email;
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

	// 以下三个是 day12/day13 要用的，先按教程接口备好
	bool CheckEmail(const std::string& name, const std::string& email);
	bool UpdatePwd(const std::string& name, const std::string& newpwd);
	bool CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo);

	// 连通性自检（启动时调一次，fail fast）
	bool Ping();

private:
	std::unique_ptr<MySqlPool> pool_;
};
