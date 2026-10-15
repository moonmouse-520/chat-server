#include "RedisMgr.h"
#include "ConfigMgr.h"
RedisMgr::RedisMgr()
{
	//从config.ini读配置
	auto& cfg = ConfigMgr::Inst();
	std::string host = cfg["Redis"]["Host"];
	std::string port = cfg["Redis"]["Port"];
	std::string passwd = cfg["Redis"]["Passwd"];
	//redis连接选项对象
	sw::redis::ConnectionOptions conn_opts;
	conn_opts.host = host;
	conn_opts.port = std::stoi(port);

	if (!passwd.empty()) {
		conn_opts.password = passwd;             // 空密码就不设（避免空串认证失败）
	}
	//连接池选项 --线程安全的关键
	sw::redis::ConnectionPoolOptions pool_opts;
	pool_opts.size = 5;
	//创建+立即验证
	try
	{
		_redis = std::make_unique<sw::redis::Redis>(conn_opts, pool_opts);
		_redis->ping();			//// 懒连接：ping 一次暴露配置错误
		std::cout << "redis connected on " << host << ":" << port << std::endl;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis ini failed: " <<e.what()<< std::endl;
		_redis.reset();  // 失败置空，后续调用会被 !_redis 挡住
	}

}

bool RedisMgr::Setex(const std::string& key, const std::string& value, long long seconds)
{
	if (!_redis) return false;
	try
	{
		_redis->set(key, value, std::chrono::seconds(seconds));
		return true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis setex error: " << e.what() << std::endl;
		return false;
	}
}

bool RedisMgr::Set(const std::string& key, const std::string& value)
{
	if (!_redis) return false;
	try
	{
		_redis->set(key, value);
		return true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis set error: " << e.what() << std::endl;
		return false;
	}
}

bool RedisMgr::Get(const std::string& key, std::string& value)
{
	if (!_redis) return false;
	try
	{
		auto val = _redis->get(key); //返回 Optional<string>
		if (!val) return false;
		value = *val;
		return true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis get error: " << e.what() << std::endl;
		return false;
	}
}

bool RedisMgr::Del(const std::string& key)
{
	if (!_redis) return false;
	try
	{
		_redis->del(key);
		return true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis del error: " << e.what() << std::endl;
		return false;
	}
	return false;
}

bool RedisMgr::ExistsKey(const std::string& key)
{
	if (!_redis) return false;
	try
	{
		return _redis->exists(key) > 0; 
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis exists error: " << e.what() << std::endl;
		return false;
	}
	return false;
}


long long RedisMgr::TTL(const std::string& key) //-2 = 键不存在，-1 = 存在但没设过期，正数 = 剩余秒数。
{
	if (!_redis) return -2;
	try 
	{
		return _redis->ttl(key); 
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis ttl error: " << e.what() << std::endl; 
		return -2;
	}
}

bool RedisMgr::LPush(const std::string& key, const std::string& value)
{
	if (!_redis) return false;
	try
	{
		_redis->lpush(key, value);
		return true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis lpush error: " << e.what() << std::endl;
		return false;
	}
}

bool RedisMgr::LPop(const std::string& key, std::string& value)
{
	if (!_redis) return false;
	try
	{
		auto val = _redis->lpop(key);
		if (!val) return false;
		value = *val;
		return true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis lpop error: " << e.what() << std::endl;
		return false;
	}
}

bool RedisMgr::RPush(const std::string& key, const std::string& value)
{
	if (!_redis) return false;
	try
	{
		_redis->rpush(key, value);
		return true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis rpush error: " << e.what() << std::endl; 
		return false;
	}
}

bool RedisMgr::RPop(const std::string& key, std::string& value)
{
	if (!_redis) return false;
	try
	{
		auto val = _redis->rpop(key);
		if (!val) return false;
		value = *val;
		return true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis rpop error: " << e.what() << std::endl;
		return false;
	}
}

bool RedisMgr::HSet(const std::string& key, const std::string& field, const std::string& value)
{
	if (!_redis) return false;
	try 
	{ 
		_redis->hset(key, field, value); return true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "redis hset error: " << e.what() << std::endl; 
	return false; 
	}
}

bool RedisMgr::HGet(const std::string& key, const std::string& field, std::string& value)
{
	if (!_redis) return false;
	try {
		auto val = _redis->hget(key, field);
		if (!val) return false;
		value = *val;
		return true;
	}
	catch (const std::exception& e) { std::cerr << "redis hget error: " << e.what() << std::endl; return false; }
}
