#include "MysqlDao.h"
#include "ConfigMgr.h"

int64_t NowSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

// ============================================================
// MySqlPool
// ============================================================
MySqlPool::MySqlPool(const std::string& url, const std::string& user, const std::string& pass,
                     const std::string& schema, int poolSize)
	: url_(url), user_(user), pass_(pass), schema_(schema), poolSize_(poolSize), b_stop_(false)
{
	// 先尽量建满（建不出来的先跳过，保活线程会补）
	for (int i = 0; i < poolSize_; ++i) {
		auto con = createOne();
		if (con != nullptr) {
			pool_.push(std::move(con));
		}
	}
	std::cout << "mysql pool init done, size = " << pool_.size()
		<< " / " << poolSize_ << std::endl;

	// ★ 保活线程无论初始化成功与否都要起来
	//   （教程把它放在构造的 try 里，MySQL 挂了就永远不会自愈）
	_check_thread = std::thread([this]() {
		while (!b_stop_) {
			checkConnection();
			// 分片睡眠：让 Close() 最多等 1 秒就能退出
			for (int i = 0; i < 6 && !b_stop_; ++i) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
		}
	});
}

MySqlPool::~MySqlPool() {
	Close();
	// 排空池子（unique_ptr 析构 → sql::Connection 析构 → 断开连接）
	std::lock_guard<std::mutex> lock(mutex_);
	while (!pool_.empty()) {
		pool_.pop();
	}
}

std::unique_ptr<SqlConnection> MySqlPool::createOne()
{
	try {
		sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
		std::unique_ptr<sql::Connection> con(driver->connect(url_, user_, pass_));
		con->setSchema(schema_);
		return std::make_unique<SqlConnection>(con.release(), NowSeconds());
	}
	catch (sql::SQLException& e) {
		std::cerr << "mysql connect failed: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode()
			<< ", SQLState: " << e.getSQLState() << ")" << std::endl;
		return nullptr;
	}
}

void MySqlPool::checkConnection()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (b_stop_) {
		return;
	}

	// ① 补足连接数（MySQL 比 GateServer 晚启动也能自愈）
	while ((int)pool_.size() < poolSize_) {
		auto con = createOne();
		if (con == nullptr) {
			break;   // 连不上就别死循环，下一轮再说
		}
		pool_.push(std::move(con));
		std::cout << "mysql connection re-created, size = " << pool_.size() << std::endl;
	}

	// ② ping 空闲超过 5 秒的连接，坏了就换一根
	int n = (int)pool_.size();
	for (int i = 0; i < n; ++i) {
		if (pool_.empty()) {
			break;
		}
		std::unique_ptr<SqlConnection> con = std::move(pool_.front());
		pool_.pop();

		if (NowSeconds() - con->_last_oper_time >= 5) {
			try {
				std::unique_ptr<sql::Statement> stmt(con->_con->createStatement());
				std::unique_ptr<sql::ResultSet> res(stmt->executeQuery("SELECT 1"));
				con->_last_oper_time = NowSeconds();
			}
			catch (sql::SQLException& e) {
				std::cerr << "mysql ping failed, reconnecting: " << e.what() << std::endl;
				auto fresh = createOne();
				if (fresh == nullptr) {
					continue;   // 换不出来就丢掉这根坏连接，让补足逻辑去处理
				}
				con = std::move(fresh);
			}
		}
		pool_.push(std::move(con));
	}
}

std::unique_ptr<SqlConnection> MySqlPool::getConnection()
{
	std::unique_lock<std::mutex> lock(mutex_);

	// ★ 等待必须有上限：教程版 cond_.wait(...) 是无限等，
	//   一旦池子空（MySQL 挂了），业务线程会永久卡死；
	//   而 AsioIOServicePool 只有 2 个工作线程 —— 卡两个整个服务就不响应了。
	bool got = cond_.wait_for(lock, std::chrono::seconds(3), [this]() {
		return b_stop_ || !pool_.empty();
		});
	if (!got) {
		std::cerr << "getConnection timeout: mysql pool is empty" << std::endl;
		return nullptr;
	}
	if (b_stop_ || pool_.empty()) {
		return nullptr;
	}
	std::unique_ptr<SqlConnection> con = std::move(pool_.front());
	pool_.pop();
	return con;
}

