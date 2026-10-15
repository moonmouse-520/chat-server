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
	DBErr = 1012,			//数据库操作失败（拿不到连接/SQL异常）★ 1010/1011 留给教程的 TokenInvalid/UidInvalid（day12+），别占号

};

// 验证码在 Redis 里的 key 前缀：code_<email>
// ★ 必须和 VarifyServer/config.js 的 code_prefix 完全一致，否则取不到缓存
#define CODEPREFIX "code_"
// Defer 类：把"收尾动作"绑定到作用域（教程用它归还数据库连接）
class Defer {
public:
	// 接受一个lambda表达式或者函数指针
	Defer(std::function<void()> func) : func_(func) {}

	// 析构函数中执行传入的函数
	~Defer() {
		func_();
	}
private:
	std::function<void()> func_;
};

