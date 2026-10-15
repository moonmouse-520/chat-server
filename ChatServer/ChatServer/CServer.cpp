#include "CServer.h"
#include "AsioIOServicePool.h"
#include <boost/asio/error.hpp>
#include <iostream>
#include <vector>
#include <boost/asio/post.hpp>
CServer::CServer(boost::asio::io_context& ioc, unsigned short& port)
	:_ioc(ioc),
	_acceptor(ioc, tcp::endpoint(tcp::v4(), port)),
	_port(port)
{
	std::cout << "Server start success, lisening on port: " << port << std::endl;
	StartAccept();
}

void CServer::ClearSesson(const std::string& id)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_sessions.erase(id);
	std::cout << "[server] session " << id << " removed, online=" << _sessions.size() << std::endl;
}

void CServer::StartAccept()
{
	auto& io_context = AsioIOServicePool::GetInstance()->GetIOService();
	std::shared_ptr<CSession> new_session = std::make_shared<CSession>(io_context,this);
	_acceptor.async_accept(new_session->GetSocket(),std::bind(&CServer::HandleAccept, this,
		new_session, std::placeholders::_1));
}

void CServer::StopAllSessions()
{
	std::vector<std::shared_ptr<CSession>> all;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		for (const auto& kv : _sessions) {
			all.emplace_back(kv.second);
		}
	}

	for (const auto& s : all) {
		net::post(s->GetSocket().get_executor(), [s]() {
			s->Close();
			});
	}
}

void CServer::HandleAccept(std::shared_ptr<CSession> new_session, const boost::system::error_code& error)
{
	if (!error) {
		std::size_t online = 0;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_sessions.insert(std::make_pair(new_session->GetSessionId(), new_session));
			online = _sessions.size();
		}
		new_session->Start();
		std::cout << "[server] accept " << new_session->GetSessionId()
			<< ", online=" << online << std::endl;
	}
	else if (error != boost::asio::error::operation_aborted)
	{
		std::cerr << "[server] accept error: " << error.message() << std::endl;
	}

	if (error != boost::asio::error::operation_aborted) {
		StartAccept();
	}
}


