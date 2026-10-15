#include "StatusGrpcClient.h"

#include <chrono>
#include <iostream>

StatusConPool::StatusConPool(size_t poolSize, std::string host, std::string port)
	:poolSize_(poolSize)
	,host_(host)
	,port_(port)
{
	for (size_t i = 0; i < poolSize_;i++) {
		std::shared_ptr<Channel> channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
		connections_.push(StatusService::NewStub(channel));
	}
}

StatusConPool::~StatusConPool()
{
	std::lock_guard<std::mutex> lock(mutex_);
	Close();
	while (!connections_.empty())
	{
		connections_.pop();
	}
}

std::unique_ptr<StatusService::Stub> StatusConPool::getConnection()
{
	std::unique_lock<std::mutex> lock(mutex_);
	// ★ 用 wait_for 而不是 wait：池空时无限等会把 2 个工作线程全部占死 → 整个服务假死
	bool got = cond_.wait_for(lock, std::chrono::seconds(3), [this]() {
		return b_stop_ || !connections_.empty();
		});
	if (!got) {
		std::cerr << "getConnection timeout: status pool is empty" << std::endl;
		return nullptr;
	}
	if (b_stop_ || connections_.empty()) {
		return nullptr;
	}
	auto context = std::move(connections_.front());
	connections_.pop();        
	return context;
}

void StatusConPool::Close()
{
	b_stop_ = true;
	cond_.notify_all();
}

void StatusConPool::returnConnection(std::unique_ptr<StatusService::Stub> context)
{
	if (context == nullptr) {
		return;
	}
	std::unique_lock<std::mutex> lock(mutex_);
	if (b_stop_) {
		return;
	}
	connections_.push(std::move(context));
	lock.unlock();
	cond_.notify_one();         
}

GetChatServerRsp StatusGrpcClient::GetChatServer(int uid)
{
	GetChatServerRsp reply;

	auto stub = pool_->getConnection();
	if (stub == nullptr) {                      
		std::cerr << "get status connection failed" << std::endl;
		reply.set_error(ErrorCodes::RPCFailed);
		return reply;
	}

	Defer defer([&stub, this]() {
		pool_->returnConnection(std::move(stub));
		});

	ClientContext context;
	GetChatServerReq request;
	request.set_uid(uid);

	Status status = stub->GetChatServer(&context, request, &reply);
	if (!status.ok()) {
		std::cerr << "GetChatServer rpc failed: " << status.error_message() << std::endl;
		reply.set_error(ErrorCodes::RPCFailed);
	}
	return reply;
}

LoginRsp StatusGrpcClient::Login(int uid, std::string token)
{
	LoginRsp reply;

	auto stub = pool_->getConnection();
	if (stub == nullptr) {
		std::cerr << "get status connection failed" << std::endl;
		reply.set_error(ErrorCodes::RPCFailed);
		return reply;
	}
	Defer defer([&stub, this]() {
		pool_->returnConnection(std::move(stub));
		});

	ClientContext context;
	LoginReq request;
	request.set_uid(uid);
	request.set_token(token);

	Status status = stub->Login(&context, request, &reply);
	if (!status.ok()) {
		std::cerr << "Login rpc failed: " << status.error_message() << std::endl;
		reply.set_error(ErrorCodes::RPCFailed);
	}
	return reply;
}

StatusGrpcClient::StatusGrpcClient()
{
	auto& gCfgMgr = ConfigMgr::Inst();
	std::string host = gCfgMgr["StatusServer"]["Host"];
	std::string port = gCfgMgr["StatusServer"]["Port"];   
	pool_.reset(new StatusConPool(5, host, port));
}
