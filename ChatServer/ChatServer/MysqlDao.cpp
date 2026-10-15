#include "MysqlDao.h"
#include "ConfigMgr.h"

#include <iostream>

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

	//   等待必须有上限
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
	cond_.notify_one();   // 还了必须通知，否则借的人还在睡
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

bool MysqlDao::GetUser(int uid, UserInfo& userInfo)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) {
		return false;
	}

	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});
	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("SELECT uid,name,nick,`desc`,sex,icon FROM user WHERE uid = ?")
		);

		pstmt->setInt(1, uid);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		while (res->next()) {
			userInfo.uid = res->getInt("uid");
			userInfo.name = res->getString("name");
			userInfo.nick = res->getString("nick");
			userInfo.desc = res->getString("desc");
			userInfo.sex = res->getInt("sex");
			userInfo.icon = res->getString("icon");
			return true;
		}
		return false;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode()
			<< ", SQLState: " << e.getSQLState() << ")" << std::endl;
		return false;
	}
}

bool MysqlDao::GetUser(const std::string& name, UserInfo& userInfo)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) {
		return false;
	}

	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("SELECT uid,name,nick,`desc`,sex,icon FROM user WHERE name = ?")
		);
		pstmt->setString(1, name);

		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		while (res->next())
		{
			userInfo.uid = res->getInt("uid");
			userInfo.name = res->getString("name");
			userInfo.nick = res->getString("nick");
			userInfo.desc = res->getString("desc");
			userInfo.sex = res->getInt("sex");
			userInfo.icon = res->getString("icon");
			return true;
		}	
		return false;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "SQLException: " << e.what() << std::endl;
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

int MysqlDao::AddFriendApply(int from_uid, int to_uid, const std::string& desc, const std::string& back_name)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) {
		return -1;
	}
	Defer defer([this, &conn] {
		pool_->returnConnection(std::move(conn));
		});
	try
	{
		{
			//已经是好友了吗 ? friend 表存的是有向边 : self_id → friend_id
			std::unique_ptr<sql::PreparedStatement> pstmt(
				conn->_con->prepareStatement("SELECT 1 FROM friend WHERE self_id = ? AND friend_id = ?"));
			pstmt->setInt(1, from_uid);
			pstmt->setInt(2, to_uid);
			std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
			if (res->next()) {
				return 2;		//已经是好友了
			}
		}
		
		{
			//有没有还没处理的申请? (客户端连点两下不该产生两条申请)
			std::unique_ptr<sql::PreparedStatement> pstmt(
				conn->_con->prepareStatement("SELECT 1 FROM friend_apply WHERE from_uid = ? AND to_uid = ? AND status = 0"));
			pstmt->setInt(1, from_uid);
			pstmt->setInt(2, to_uid);
			std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
			if (res->next()) {
				std::unique_ptr<sql::PreparedStatement> upd(
					conn->_con->prepareStatement("UPDATE friend_apply SET descs = ?, back_name = ? WHERE from_uid = ? AND to_uid = ?"));
				upd->setString(1, desc);
				upd->setString(2, back_name);
				upd->setInt(3, from_uid);
				upd->setInt(4, to_uid);
				upd->executeUpdate();
				return 1;		//已经申请过, 等对方处理
			}
		}
		
		// 落库 status = 0 待处理
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("INSERT INTO friend_apply (from_uid, to_uid, status, descs, back_name) VALUES (?, ?, 0, ?, ?) "
				"ON DUPLICATE KEY UPDATE descs = ?, back_name = ?"));
		pstmt->setInt(1, from_uid);
		pstmt->setInt(2, to_uid);
		pstmt->setString(3, desc);
		pstmt->setString(4, back_name);
		pstmt->setString(5, desc);
		pstmt->setString(6, back_name);
		pstmt->executeUpdate();
		return 0;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode() << ")" << std::endl;
		return -1;
	}
}

