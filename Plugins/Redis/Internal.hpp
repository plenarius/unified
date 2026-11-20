#pragma once

#include <cpp_redis/cpp_redis>
#include "Redis.hpp"
#include "Pool.hpp"
#include <mutex>
#include <sstream>
#include <thread>
#include <condition_variable>

namespace Redis
{

struct Internal {
    Internal(Pool<cpp_redis::redis_client>::MakeFunc m) :
        m_redis_pool(m, [](cpp_redis::redis_client& c) { return c.is_connected(); }) {}

    // Connection pool for redis, used both for nwscript and for
    // external consumers (like other Plugins).
    Pool<cpp_redis::redis_client> m_redis_pool;

    // The pubsub connection, replaced by m_pubsub_thread whenever it drops.
    std::unique_ptr<cpp_redis::redis_subscriber> m_connection_pubsub;
    std::thread m_pubsub_thread;
    std::mutex m_pubsub_mtx;
    std::condition_variable m_pubsub_cv;
    bool m_pubsub_lost = false;
    bool m_pubsub_stop = false;

    // Fail fast instead of reconnecting on every call while redis is down.
    std::chrono::steady_clock::time_point m_last_connect_failure;
    std::string m_last_connect_error;
    std::string m_last_pubsub_channel;
    std::string m_last_pubsub_message;

    // Config update mutex. Pool could run into this!
    std::mutex m_config_mtx;

    // Aaand the config
    Redis::Configuration m_config;
};

inline std::string RedisReplyTypeToString(const cpp_redis::reply::type& t)
{
    switch (t) {
        case cpp_redis::reply::type::array: return "Array";
        case cpp_redis::reply::type::error: return "Error";
        case cpp_redis::reply::type::integer: return "Integer";

        case cpp_redis::reply::type::bulk_string:
        case cpp_redis::reply::type::simple_string:
            return "String";

        case cpp_redis::reply::type::null: return "Null";
    }
    return "Unknown";
}

inline int RedisReplyTypeToInt(const cpp_redis::reply::type& t)
{
    switch (t)
    {
        case cpp_redis::reply::type::array: return 1;
        case cpp_redis::reply::type::error: return 3;
        case cpp_redis::reply::type::integer: return 4;

        case cpp_redis::reply::type::bulk_string:
        case cpp_redis::reply::type::simple_string:
            return 5;

        case cpp_redis::reply::type::null: return 6;
    }
    return 0;
}

inline std::string RedisReplyAsString(const cpp_redis::reply& r)
{
    std::stringstream ss;
    ss << r;
    return ss.str();
}

}
