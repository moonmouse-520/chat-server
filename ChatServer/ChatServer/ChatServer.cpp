#include "LogicSystem.h"
#include <csignal>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "AsioIOServicePool.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include <iostream>
#include <atomic>
#include <boost/asio.hpp>
std::atomic<bool> b_stop = false;
std::condition_variable cond_quit;
std::mutex mtx_quit;
int main()
{
    try
    {
        auto& cfg = ConfigMgr::Inst();
        boost::asio::io_context ioc;
        auto pool = AsioIOServicePool::GetInstance();
        auto port_str = cfg["SelfServer"]["Port"];
        unsigned short port = atoi(port_str.c_str());
        CServer s(ioc, port);
        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&ioc, pool,&s](auto, auto) {
            ioc.stop();
            s.StopAllSessions();
            pool->Stop();
            });

        LogicSystem::GetInstance();
       
        ioc.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    
}

