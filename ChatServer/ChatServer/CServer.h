#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "CSession.h"
using boost::asio::ip::tcp;
class CServer
{
public:
	CServer(boost::asio::io_context& ioc, unsigned short& port);
	void ClearSesson(const std::string& id);
	void StartAccept();
	void StopAllSessions();
private:
	void HandleAccept(std::shared_ptr<CSession> new_session, const boost::system::error_code&);
	tcp::acceptor _acceptor;
	net::io_context& _ioc;
	unsigned short _port;
	std::unordered_map<std::string, std::shared_ptr<CSession>> _sessions;
	std::mutex _mutex;
};