void MySqlPool::returnConnection(std::unique_ptr<SqlConnection> con)
{
	if (con == nullptr) {
		return;
	}
	std::unique_lock<std::mutex> lock(mutex_);
	if (b_stop_) {
		return;   // 停机中：unique_ptr 出作用域自动析构断开
	}
	con->_last_oper_time = NowSeconds();
	pool_.push(std::move(con));
	cond_.notify_one();   // ★ 还了必须通知，否则借的人还在睡
}

void MySqlPool::Close()
{
	bool expected = false;
	if (!b_stop_.compare_exchange_strong(expected, true)) {
		return;   // 已经关过了
	}
	cond_.notify_all();
	if (_check_thread.joinable()) {
		_check_thread.join();
	}
}

size_t MySqlPool::Size()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return pool_.size();
}

// ============================================================
// MysqlDao
// ============================================================
MysqlDao::MysqlDao()
{
	auto& cfg = ConfigMgr::Inst();
	const auto& host = cfg["Mysql"]["Host"];
	const auto& port = cfg["Mysql"]["Port"];
	const auto& user = cfg["Mysql"]["User"];
	const auto& passwd = cfg["Mysql"]["Passwd"];
	const auto& schema = cfg["Mysql"]["Schema"];
	// 教程就是 "host:port" 拼成 url 传给 driver->connect
	pool_.reset(new MySqlPool(host + ":" + port, user, passwd, schema, 5));
}

MysqlDao::~MysqlDao()
{
	pool_->Close();
}

int MysqlDao::RegUserTransaction(const std::string& name, const std::string& email,
                                 const std::string& pwd, const std::string& icon)
{
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return -1;
	}

	// ★ 教程的 Defer：无论从哪条路返回，连接都会还回池子
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	// 统一收尾：回滚 + 恢复自动提交
	// （★ 一定要恢复，否则连接还回池子后，下一个使用者会莫名其妙处于事务里）
	auto fail = [&]() {
		try {
			con->_con->rollback();
			con->_con->setAutoCommit(true);
		}
		catch (...) {}
		return -1;
	};

	try {
		con->_con->setAutoCommit(false);      // 开事务（对应 JDBC 的 setAutoCommit(false)）

		// ① 邮箱查重
		std::unique_ptr<sql::PreparedStatement> pstmt_email(
			con->_con->prepareStatement("SELECT 1 FROM user WHERE email = ?"));
		pstmt_email->setString(1, email);
		std::unique_ptr<sql::ResultSet> res_email(pstmt_email->executeQuery());
		if (res_email->next()) {
			std::cout << "email " << email << " exist" << std::endl;
			con->_con->rollback();
			con->_con->setAutoCommit(true);
			return 0;
		}

		// ② 用户名查重
		std::unique_ptr<sql::PreparedStatement> pstmt_name(
			con->_con->prepareStatement("SELECT 1 FROM user WHERE name = ?"));
		pstmt_name->setString(1, name);
		std::unique_ptr<sql::ResultSet> res_name(pstmt_name->executeQuery());
		if (res_name->next()) {
			std::cout << "name " << name << " exist" << std::endl;
			con->_con->rollback();
			con->_con->setAutoCommit(true);
			return 0;
		}

		// ③ 发号：user_id 单行表 id+1，拿到新 uid
		std::unique_ptr<sql::PreparedStatement> pstmt_upid(
			con->_con->prepareStatement("UPDATE user_id SET id = id + 1"));
		pstmt_upid->executeUpdate();

		std::unique_ptr<sql::PreparedStatement> pstmt_uid(
			con->_con->prepareStatement("SELECT id FROM user_id"));
		std::unique_ptr<sql::ResultSet> res_uid(pstmt_uid->executeQuery());
		int newId = 0;
		if (res_uid->next()) {
			newId = res_uid->getInt("id");
		}
		else {
			std::cout << "select id from user_id failed" << std::endl;
			return fail();
		}

		// ④ 插入用户（nick 默认跟 name 一样，icon 用默认头像）
		std::unique_ptr<sql::PreparedStatement> pstmt_insert(con->_con->prepareStatement(
			"INSERT INTO user (uid, name, email, pwd, nick, icon) VALUES (?,?,?,?,?,?)"));
		pstmt_insert->setInt(1, newId);
		pstmt_insert->setString(2, name);
		pstmt_insert->setString(3, email);
		pstmt_insert->setString(4, pwd);
		pstmt_insert->setString(5, name);
		pstmt_insert->setString(6, icon);
		pstmt_insert->executeUpdate();

		con->_con->commit();                  // 提交事务
		con->_con->setAutoCommit(true);       // ★ 归还前恢复自动提交
		std::cout << "newuser insert into user success, uid = " << newId << std::endl;
		return newId;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode()
			<< ", SQLState: " << e.getSQLState() << ")" << std::endl;
		return fail();
	}
}

