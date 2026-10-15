#include "CServer.h"
#include "HttpConnection.h"
#include "AsioIOServicePool.h"
CServer::CServer(boost::asio::io_context& ioc, unsigned short& port)
	:_ioc(ioc),
	_acceptor(ioc, tcp::endpoint(tcp::v4(), port))
{

}

void CServer::Start()
{
	auto self = shared_from_this();
	auto& io_context = AsioIOServicePool::GetInstance()->GetIOService();
	std::shared_ptr<HttpConnection> new_con = std::make_shared<HttpConnection>(io_context);
	_acceptor.async_accept(new_con->GetSocket(), [self, new_con](beast::error_code ec) {
		try
		{
			//储存放弃这个链接, 继续监听其它链接
			if (ec) {
				self->Start();
				return;
			}

			//创建新连接, 并且创建HttpConnection类管理这个链接
			new_con->Start();
			
			//继续监听
			self->Start();
		}
		catch (const std::exception& exp)
		{
			// 至少输出异常信息，方便排查
			std::cerr << "CServer accept exception: " << exp.what() << std::endl;
			self->Start(); // 异常也不能停服务，继续监听
		}
		});
}
