#include "Redis.hpp"
#include "Internal.hpp"

#include <algorithm>
#include <cctype>


namespace Redis
{

using namespace NWNXLib;
using namespace NWNXLib::Services;

std::unique_ptr<cpp_redis::redis_client> Redis::PoolMakeFunc()
{
    std::lock_guard<std::mutex> lock(m_internal->m_config_mtx);

    ASSERT(!m_internal->m_config.m_host.empty());
    ASSERT(m_internal->m_config.m_port >= 0);

    auto p = std::make_unique<cpp_redis::redis_client>();
    (*p).connect(m_internal->m_config.m_host, static_cast<size_t>(m_internal->m_config.m_port));
    return p;
}

void Redis::ConfigureModernClient()
{
    // Called with m_config_mtx held.
    ModernOptions o;
    o.host = m_internal->m_config.m_host;
    o.port = m_internal->m_config.m_port;
    o.username = Config::Get<std::string>("USERNAME", "");
    o.password = Config::Get<std::string>("PASSWORD", "");
    o.database = Config::Get<int>("DATABASE", 0);
    o.resp = Config::Get<int>("RESP", 2);
    o.tls = Config::Get<bool>("TLS", false);
    o.tls_cacert = Config::Get<std::string>("TLS_CACERT", "");
    o.tls_cacertdir = Config::Get<std::string>("TLS_CACERTDIR", "");
    o.tls_cert = Config::Get<std::string>("TLS_CERT", "");
    o.tls_key = Config::Get<std::string>("TLS_KEY", "");
    o.tls_sni = Config::Get<std::string>("TLS_SNI", "");
    o.tls_verify = Config::Get<bool>("TLS_VERIFY", true);
    o.pool_size = static_cast<size_t>(std::max(1, Config::Get<int>("POOL_SIZE", 4)));
    o.connect_timeout = std::chrono::milliseconds(Config::Get<int>("CONNECT_TIMEOUT_MS", 1000));
    o.socket_timeout = std::chrono::milliseconds(Config::Get<int>("SOCKET_TIMEOUT_MS", 5000));
    o.failfast = std::chrono::milliseconds(Config::Get<int>("FAILFAST_MS", 3000));
    o.pool_wait_timeout = std::chrono::milliseconds(Config::Get<int>("POOL_WAIT_TIMEOUT_MS", 1000));
    for (auto& ch : m_internal->m_config.m_pubsub_channels)
        if (!ch.empty())
            o.pubsub_channels.push_back(ch);

    LOG_INFO("Client: redis-plus-plus (hiredis), pool=%d, tls=%s, user=%s, resp=%d",
             (int)o.pool_size, o.tls ? "on" : "off", o.username.empty() ? "default" : o.username, o.resp);

    m_internal->m_modern.reset();
    m_internal->m_modern = std::make_unique<ModernClient>(o, [](ModernClient::LogLevel l, const std::string& s) {
        switch (l)
        {
            case ModernClient::LogLevel::Error:   LOG_ERROR("%s", s); break;
            case ModernClient::LogLevel::Warning: LOG_WARNING("%s", s); break;
            case ModernClient::LogLevel::Info:    LOG_INFO("%s", s); break;
            case ModernClient::LogLevel::Debug:   LOG_DEBUG("%s", s); break;
        }
    });

    // Don't fail startup if Redis is down: the pool reconnects on its own.
    auto pong = m_internal->m_modern->Execute({"PING"});
    if (pong.is_error())
        LOG_WARNING("Redis not reachable yet (%s); will keep retrying", pong.error());
    else
        LOG_INFO("%s", "Connected!");

    for (auto& ch : o.pubsub_channels)
        LOG_INFO("PubSub: Subscribing to %s", ch);
    m_internal->m_modern->StartPubSub([this](const std::string& channel, const std::string& message) {
        OnPubsub(channel, message);
    });
}

void Redis::Reconfigure()
{
    using namespace std::placeholders;

    {
        std::lock_guard<std::mutex> lock(m_internal->m_config_mtx);

        // Redis server.
        m_internal->m_config.m_host = *Config::Get<std::string>("HOST");
        m_internal->m_config.m_port = Config::Get<int>("PORT", 6379);

        // Pubsub.
        m_internal->m_config.m_pubsub_script = Config::Get<std::string>("PUBSUB_SCRIPT", "on_pubsub");
        m_internal->m_config.m_pubsub_channels = String::Split(
            Config::Get<std::string>("PUBSUB_CHANNELS", ""), ',');

        LOG_INFO("Reconfiguring for redis at %s:%d",
                                   m_internal->m_config.m_host,
                                   m_internal->m_config.m_port);

        LOG_INFO("PubSub: Using NWScript: %s", m_internal->m_config.m_pubsub_script);

        auto client = Config::Get<std::string>("CLIENT", "cpp_redis");
        std::transform(client.begin(), client.end(), client.begin(), ::tolower);
        if (client == "redis-plus-plus" || client == "redis++" || client == "hiredis")
        {
            ConfigureModernClient();
            return;
        }
        if (client != "cpp_redis")
            LOG_WARNING("Unknown NWNX_REDIS_CLIENT '%s', using cpp_redis", client);

        try
        {
            m_internal->m_connection_pubsub.disconnect();
        }
        catch (cpp_redis::redis_error& e)
        {
            LOG_NOTICE("Error while reconfiguring pubsub client: %s", e.what());
        }
        m_internal->m_connection_pubsub.connect(
            m_internal->m_config.m_host, static_cast<size_t>(m_internal->m_config.m_port));

        auto bound = std::bind(&Redis::OnPubsub, this, _1, _2);
        for (auto& ch : m_internal->m_config.m_pubsub_channels)
        {
            LOG_INFO("PubSub: Subscribing to %s", ch);
            m_internal->m_connection_pubsub.psubscribe(ch, bound);
        }
        m_internal->m_connection_pubsub.commit();

        LOG_INFO("%s", "Connected!");
    }

    m_internal->m_redis_pool.Clean();
}

}