bool MysqlDao::CheckEmail(const std::string& name, const std::string& email)
{
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(
			con->_con->prepareStatement("SELECT email FROM user WHERE name = ?")
		);
		pstmt->setString(1, name);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		while (res->next()) {
			std::cout << "Check Email: " << res->getString("email") << std::endl;
			return email == res->getString("email"); 
		}
		return false;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode()
			<< ", SQLState: " << e.getSQLState() << ")" << std::endl;
		return false;
	}
}

bool MysqlDao::UpdatePwd(const std::string& name, const std::string& newpwd)
{
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(
			con->_con->prepareStatement("UPDATE user SET pwd = ? WHERE name = ?"));
		pstmt->setString(1, newpwd);
		pstmt->setString(2, name);
		int updateCount = pstmt->executeUpdate();
		std::cout << "Updated rows: " << updateCount << std::endl;
		return true;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode()
			<< ", SQLState: " << e.getSQLState() << ")" << std::endl;
		return false;
	}
}

bool MysqlDao::CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo)
{
	auto con = pool_->getConnection();
	if (con == nullptr) {
		return false;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		std::unique_ptr<sql::PreparedStatement> pstmt(
			con->_con->prepareStatement("SELECT * FROM user WHERE email = ?"));
		pstmt->setString(1, email);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

		// ★ 注意：所有字段都必须在【游标有效时】读完
		//   （教程版是 break 出循环后再 res->getString("name")，游标已失效）
		std::string origin_pwd;
		bool found = false;
		while (res->next()) {
			origin_pwd = res->getString("pwd");
			userInfo.name = res->getString("name");
			userInfo.email = res->getString("email");
			userInfo.uid = res->getInt("uid");
			userInfo.pwd = origin_pwd;
			found = true;
			break;
		}
		if (!found) {
			return false;
		}
		return pwd == origin_pwd;
	}
	catch (sql::SQLException& e) {
		std::cerr << "SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode()
			<< ", SQLState: " << e.getSQLState() << ")" << std::endl;
		return false;
	}
}

bool MysqlDao::Ping()
{
	auto con = pool_->getConnection();
	if (con == nullptr) {
		std::cerr << "mysql ping failed: no connection available" << std::endl;
		return false;
	}
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		std::unique_ptr<sql::Statement> stmt(con->_con->createStatement());
		std::unique_ptr<sql::ResultSet> res(stmt->executeQuery("SELECT 1"));
		return res->next();
	}
	catch (sql::SQLException& e) {
		std::cerr << "mysql ping failed: " << e.what() << std::endl;
		return false;
	}
}