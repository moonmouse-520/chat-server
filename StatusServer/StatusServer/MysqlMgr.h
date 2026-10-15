#pragma once
#include "const.h"
#include "MysqlDao.h"

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

	// 连通性自检
	bool Ping();

private:
	MysqlMgr();

	MysqlDao _dao;
};
