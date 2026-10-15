const config_module = require('./config');
const redis = require('ioredis');

//创建redis客户端实例
const redisClient = new redis({
    host: config_module.redis_host,
    port: config_module.redis_port,
    password: config_module.redis_password,
});

//监听错误信息
redisClient.on("error", function (err) {
    console.log("RedisClient error:", err);
})

/**
 * 根据key获取缓存数据
 * @param {*} key - 缓存key
 * @returns {Promise<any>} - 缓存数据
 */

async function getRedisCache(key) {
    try {
        const result = await redisClient.get(key);
        if (result === null) {
            console.log(`getRedisCache: key ${key} not found in cache`);
            return null;
        }
        console.log(`getRedisCache: key ${key} found in cache, value: ${result}`);
        return result;
    } catch (error) {
        console.log("getRedisCache error:", error);
        return null;
    }
}

/**
 * 根据key查询redis是否存在key
 * @param {*} key - 缓存key
 * @returns {Promise<boolean>} - key是否存在
 */
async function QueryRedis(key) {
    try {
        const result = await redisClient.exists(key);
        if (result === 0) {
            console.log(`QueryRedis: key ${key} not found in cache`);
            return false;
        }
        console.log(`QueryRedis: key ${key} found in cache`);
        return true;
    } catch (error) {
        console.log("QueryRedis error:", error);
        return false;
    }
}

/**
 * 根据key设置缓存数据, 并设置过期时间
 * @param {*} key - 缓存key
 * @param {*} value - 缓存值
 * @param {*} exptime - 过期时间，单位秒
 */
async function setRedisCache(key, value, exptime) {
    try {
        await redisClient.set(key, value, 'EX', exptime);   // 设置+过期一步完成（SETEX）
        return true;
    } catch (error) {
        console.log("setRedisCache error:", error);
        return false;
    }
}

/**
 * 退出函数
 */

function quitRedis() {
    redisClient.quit();
}

module.exports = {
    getRedisCache,
    setRedisCache,
    QueryRedis,
    quitRedis
}