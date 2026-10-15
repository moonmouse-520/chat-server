#pragma once
#include <iostream>
#include <boost/beast/http.hpp>
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <memory>
#include "Singleton.h"
#include <functional>
#include <map>
#include <unordered_map>
#include <json.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <sw/redis++/redis++.h>
#include <jdbc/mysql_driver.h>
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/cppconn/exception.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using Json = nlohmann::json;

enum ErrorCodes {
	Success = 0,			
	Error_Json = 1001,		//Json解析
	RPCFailed = 1002,		//RPC请求
	VarifyExpired = 1003,	//验证码过期
	VarifyCodeErr = 1004,	//验证码错误
	UserExist = 1005,		//用户已存在
	PasswdErr = 1006,		//密码错误
	EmailNotMatch = 1007,	//邮箱不匹配
	PasswdUpFailed = 1008,	//更新密码失败
	PasswdInvalid = 1009,	//密码更新失败
	TokenInvalid = 1010,	//Token失效（StatusServiceImpl::Login 用）
	UidInvalid = 1011,		//uid无效（Redis 里没有这个 uid 的 token）
	DBErr = 1012,			//数据库操作失败（拿不到连接/SQL异常）

};

// 验证码在 Redis 里的 key 前缀：code_<email>
// ★ 必须和 VarifyServer/config.js 的 code_prefix 完全一致，否则取不到缓存
#define CODEPREFIX "code_"

// ---- Redis key 前缀（和教程一致，后面 ChatServer 也会用）----
#define USERIPPREFIX     "uip_"        // 用户 IP
#define USERTOKENPREFIX  "utoken_"     // 用户 token：utoken_<uid>
#define IPCOUNTPREFIX    "ipcount_"    // IP 计数
#define USER_BASE_INFO   "ubaseinfo_"  // 用户基础信息
#define LOGIN_COUNT      "logincount"  // 各 chat server 的登录数（哈希）
#define LOCK_COUNT       "lockcount"   // 分布式锁计数

// 分布式锁的持有时间 / 重试时间（教程 day14+ 用）
#define LOCK_TIME_OUT 10
#define ACQUIRE_TIME_OUT 5

class Defer {
public:
	Defer(std::function<void()> func) : func_(func) {}

	~Defer() {
		func_();
	}
private:
	std::function<void()> func_;
};
