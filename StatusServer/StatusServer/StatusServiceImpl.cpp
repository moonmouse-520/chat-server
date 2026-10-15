#include "StatusServiceImpl.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include <climits>
#include <sstream>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

// 生成唯一 token
std::string generate_unique_string() {
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	return boost::uuids::to_string(uuid);
}

StatusServiceImpl::StatusServiceImpl()
{
	auto& cfg = ConfigMgr::Inst();

	// 从 config.ini 的 [chatservers] Name=xxx,yyy 读出所有 chat server 的名字
	std::string server_list = cfg["chatservers"]["Name"];
	std::vector<std::string> words;

	std::stringstream ss(server_list);
	std::string word;
	while (std::getline(ss, word, ',')) {
		words.push_back(word);
	}

	for (auto& w : words) {
		if (cfg[w]["Name"].empty()) {
			continue;
		}
		ChatServer server;
		server.port = cfg[w]["Port"];
		server.host = cfg[w]["Host"];
		server.name = cfg[w]["Name"];
		_servers[server.name] = server;
	}

	if (_servers.empty()) {
		std::cerr << "[warn] no chatserver in config.ini! check [chatservers] section" << std::endl;
	}
	std::cout << "chatservers loaded: " << _servers.size() << std::endl;
}

ChatServer StatusServiceImpl::getChatServer() {
	std::lock_guard<std::mutex> guard(_server_mtx);

	if (_servers.empty()) {        
		return ChatServer();
	}
	return _servers.begin()->second;
}

Status StatusServiceImpl::GetChatServer(ServerContext* context, const GetChatServerReq* request, GetChatServerRsp* reply)
{
	const auto& server = getChatServer();
	reply->set_host(server.host);
	reply->set_port(server.port);
	reply->set_error(ErrorCodes::Success);
	reply->set_token(generate_unique_string());
	insertToken(request->uid(), reply->token());

	std::cout << "GetChatServer uid = " << request->uid()
		<< " -> " << server.host << ":" << server.port << std::endl;
	return Status::OK;
}

Status StatusServiceImpl::Login(ServerContext* context, const LoginReq* request, LoginRsp* reply)
{
	auto uid = request->uid();
	auto token = request->token();

	std::string uid_str = std::to_string(uid);
	std::string token_key = USERTOKENPREFIX + uid_str;
	std::string token_value = "";

	bool success = RedisMgr::GetInstance()->Get(token_key, token_value);
	if (!success) {
		std::cout << "uid " << uid << " has no token in redis" << std::endl;
		reply->set_error(ErrorCodes::UidInvalid);      // 1011
		return Status::OK;
	}

	if (token_value != token) {
		std::cout << "token not match, uid = " << uid << std::endl;
		reply->set_error(ErrorCodes::TokenInvalid);    // 1010
		return Status::OK;
	}

	reply->set_error(ErrorCodes::Success);
	reply->set_uid(uid);
	reply->set_token(token);
	std::cout << "Login ok, uid = " << uid << std::endl;
	return Status::OK;
}

void StatusServiceImpl::insertToken(int uid, std::string token)
{
	std::string uid_str = std::to_string(uid);
	std::string token_key = USERTOKENPREFIX + uid_str;
	RedisMgr::GetInstance()->Set(token_key, token);
}
