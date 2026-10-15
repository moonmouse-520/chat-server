#include "LogicSystem.h"
#include "HttpConnection.h"
#include "VarifGrpcClient.h"
#include "RedisMgr.h"
#include "MysqlMgr.h"
#include "const.h"
#include "StatusGrpcClient.h"
bool LogicSystem::HandleGet(std::string path, std::shared_ptr<HttpConnection> con)
{
	if (_get_handlers.find(path) == _get_handlers.end()) {
		return false;
	}
	_get_handlers[path](con);
	return true;
}

void LogicSystem::RegGet(std::string url, HttpHandler handler)
{
	_get_handlers.insert(std::make_pair(url, handler));
}

void LogicSystem::RegPost(std::string url, HttpHandler handler)
{
	_post_handlers.insert(std::make_pair(url, handler));
}

bool LogicSystem::HandlePost(std::string path, std::shared_ptr<HttpConnection> con)
{
	if (_post_handlers.find(path) == _post_handlers.end()) {
		return false;
	}
	_post_handlers[path](con);
	return true;
}

LogicSystem::LogicSystem()
{
	RegGet("/get_test", [](std::shared_ptr<HttpConnection> connection) {
		beast::ostream(connection->_response.body()) << "receive get_test req\r\n";
		int i = 0;
		for (auto& elem : connection->_get_params) {
			i++;
			beast::ostream(connection->_response.body())
				<< "parm" << i << " key is: " << elem.first
				<< ", value is: " << elem.second << "\r\n";
		}
		});

	RegPost("/get_varifycode", [](std::shared_ptr<HttpConnection> connection) {
		auto body_string = boost::beast::buffers_to_string(connection->_request.body().data());
		std::cout << "receive body is: " << body_string << std::endl;
		connection->_response.set(http::field::content_type, "application/json");
		Json root = Json::parse(body_string, nullptr, false);
		Json error_resp;
		bool parse_success = !root.is_discarded();
		if (!parse_success)
		{
			std::cout << "Failed to parse Json data!" << std::endl;
			error_resp["error"] = ErrorCodes::Error_Json;
			std::string jsonstr = error_resp.dump();
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		
		if (!root.contains("email")) {
			//key 不存在
			std::cout << "Failed to parse Json data!" << std::endl;
			error_resp["error"] = ErrorCodes::Error_Json;
			std::string jsonstr = error_resp.dump();
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		auto email = root["email"].get<std::string>();
		if (email.empty())
		{
			Json error_resp;
			error_resp["error"] = ErrorCodes::Error_Json;  
			error_resp["detail"] = "email is empty";
			beast::ostream(connection->_response.body()) << error_resp.dump();
			return true;
		}
		else
		{
			std::cout << "email is: " << email;
		}
		GetVarifyRsp rsp = VarifGrpcClient::GetInstance()->GetVarifyCode(email);

		Json resp;
		resp["error"] = rsp.error();
		resp["email"] = email;
		auto jsonStr = resp.dump();
		beast::ostream(connection->_response.body()) << jsonStr;
		return true;
		
		
		});
	RegPost("/user_register", [](std::shared_ptr<HttpConnection> connection) {
		auto body_str = beast::buffers_to_string(connection->_request.body().data());
		std::cout << "receive body is: " << body_str << std::endl;
		connection->_response.set(http::field::content_type, "application/json");
		Json root;
		try
		{
			Json src_root = Json::parse(body_str, nullptr, false);
			bool parse_success = !src_root.is_discarded();
			if (!parse_success) {
				root["error"] = ErrorCodes::Error_Json;
				beast::ostream(connection->_response.body()) << root.dump();
				return true;
			}

			for (const char* key : { "user", "email", "passwd", "confirm", "varifycode" })
			{
				if (!src_root.contains(key))
				{
					root["error"] = ErrorCodes::Error_Json;
					root["detail"] = std::string("missing field key: ") + key;
					beast::ostream(connection->_response.body()) << root.dump();
					return true;
				}
			}

			auto email = src_root["email"].get<std::string>();
			auto name = src_root["user"].get<std::string>();
			auto pwd = src_root["passwd"].get<std::string>();
			auto confirm = src_root["confirm"].get<std::string>();
			auto varifycode = src_root["varifycode"].get<std::string>();

			if (pwd != confirm) {
				std::cout << "password err" << std::endl;
				root["error"] = ErrorCodes::PasswdErr;
				auto jsonstr = root.dump();
				beast::ostream(connection->_response.body()) << jsonstr;
				return true;
			}

			// ① 校验验证码：key 必须和 VarifyServer 写入时一致 = CODEPREFIX + email
			std::string cached_code;
			if (!RedisMgr::GetInstance()->Get(CODEPREFIX + email, cached_code)) {
				std::cout << "varify code expired, email = " << email << std::endl;
				root["error"] = ErrorCodes::VarifyExpired;      // 1003
				beast::ostream(connection->_response.body()) << root.dump();
				return true;
			}
			if (cached_code != varifycode) {
				std::cout << "varify code error" << std::endl;
				root["error"] = ErrorCodes::VarifyCodeErr;      // 1004
				beast::ostream(connection->_response.body()) << root.dump();
				return true;
			}

			// ② 落库：查重 + 发号 + 插入（MysqlDao 内部一个事务完成）
			int uid = MysqlMgr::GetInstance()->RegUser(name, email, pwd);
			if (uid == 0) {
				std::cout << "user or email exist" << std::endl;
				root["error"] = ErrorCodes::UserExist;          // 1005
				beast::ostream(connection->_response.body()) << root.dump();
				return true;
			}
			if (uid < 0) {
				std::cout << "reg user failed, db error" << std::endl;
				root["error"] = ErrorCodes::DBErr;              // 1012
				beast::ostream(connection->_response.body()) << root.dump();
				return true;
			}

			// ③ 成功（★ 不回显 passwd/confirm：密码没有任何理由回给客户端）
			root["error"] = 0;
			root["uid"] = uid;
			root["email"] = email;
			root["user"] = name;
			std::string jsonstr = root.dump();
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}
		catch (const std::exception& e)
		{
			root["error"] = ErrorCodes::Error_Json;
			root["detail"] = e.what();
			beast::ostream(connection->_response.body()) << root.dump();
			std::cerr << "user_register exception: " << e.what() << std::endl;
		}
		return true;
		});
	RegPost("/reset_pwd", [](std::shared_ptr<HttpConnection> connection) {
		auto body_str = beast::buffers_to_string(connection->_request.body().data());
		std::cout << "receive data is " << body_str << std::endl;
		connection->_response.set(http::field::content_type, "application/json");
		Json root;
		try
		{
			Json src_root = Json::parse(body_str, nullptr, false);
			if (src_root.is_discarded()) {
				root["error"] = ErrorCodes::Error_Json;
				beast::ostream(connection->_response.body()) << root.dump();
				return true;
			}

			for (const char* key : { "user", "email", "passwd", "varifycode" })
			{
				if (!src_root.contains(key))
				{
					root["error"] = ErrorCodes::Error_Json;
					root["detail"] = std::string("missing field key: ") + key;
					beast::ostream(connection->_response.body()) << root.dump();
					return true;
				}
			}

			auto email = src_root["email"].get<std::string>();
			auto name = src_root["user"].get<std::string>();
			auto pwd = src_root["passwd"].get<std::string>();
			auto varifycode = src_root["varifycode"].get<std::string>();

			std::string cached_code;
			if (!RedisMgr::GetInstance()->Get(CODEPREFIX + email, cached_code)) {
				std::cout << "varify code expired, email = " << email << std::endl;
				root["error"] = ErrorCodes::VarifyExpired;      // 1003
				beast::ostream(connection->_response.body()) << root.dump();
				return true;
			}

			if (cached_code != varifycode) {
				std::cout << "varify code error" << std::endl;
				root["error"] = ErrorCodes::VarifyCodeErr;      // 1004
				beast::ostream(connection->_response.body()) << root.dump();
				return true;
			}

			bool email_valid = MysqlMgr::GetInstance()->CheckEmail(name, email);
			if (!email_valid) {
				std::cout << "user email not match" << std::endl;
				root["error"] = ErrorCodes::EmailNotMatch;
				auto jsonstr = root.dump();
				beast::ostream(connection->_response.body()) << jsonstr;
				return true;
			}

			bool b_up = MysqlMgr::GetInstance()->UpdatePwd(name, pwd);
			if (!b_up) {
				std::cout << "update pwd failed" << std::endl;
				root["error"] = ErrorCodes::PasswdUpFailed;
				auto jsonstr = root.dump();
				beast::ostream(connection->_response.body()) << jsonstr;
				return true;
			}

			std::cout << "update pwd success!" << std::endl;

			root["error"] = 0;
			root["email"] = email;
			root["user"] = name;
			std::string jsonstr = root.dump();
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}
		catch (const std::exception& e)
		{
			root["error"] = ErrorCodes::Error_Json;
			root["detail"] = e.what();
			beast::ostream(connection->_response.body()) << root.dump();
			std::cerr << "reset_passwd exception: " << e.what() << std::endl;
		}
		return true;
		});
	RegPost("/user_login", [](std::shared_ptr<HttpConnection> connection) {
		auto body_str = beast::buffers_to_string(connection->_request.body().data());
		std::cout << "receive data is " << body_str << std::endl;
		connection->_response.set(http::field::content_type, "application/json");
		Json root;
		try
		{
			Json src_root = Json::parse(body_str, nullptr, false);
			if (src_root.is_discarded()) {
				root["error"] = ErrorCodes::Error_Json;
				beast::ostream(connection->_response.body()) << root.dump();
				return true;
			}

			for (const char* key : { "email", "passwd" })
			{
				if (!src_root.contains(key))
				{
					root["error"] = ErrorCodes::Error_Json;
					root["detail"] = std::string("missing field key: ") + key;
					beast::ostream(connection->_response.body()) << root.dump();
					return true;
				}
			}

			auto email = src_root["email"].get<std::string>();
			auto pwd = src_root["passwd"].get<std::string>();
			UserInfo userInfo;
			bool pwd_valid = MysqlMgr::GetInstance()->CheckPwd(email,pwd,userInfo);
			if (!pwd_valid) {
				std::cout << "user pwd not match" << std::endl;
				root["error"] = ErrorCodes::PasswdInvalid;
				auto jsonstr = root.dump();
				beast::ostream(connection->_response.body()) << jsonstr;
				return true;
			}

			auto reply = StatusGrpcClient::GetInstance()->GetChatServer(userInfo.uid);
			if (reply.error()) {
				std::cout << "grpc get chat server failed, error is " << reply.error() << std::endl;
				root["error"] = ErrorCodes::RPCFailed;
				auto jsonstr = root.dump();
				beast::ostream(connection->_response.body()) << jsonstr;
				return true;
			}

			std::cout << "succeed to load userinfo uid is!"<<userInfo.uid << std::endl;

			root["error"] = 0;
			root["email"] = email;
			root["uid"] = userInfo.uid;
			root["token"] = reply.token();
			root["host"] = reply.host();
			root["port"] = reply.port();
			std::string jsonstr = root.dump();
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}
		catch (const std::exception& e)
		{
			root["error"] = ErrorCodes::Error_Json;
			root["detail"] = e.what();
			beast::ostream(connection->_response.body()) << root.dump();
			std::cerr << "user_login exception: " << e.what() << std::endl;
		}
		return true;


		});
}