int MysqlDao::AuthFriendApply(int from_uid, int to_uid, const std::string& back_name)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) return -1;

	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		conn->_con->setAutoCommit(false);

		std::string a_back;
		{
			std::unique_ptr < sql::PreparedStatement >sel(
			conn->_con->prepareStatement("SELECT back_name FROM friend_apply WHERE from_uid = ? AND to_uid = ? FOR UPDATE"));
			sel->setInt(1, from_uid);
			sel->setInt(2, to_uid);
			std::unique_ptr<sql::ResultSet> res(sel->executeQuery());
			if (!res->next()) {
				conn->_con->rollback();
				conn->_con->setAutoCommit(true);
				return 1;	//没有这条申请
			}
			a_back = res->getString("back_name");
		}

		std::unique_ptr<sql::PreparedStatement> upd(
			conn->_con->prepareStatement("UPDATE friend_apply SET status = 1 WHERE from_uid = ? AND to_uid = ? AND status = 0"));
		upd->setInt(1, from_uid);
		upd->setInt(2, to_uid);
		if (upd->executeUpdate() == 0) {
			conn->_con->rollback();
			conn->_con->setAutoCommit(true);
			return 1;			//申请不存在已经处理
		}
		//建两条有向边, 各带各的备注
		std::unique_ptr<sql::PreparedStatement> ins(
			conn->_con->prepareStatement("INSERT IGNORE INTO friend (self_id, friend_id, back) VALUES (?, ?, ?)"));
		ins->setInt(1, from_uid);
		ins->setInt(2, to_uid);
		ins->setString(3, back_name);
		ins->executeUpdate();	// A 的联系人里有 B
		ins->setInt(1, to_uid);
		ins->setInt(2, from_uid);
		ins->setString(3, a_back);
		ins->executeUpdate();	// B 的联系人里有 A

		conn->_con->commit();
		conn->_con->setAutoCommit(true);          
		return 0;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "AuthFriendApply SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode() << ")" << std::endl;
		try
		{
			conn->_con->rollback();
			conn->_con->setAutoCommit(true);
		}
		catch (sql::SQLException& e)
		{

		}
		return -1;
	}
}

bool MysqlDao::GetFriendList(int uid, std::vector<std::shared_ptr<UserInfo>>& friends)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) {
		return false;
	}
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(conn->_con->prepareStatement(
			"SELECT u.uid,u.name,u.nick,u.`desc`,u.sex,u.icon,f.back "
			"FROM friend f JOIN user u ON f.friend_id = u.uid "
			"WHERE f.self_id = ?"));
		pstmt->setInt(1, uid);	
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		while (res->next())	
		{
			auto info = std::make_shared<UserInfo>();
			info->uid = res->getInt("uid");
			info->name = res->getString("name");
			info->nick = res->getString("nick");
			info->desc = res->getString("desc");
			info->sex = res->getInt("sex");
			info->icon = res->getString("icon");
			info->back = res->getString("back");
			friends.emplace_back(info);
		}
		return true;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "GetFriendList SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode() << ")" << std::endl;
		return false;
	}
}

bool MysqlDao::GetApplyList(int uid, std::vector<std::shared_ptr<ApplyInfo>>& applies)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) {
		return false;
	}
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("SELECT f.from_uid, u.name, u.nick, u.`desc`,u.sex, u.icon, f.status "
				"FROM friend_apply f JOIN user u ON f.from_uid = u.uid "
				"WHERE f.to_uid = ? AND f.status = 0 ORDER BY f.id DESC"));
		pstmt->setInt(1, uid);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		while (res->next())
		{
			applies.emplace_back(std::make_shared<ApplyInfo>(
				res->getInt("from_uid"),
				res->getString("name"),
				res->getString("desc"),
				res->getString("icon"),
				res->getString("nick"),
				res->getInt("sex"),
				res->getInt("status")));
		}
		return true;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "GetApplyList SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode() << ")" << std::endl;
		return false;
	}
}

bool MysqlDao::IsFriend(int uid1, int uid2)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) return false;
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("SELECT 1 FROM friend WHERE self_id = ? AND friend_id = ?"));
		pstmt->setInt(1,uid1);
		pstmt->setInt(2,uid2);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		return res->next();
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "IsFriend SQLException: " << e.what() << std::endl;
		// 查库失败也返回 false: 业务层会当成"不是好友"拒掉。
		// 这是有意选的"安全侧" —— 宁可发不出去, 也不要把消息发给不该发的人。
		return false;
	}
}

