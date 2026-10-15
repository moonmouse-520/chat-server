#include "CSession.h"
#include "CServer.h"
#include <array>
#include <iostream>
#include <string>
#include <chrono>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/write.hpp>
#include "LogicSystem.h"
#include "UserMgr.h"
#include <thread>
namespace {
	constexpr int kIdleTimeoutSec = 300;		// 空闲兜底(心跳上线后收紧到 90)
	constexpr int kHalfPacketTimeoutSec = 5;	// 半包超时: 防 Slowloris
}

CSession::CSession(net::io_context& ioc, CServer* server)
	: _socket(ioc)
	, _timer(_socket.get_executor())
	, _server(server)
{
	auto a_uuid = boost::uuids::random_generator()();
	_session_id = boost::uuids::to_string(a_uuid);
	std::cout << "[session] create " << _session_id << std::endl;
}

CSession::~CSession()
{
	std::cout << "[session] destroy " << _session_id << std::endl;
}

tcp::socket& CSession::GetSocket()
{
	return _socket;
}

const std::string& CSession::GetSessionId() const
{
	return _session_id;
}

void CSession::Start()
{
	auto self = shared_from_this();
	net::co_spawn(_socket.get_executor(), [self]()->net::awaitable<void> {
		co_await self->ReadLoop();
		},
		net::detached);
}

void CSession::Send(const std::string& body, unsigned short msgid)
{
	if (_b_close.load()) {
		return;
	}

	if (body.size() > 65535) {
		std::cout << "[session] " << _session_id << " send too big: " << body.size() << std::endl;
		return;
	}

	std::string frame;
	frame.reserve(HEAD_TOTAL_LEN + body.size());
	frame.append(HEAD_TOTAL_LEN, '\0');
	frame += body;

	unsigned short len = static_cast<unsigned short>(body.size());
	frame[0] = static_cast<char>((msgid >> 8) & 0xFF);   
	frame[1] = static_cast<char>(msgid & 0xFF);         
	frame[2] = static_cast<char>((len >> 8) & 0xFF);    
	frame[3] = static_cast<char>(len & 0xFF);			

	bool overflow = false;
	bool need_start = false;
	{
		std::lock_guard<std::mutex> lock(_send_lock);
		if (_send_que.size() > MAX_SENDQUE) {
			overflow = true;
		}
		else
		{
			_send_que.push(std::move(frame));
			if (!_writing) {
				need_start = true;
				_writing = true;
			}
		}
	}

	if (overflow)
	{
		std::cout << "[session] " << _session_id << " send queue full, close" << std::endl;
		Close();
		return;
	}

	if (need_start) {
		net::co_spawn(_socket.get_executor(), [self = shared_from_this()]() -> net::awaitable<void> {
			co_await self->WriteLoop();
			},
			net::detached);
	}
}

void CSession::Close()
{
	if (_b_close.exchange(true)) {
		return;
	}
	
	_timer.cancel();
	boost::system::error_code ec;
	_socket.shutdown(tcp::socket::shutdown_both, ec);
	_socket.close(ec);

	int uid = _user_uid.load();
	if (uid != 0) {
		UserMgr::GetInstance()->RemoveUser(uid);
	}

	if (_server) {
		_server->ClearSesson(_session_id);
	}

	std::cout << "[session] " << _session_id << " closed" << std::endl;
}

net::awaitable<void> CSession::ReadLoop()
{
	auto self = shared_from_this();
	boost::system::error_code ec;
	std::array<unsigned char, HEAD_TOTAL_LEN> head{};

	for (;;) {
		// ---- 读头之前: 只装"空闲兜底"(300 秒) ----
		//  为什么: 聊天长连接"静默"是常态(用户在看列表、在发呆), 60 秒就踢是误杀。
		//  300 秒只是防止半开连接永远占坑; 真正的死连接检测留给心跳
		_timer.expires_after(std::chrono::seconds(kIdleTimeoutSec));
		_timer.async_wait([self](const boost::system::error_code tec) {
			if (!tec && !self->_b_close.load()) {
				std::cout << "[session] " << self->_session_id << " read timeout" << std::endl;
				self->Close();
			}
			});
		co_await net::async_read(_socket, net::buffer(head), net::redirect_error(net::use_awaitable, ec));
		if (ec) {
			std::cout << "[session] " << _session_id << ", read head: " << ec.message() << std::endl;
			break;
		}
		unsigned short msgid = static_cast<unsigned short>((head[0] << 8) | head[1]);
		unsigned short body_len = static_cast<unsigned short>(head[2] << 8 | head[3]);

		if (body_len > MAX_LENGTH)
		{
			std::cout << "[session] " << _session_id << " bad frame len=" << body_len
				<< " > MAX_LENGTH=" << MAX_LENGTH << std::endl;
			break;                      
		}
		
		// ---- 读体之前: 把超时收紧成"半包 5 秒" ----
		//  这才是有攻击面的地方: 头里声明了长度却慢慢发, 用极小流量占住连接(Slowloris)。
		//  expires_after 会隐式取消上一个还没到期的等待, 所以直接重新装即可。
		if (body_len > 0) {
			_timer.expires_after(std::chrono::seconds(kHalfPacketTimeoutSec));
			std::string body;
			body.resize(body_len);
			co_await net::async_read(_socket, net::buffer(body), net::redirect_error(net::use_awaitable, ec));
			if (ec) {
				std::cout << "[session] " << _session_id << ", read body: " << ec.message() << std::endl;
				break;
			}
			_timer.cancel();
			HandleMsg(msgid, body);
			continue;
		}
		_timer.cancel();
		HandleMsg(msgid, std::string());
	}
	Close();
}

net::awaitable<void> CSession::WriteLoop()
{
	boost::system::error_code ec;
	auto self = shared_from_this();
	for (;;) {
		std::string frame;
		{
			std::lock_guard<std::mutex> lock(_send_lock);
			if (_send_que.empty()) {
				_writing = false;
				co_return;
			}
			frame = std::move(_send_que.front());
			_send_que.pop();
		}

		co_await(net::async_write(_socket, net::buffer(frame), net::redirect_error(net::use_awaitable, ec)));

		if (ec) {
			std::cout << "[session] " << _session_id << " write: " << ec.message() << std::endl;
			Close();
			co_return;
		}
		std::cout << "[session] " << _session_id << " sent " << frame.size() << " bytes" << std::endl;
	}
	
}

void CSession::HandleMsg(unsigned short msgid, const std::string& body)
{
	std::cout << "[session] " << _session_id << " recv id=" << msgid
		<< " len=" << body.size() << " body=" << body << std::endl;
	LogicSystem::GetInstance()->PostMsgToQue(shared_from_this(), msgid, body);
}
