#include "HttpConnection.h"
#include "LogicSystem.h"
HttpConnection::HttpConnection(boost::asio::io_context& ioc)
	:_socket(ioc)
{

}


void HttpConnection::Start()
{
	auto self = shared_from_this();
	http::async_read(_socket, _buffer, _request, [self](beast::error_code ec, std::size_t bytes_transferred) {
		try
		{
			if (ec) {
				std::cout << "Http read error is:" << ec.what() << std::endl;
				return;
			}

			boost::ignore_unused(bytes_transferred);
			self->CheckDeadline();
			self->HandleReq();

		}
		catch (const std::exception& exp)
		{
			std::cout << "excption is " << exp.what() << std::endl;
		}
		});
}

void HttpConnection::CheckDeadline()
{
	auto self = shared_from_this();
	deadline_.async_wait([self](beast::error_code ec) {
		if (!ec) {
			self->_socket.close(ec);
		}
		});
}

void HttpConnection::WriteResponse()
{
	auto self = shared_from_this();
	_response.content_length(_response.body().size());
	http::async_write(_socket, _response, [self](beast::error_code ec, std::size_t bytes_transferred) {
		self->_socket.shutdown(tcp::socket::shutdown_send, ec);
		self->deadline_.cancel();
		});
}

unsigned char ToHex(unsigned char x)
{
	return x > 9 ? x + 55 : x + 48;
}

unsigned char FromHex(unsigned char x)
{
	unsigned char y = 0;
	if (x >= 'A' && x <= 'Z') {
		y = x - 'A' + 10;
	}
	else if (x >= 'a' && x <= 'z') {  // >= 'a'，修复bug
		y = x - 'a' + 10;
	}
	else if (x >= '0' && x <= '9') {
		y = x - '0';
	}
	else {
		// 非法16进制字符，url解码遇到错误，返回0xff作为错误标记
		y = 0xff;
	}
	return y;
}

std::string UrlEncode(const std::string& str)
{
	std::string strTemp = "";
	size_t length = str.length();
	for (size_t i = 0; i < length; i++)
	{
		//判断是否仅有数字和字母构成
		if (isalnum((unsigned char)str[i]) ||
			(str[i] == '-') ||
			(str[i] == '_') ||
			(str[i] == '~') ||
			(str[i] == '.'))
			strTemp += str[i];
		else if (str[i] == ' ')//空字符
			strTemp += "+";
		else
		{
			//其它字符需要提前加%并且高四位和低四位分别转为16进制
			strTemp += '%';
			strTemp += ToHex((unsigned char)str[i] >> 4);
			strTemp += ToHex((unsigned char)str[i] & 0x0F);
		}

	}
	return strTemp;

}

std::string UrlDecode(const std::string& str)
{
	std::string strTemp;
	size_t length = str.length();
	for (size_t i = 0; i < length; ++i)
	{
		if (str[i] == '+')
		{
			strTemp += ' ';
		}
		else if (str[i] == '%')
		{
			// 剩余字符不够两个，直接退出，不用assert
			if (i + 2 >= length)
				break;

			unsigned char high = FromHex(static_cast<unsigned char>(str[i + 1]));
			unsigned char low = FromHex(static_cast<unsigned char>(str[i + 2]));

			//非法十六进制字符直接跳过
			if (high == 0xff || low == 0xff)
				continue;

			unsigned char byte = static_cast<unsigned char>(high * 16 + low);
			strTemp += static_cast<char>(byte);
			i += 2; //跳过后面两个字符
		}
		else
		{
			strTemp += str[i];
		}
	}
	return strTemp;
}

void HttpConnection::HandleReq()
{
	//设置版本
	_response.version(_request.version());
	_request.keep_alive(false);
	if (_request.method() == http::verb::get) {
		PreParseGetParam();
		bool success = LogicSystem::GetInstance()->HandleGet(_get_url, shared_from_this());
		if (!success) {
			_response.result(http::status::not_found);
			_response.set(http::field::content_type, "text/plain");
			beast::ostream(_response.body()) << "url not found\r\n";
			WriteResponse();
			return;
		}

		_response.result(http::status::ok);
		_response.set(http::field::server, "GetServer");
		WriteResponse();
		return;
	}

	if (_request.method() == http::verb::post) {
		bool success = LogicSystem::GetInstance()->HandlePost(_request.target(), shared_from_this());
		if (!success) {
			_response.result(http::status::not_found);
			_response.set(http::field::content_type, "text/plain");
			beast::ostream(_response.body()) << "url not found\r\n";
			WriteResponse();
			return;
		}

		_response.result(http::status::ok);
		_response.set(http::field::server, "GetServer");
		WriteResponse();
		return;
	}
}

void HttpConnection::PreParseGetParam() {
	// 提取 URI
	auto uri = _request.target();
	// 查找查询字符串的开始位置（即 '?' 的位置）
	auto query_pos = uri.find('?');
	if (query_pos == std::string::npos) {
		_get_url = uri;
		return;
	}

	_get_url = uri.substr(0, query_pos);
	std::string query_string = uri.substr(query_pos + 1);
	std::string key;
	std::string value;
	size_t pos = 0;
	while ((pos = query_string.find('&')) != std::string::npos) {
		auto pair = query_string.substr(0, pos);
		size_t eq_pos = pair.find('=');
		if (eq_pos != std::string::npos) {
			key = UrlDecode(pair.substr(0, eq_pos)); // 假设有 url_decode 函数来处理URL解码
			value = UrlDecode(pair.substr(eq_pos + 1));
			_get_params[key] = value;
		}
		query_string.erase(0, pos + 1);
	}
	// 处理最后一个参数对（如果没有 & 分隔符）
	if (!query_string.empty()) {
		size_t eq_pos = query_string.find('=');
		if (eq_pos != std::string::npos) {
			key = UrlDecode(query_string.substr(0, eq_pos));
			value = UrlDecode(query_string.substr(eq_pos + 1));
			_get_params[key] = value;
		}
	}
}