int64_t MysqlDao::GetOrCreateThread(int uid1, int uid2)
{
	//规范化: owner_id 永远存小的那个。A->B 和 ->A 因此落到同一行。
	int owner_id = (uid1 < uid2) ? uid1 : uid2;
	int other_id = (uid1 < uid2) ? uid2 : uid1;

	auto conn = pool_->getConnection();
	if (conn == nullptr) {
		return -1;
	}

	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});
	
	try
	{
		{		//已有会话
			std::unique_ptr<sql::PreparedStatement> pstmt(
				conn->_con->prepareStatement("SELECT thread_id FROM chat_thread WHERE owner_id = ? AND other_id = ?"));
			pstmt->setInt(1, owner_id);
			pstmt->setInt(2, other_id);
			std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
			if (res->next()) {
				return res->getInt64("thread_id");
			}
		}


		try
		{	//没有就插。并发下两个线程可能同时走到这里 —— 唯一键 uk_pair 会让其中一个拿到 1062
			std::unique_ptr<sql::PreparedStatement> pstmt(
				conn->_con->prepareStatement("INSERT INTO chat_thread (`type`, owner_id, other_id) VALUES (1, ?, ?)"));
			pstmt->setInt(1, owner_id);
			pstmt->setInt(2, other_id);
			pstmt->executeUpdate();
		}
		catch (sql::SQLException& e)
		{
			if (e.getErrorCode() != 1062) {
				throw;		 // 不是"重复键", 是真错误(表不存在/连接断了), 往上抛给外层日志
			}
		}

		//重查: 无论是自己刚插的, 还是并发时被别人抢先插的, 都能查到
		std::unique_ptr<sql::PreparedStatement> pstmt(conn->_con->prepareStatement(
			"SELECT thread_id FROM chat_thread WHERE owner_id = ? AND other_id = ?"));
		pstmt->setInt(1, owner_id);
		pstmt->setInt(2, other_id);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		if (res->next()) {
			return res->getInt64("thread_id");
		}
		return -1;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "GetOrCreateThread SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode() << ")" << std::endl;
		return -1;
	}
}

int64_t MysqlDao::GetThreadId(int uid1, int uid2)
{
	int owner_id = (uid1 < uid2) ? uid1 : uid2;
	int other_id = (uid1 < uid2) ? uid2 : uid1;
	
	auto conn = pool_->getConnection();
	if (conn == nullptr) return 0;
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(conn->_con->prepareStatement(
			"SELECT thread_id FROM chat_thread WHERE owner_id = ? AND other_id = ?"));
		pstmt->setInt(1, owner_id);
		pstmt->setInt(2, other_id);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		if (res->next()) {
			return res->getInt64("thread_id");
		}
		return 0;                       // 没聊过 -> 0
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "GetThreadId SQLException: " << e.what() << std::endl;
		return 0;
	}
}

int64_t MysqlDao::SaveMessage(int64_t thread_id, int from_id, int to_id, const std::string& content, int msg_type)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) return -1;
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("INSERT INTO chat_msg (thread_id, from_uid, to_uid, content, msg_type, status) "
										 "VALUES (?, ?, ?, ?, ?, 0)"));
		pstmt->setInt64(1, thread_id);
		pstmt->setInt(2, from_id);
		pstmt->setInt(3, to_id);
		pstmt->setString(4, content);
		pstmt->setInt(5, msg_type);
		pstmt->executeUpdate();

		// 拿刚插入的自增 id。LAST_INSERT_ID() 是"本连接上一次插入"的 id,
		// 所以必须在同一条连接上紧接着查(我们用的就是) —— 换个连接查就是别人的 id。
		// 它的列名就是字符串 "LAST_INSERT_ID()", 用列序号 1 取最省事。
		std::unique_ptr<sql::Statement> stmt(conn->_con->createStatement());
		std::unique_ptr<sql::ResultSet> res(stmt->executeQuery("SELECT LAST_INSERT_ID()"));
		if (res->next()) {
			return res->getInt64(1);
		}
		return -1;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "SaveMessage SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode() << ")" << std::endl;
		return -1;
	}
}

