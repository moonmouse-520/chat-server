#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "const.h"

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

class CServer;

class CSession : public std::enable_shared_from_this<CSession>
{
public:
	CSession(net::io_context& ioc, CServer* server);
	~CSession();

	tcp::socket& GetSocket();
	const std::string& GetSessionId() const;

	int GetUserId() const { return _user_uid.load(); }
	void SetUserId(int uid) { _user_uid.store(uid); }

	void Start();
	void Send(const std::string& body, unsigned short msgid);
	void Close();

private:
	net::awaitable<void> ReadLoop();
	net::awaitable<void> WriteLoop();
	void HandleMsg(unsigned short msgid, const std::string& body);

	tcp::socket _socket;
	net::steady_timer _timer;
	CServer* _server;
	std::string _session_id;
	std::atomic<bool> _b_close{ false };
	std::atomic<int> _user_uid{ 0 };

	std::queue<std::string> _send_que;
	std::mutex _send_lock;
	bool _writing{ false };
};