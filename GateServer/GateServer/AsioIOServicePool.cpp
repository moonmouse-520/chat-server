#include "AsioIOServicePool.h"

AsioIOServicePool::~AsioIOServicePool()
{
	Stop();
	std::cout << "AsioIOServicePool is distructed!" << std::endl;
}

boost::asio::io_context& AsioIOServicePool::GetIOService()
{
	auto& service = _ioServices[_nextIOService++];
	if (_nextIOService == _ioServices.size()) {
		_nextIOService = 0;
	}
	return service;
}

void AsioIOServicePool::Stop()
{
	for (auto& work : _works) {
		if (work) 
		{
			work->reset();
		}
	}

	// 等待所有工作线程退出
	for (auto& t : _threads) {
		if (t.joinable()) 
		{
			t.join();
		}
	}
}

AsioIOServicePool::AsioIOServicePool(std::size_t size)
	: _ioServices(size)
	, _works(size)
	,_nextIOService(0)
{
	for (size_t i = 0; i < size; i++)
	{	
		_works[i] = std::make_unique<Work>(boost::asio::make_work_guard(_ioServices[i]));
	}

	for (size_t i = 0; i < _ioServices.size(); i++)
	{
		_threads.emplace_back([this, i]() {
			_ioServices[i].run();
			});
	}
}