bool MysqlDao::GetHistoryMsgs(int64_t thread_id, int count, int64_t before_msg_id, std::vector<std::shared_ptr<ChatMsgInfo>>& msgs)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) return false;
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("SELECT msg_id,thread_id,from_uid,to_uid,content,msg_type,create_time"
				" from chat_msg WHERE thread_id = ? AND (? = 0 OR msg_id < ?)"
				" ORDER BY msg_id DESC LIMIT ?"));
		pstmt->setInt64(1, thread_id);
		pstmt->setInt64(2, before_msg_id);
		pstmt->setInt64(3, before_msg_id);
		pstmt->setInt(4, count);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

		while (res->next())
		{
			msgs.emplace_back(std::make_shared<ChatMsgInfo>(
				res->getInt64("msg_id"),
				res->getInt64("thread_id"),
				res->getInt("from_uid"),
				res->getInt("to_uid"),
				res->getString("content"),
				res->getInt("msg_type"),
				res->getString("create_time")));
		}

		std::reverse(msgs.begin(), msgs.end());// 反转为时间正序, 客户端从上往下渲染
		return true;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "GetHistoryMsgs SQLException: " << e.what()
			<< " (MySQL error code: " << e.getErrorCode() << ")" << std::endl;
		return false;
	}
}

bool MysqlDao::IsThreadMember(int64_t thread_id, int uid)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) return false;
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("SELECT 1 FROM chat_thread WHERE thread_id = ? AND (owner_id = ? OR other_id = ?)"));
		pstmt->setInt64(1, thread_id);
		pstmt->setInt(2, uid);
		pstmt->setInt(3, uid);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
		return res->next();
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "IsThreadMember SQLException: " << e.what() << std::endl;
		return false;   // 查库失败也当"没权限", 拒掉(安全侧)
	}
}

bool MysqlDao::GetOfflineMsgs(int uid, std::vector<std::shared_ptr<ChatMsgInfo>>& msgs)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) return false;
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("SELECT msg_id,thread_id,from_uid,to_uid,content,msg_type,create_time"
										 " FROM chat_msg WHERE to_uid = ? AND status = 0 ORDER BY msg_id ASC"));
		pstmt->setInt(1, uid);
		std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());	
		while (res->next()) {
			msgs.emplace_back(std::make_shared<ChatMsgInfo>(
				res->getInt64("msg_id"),
				res->getInt64("thread_id"),
				res->getInt("from_uid"),
				res->getInt("to_uid"),
				res->getString("content"),
				res->getInt("msg_type"),
				res->getString("create_time")));
		}
		return true; // 没有离线消息也是成功(空列表)
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "GetOfflineMsgs SQLException: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::MarkOfflineMsgsDelivered(int uid)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) return false;
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(
			conn->_con->prepareStatement("UPDATE chat_msg SET status = 1 WHERE to_uid = ? AND status = 0"));
		pstmt->setInt(1, uid);
		pstmt->executeUpdate();
		return true;   // 影响 0 行也正常(本来就没有离线消息)
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "MarkOfflineMsgsDelivered SQLException: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::MarkMsgDelivered(int64_t msg_id)
{
	auto conn = pool_->getConnection();
	if (conn == nullptr) return false;
	Defer defer([this, &conn]() {
		pool_->returnConnection(std::move(conn));
		});

	try
	{
		std::unique_ptr<sql::PreparedStatement> pstmt(conn->_con->prepareStatement(
			"UPDATE chat_msg SET status = 1 WHERE msg_id = ?"));
		pstmt->setInt64(1, msg_id);
		int affected = pstmt->executeUpdate();
		if (affected <= 0)
		{
			std::cerr << "MarkMsgDelivered: no row updated, msg_id=" << msg_id << std::endl;
			return false;
		}
		return true;
	}
	catch (sql::SQLException& e)
	{
		std::cerr << "MarkMsgDelivered SQLException: " << e.what() << std::endl;
		return false;
	}
}
