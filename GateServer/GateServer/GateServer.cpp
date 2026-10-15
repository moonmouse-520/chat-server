#include <iostream>
#include <string>
#include <json.hpp>
#include "CServer.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "MysqlMgr.h"
using json = nlohmann::json;

void testRedis() {
	try {
		sw::redis::Redis redis("tcp://127.0.0.1:6380");
		redis.auth("123456");            // ← 有密码必须 auth
		redis.set("test_key", "hello_redis");
		auto val = redis.get("test_key");
		std::cout << "get = " << (val ? *val : "(null)") << std::endl;
	}
	catch (const std::exception& e) {
		std::cerr << "redis error: " << e.what() << std::endl;
	}
}
void TestRedisMgr() {
	auto redis = RedisMgr::GetInstance();
	std::cout << "===== TestRedisMgr start =====" << std::endl;
	redis->Set("test_key", "hello_redis");
	std::string val;
	if (redis->Get("test_key", val)) {
		std::cout << "[OK] Get test_key = " << val << " (expect hello_redis)" << std::endl;
	}
	else {
		std::cout << "[FAIL] Get test_key returned false" << std::endl;
	}
	// ② 存在性：已存在的键 vs 不存在的键，两种都要测
	std::cout << "[INFO] ExistsKey(test_key)    = " << redis->ExistsKey("test_key")
		<< " (expect 1)" << std::endl;
	std::cout << "[INFO] ExistsKey(no_such_key) = " << redis->ExistsKey("no_such_key")
		<< " (expect 0)" << std::endl;
	//带过期写入 + 查剩余时间（验证码就靠这个）
	redis->Setex("test_code", "1234", 10);
	std::this_thread::sleep_for(std::chrono::seconds(3));
	std::cout << "[INFO] TTL(test_code) = " << redis->TTL("test_code")
		<< " (expect ~10)" << std::endl;
	// ④ Get 不存在的键：应返回 false（边界，不能崩）
	std::string empty;
	std::cout << "[INFO] Get(no_such_key) = " << redis->Get("no_such_key", empty)
		<< " (expect 0/false)" << std::endl;

	// ⑤ TTL 的两个特殊返回值
	std::cout << "[INFO] TTL(no_such_key) = " << redis->TTL("no_such_key")
		<< " (expect -2 = 键不存在)" << std::endl;
	redis->Set("no_expire_key", "x");           // 不设过期
	std::cout << "[INFO] TTL(no_expire_key) = " << redis->TTL("no_expire_key")
		<< " (expect -1 = 无过期)" << std::endl;

	// ⑥ Del 生效验证
	redis->Del("test_key");
	std::cout << "[INFO] after Del, ExistsKey(test_key) = " << redis->ExistsKey("test_key")
		<< " (expect 0)" << std::endl;
	redis->Del("no_expire_key");
	// ⑦ 列表：压入两个，弹出一个（LPop 是"后进先出"）
	redis->LPush("test_list", "first");
	redis->LPush("test_list", "second");
	std::string pop_val;
	if (redis->LPop("test_list", pop_val)) {
		std::cout << "[OK] LPop = " << pop_val << " (expect second)" << std::endl;
	}
	redis->Del("test_list");     // 清掉测试数据
	// ⑧ 哈希：一个 key 下多个字段（类似"对象"）
	redis->HSet("test_hash", "name", "zack");
	redis->HSet("test_hash", "age", "18");
	std::string hval;
	if (redis->HGet("test_hash", "name", hval)) {
		std::cout << "[OK] HGet name = " << hval << " (expect zack)" << std::endl;
	}
	redis->Del("test_hash");

	std::cout << "===== TestRedisMgr end =====" << std::endl;
}

int main()
{
	//TestRedisMgr();
	try
	{
		auto& gCfgMgr = ConfigMgr::Inst();
		std::string gate_port_str = gCfgMgr["GateServer"]["Port"];
		unsigned short gate_port = atoi(gate_port_str.c_str());

		net::io_context ioc{ 1 };
		boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
		signals.async_wait([&ioc](const boost::system::error_code ec, int signal_number) {
			if (ec) return;
			ioc.stop();
			});
		// 数据库自检：启动日志里一眼看出 MySQL 通不通（fail fast）
		if (MysqlMgr::GetInstance()->Ping()) {
			std::cout << "mysql connect ok" << std::endl;
		}
		else {
			std::cerr << "mysql connect failed, /user_register will return DBErr(1012)" << std::endl;
		}

		std::make_shared<CServer>(ioc, gate_port)->Start();
		std::cout << "GateServer is listening on port: " << gate_port << std::endl;
		ioc.run();

	}
	catch (const std::exception& exp)
	{
		std::cout << "error code is " << exp.what() << std::endl;
		return EXIT_FAILURE;
	}
    return 0;
}