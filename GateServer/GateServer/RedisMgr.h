#pragma once
#include "const.h"

class RedisMgr : public Singleton<RedisMgr>
{
	friend class Singleton<RedisMgr>;
public:
	~RedisMgr() = default;

public:
	// 字符串 + 过期时间
	bool Setex(const std::string& key, const std::string& value, long long seconds);

	// 字符串基础操作
	bool Set(const std::string& key, const std::string& value);
	bool Get(const std::string& key, std::string& value);

	// 键管理
	bool Del(const std::string& key);
	bool ExistsKey(const std::string& key);
	long long TTL(const std::string& key);      // 剩余过期秒数

	// 列表（将来消息队列/离线消息可用）
	bool LPush(const std::string& key, const std::string& value);
	bool LPop(const std::string& key, std::string& value);
	bool RPush(const std::string& key, const std::string& value);
	bool RPop(const std::string& key, std::string& value);
	// 哈希（将来存用户信息可用）
	bool HSet(const std::string& key, const std::string& field, const std::string& value);
	bool HGet(const std::string& key, const std::string& field, std::string& value);

private:
	RedisMgr();
	std::unique_ptr<sw::redis::Redis> _redis;   // 内部带连接池 → 多线程安全 //  唯一的 Redis 对象（RAII：析构自动断开）
};