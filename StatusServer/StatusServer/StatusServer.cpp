#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <boost/asio.hpp>
#include "const.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "MysqlMgr.h"
#include "AsioIOServicePool.h"
#include "StatusServiceImpl.h"

// 启动 gRPC 服务端（监听 config.ini 里 [StatusServer] Host:Port，默认 0.0.0.0:50052）
void RunServer() {
	auto& cfg = ConfigMgr::Inst();

	std::string server_address(cfg["StatusServer"]["Host"] + ":" + cfg["StatusServer"]["Port"]);
	StatusServiceImpl service;

	grpc::ServerBuilder builder;
	// 监听端口和添加服务
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
	builder.RegisterService(&service);

	// 构建并启动 gRPC 服务器
	std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
	std::cout << "StatusServer listening on " << server_address << std::endl;

	// 用 asio 的 signal_set 捕获 Ctrl+C，优雅关闭
	boost::asio::io_context io_context;
	boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
	signals.async_wait([&server, &io_context](const boost::system::error_code& error, int signal_number) {
		if (!error) {
			std::cout << "Shutting down server..." << std::endl;
			server->Shutdown();   // 优雅地关闭服务器（不再接新请求，等已有请求处理完）
			io_context.stop();
		}
		});

	// io_context 放到独立线程跑（不阻塞 server->Wait()）
	std::thread([&io_context]() { io_context.run(); }).detach();

	// 等待服务器关闭
	server->Wait();
}

int main(int argc, char** argv) {
	try {
		RunServer();
	}
	catch (std::exception const& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}
	return 0;
}
