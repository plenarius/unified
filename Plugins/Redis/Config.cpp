#include "Redis.hpp"
#include "Internal.hpp"

#include <future>

namespace Redis
{

using namespace NWNXLib;
using namespace NWNXLib::Services;
using namespace std::chrono_literals;

static constexpr auto AUTH_TIMEOUT = 5s;
static constexpr auto CONNECT_RETRY_MAX = 8s;
static constexpr auto PUBSUB_MAX_BACKOFF = 30s;

// Sends AUTH through send() and checks the reply. Never logs the password.
bool Redis::Authenticate(const char* what, const AuthSender& send)
{
    std::string username;
    {
        std::lock_guard<std::mutex> lock(m_internal->m_config_mtx);
        username = m_internal->m_config.m_username;
    }
    const std::string who = username.empty() ? "the default user" : "user '" + username + "'";

    // Shared so a reply arriving after the timeout has somewhere to go.
    auto promise = std::make_shared<std::promise<cpp_redis::reply>>();
    auto future = promise->get_future();
    send([promise](cpp_redis::reply& r) { promise->set_value(r); });

    if (future.wait_for(AUTH_TIMEOUT) != std::future_status::ready)
    {
        LOG_ERROR("%s: AUTH as %s timed out after %ds", what, who,
                  (int)std::chrono::duration_cast<std::chrono::seconds>(AUTH_TIMEOUT).count());
        return false;
    }

    cpp_redis::reply reply;
    try
    {
        reply = future.get();
    }
    catch (const std::future_error&)
    {
        LOG_ERROR("%s: connection closed before the AUTH reply arrived", what);
        return false;
    }

    if (!reply.is_error())
    {
        LOG_DEBUG("%s: authenticated as %s", what, who);
        return true;
    }

    const auto err = reply.as_string();
    if (err.find("no password is set") != std::string::npos ||
        err.find("without any password configured") != std::string::npos)
    {
        // The connection works without AUTH; don't turn a config mismatch into an outage.
        LOG_WARNING("%s: NWNX_REDIS_PASSWORD is set but the redis server has no password configured "
                    "(server said: %s)", what, err);
        return true;
    }
    if (err.rfind("WRONGPASS", 0) == 0 || err.find("invalid password") != std::string::npos)
        LOG_ERROR("%s: AUTH as %s failed: wrong password, unknown user or user disabled (server said: %s)",
                  what, who, err);
    else if (!username.empty() && err.find("wrong number of arguments") != std::string::npos)
        LOG_ERROR("%s: AUTH failed: this redis server is older than 6.0 and does not support "
                  "NWNX_REDIS_USERNAME (server said: %s)", what, err);
    else
        LOG_ERROR("%s: AUTH as %s failed (server said: %s)", what, who, err);
    return false;
}

std::unique_ptr<cpp_redis::redis_client> Redis::PoolMakeFunc()
{
    std::string host, username, password;
    int port;
    {
        std::lock_guard<std::mutex> lock(m_internal->m_config_mtx);

        ASSERT(!m_internal->m_config.m_host.empty());
        ASSERT(m_internal->m_config.m_port >= 0);

        host = m_internal->m_config.m_host;
        port = m_internal->m_config.m_port;
        username = m_internal->m_config.m_username;
        password = m_internal->m_config.m_password;
    }

    // Called under the pool lock, so the failure state needs no lock of its own.
    if (!m_internal->m_last_connect_error.empty() &&
        std::chrono::steady_clock::now() < m_internal->m_next_connect_attempt)
    {
        throw cpp_redis::redis_error(m_internal->m_last_connect_error);
    }

    try
    {
        auto p = std::make_unique<cpp_redis::redis_client>();
        p->connect(host, static_cast<size_t>(port));

        if (!password.empty() && !Authenticate("Pool", [&](auto cb) {
                if (username.empty())
                    p->send({"AUTH", password}, cb);
                else
                    p->send({"AUTH", username, password}, cb);
                p->commit();
            }))
        {
            throw cpp_redis::redis_error("authentication failed");
        }

        if (!m_internal->m_last_connect_error.empty())
            LOG_INFO("Pool: connection to %s:%d restored", host, port);
        m_internal->m_last_connect_error.clear();
        m_internal->m_connect_retry_delay = 0s;
        return p;
    }
    catch (const std::exception& e)
    {
        // Back off from the end of the attempt: a hung server costs AUTH_TIMEOUT per try on the main thread.
        auto& delay = m_internal->m_connect_retry_delay;
        delay = std::min<std::chrono::seconds>(delay == 0s ? 1s : delay * 2, CONNECT_RETRY_MAX);
        m_internal->m_next_connect_attempt = std::chrono::steady_clock::now() + delay;
        m_internal->m_last_connect_error = std::string("redis unavailable: ") + e.what();
        LOG_ERROR("Pool: could not open a connection to %s:%d: %s", host, port, e.what());
        throw cpp_redis::redis_error(m_internal->m_last_connect_error);
    }
}

bool Redis::ConnectPubsub()
{
    using namespace std::placeholders;

    std::string host, username, password;
    std::vector<std::string> channels;
    int port;
    {
        std::lock_guard<std::mutex> lock(m_internal->m_config_mtx);
        host = m_internal->m_config.m_host;
        port = m_internal->m_config.m_port;
        username = m_internal->m_config.m_username;
        password = m_internal->m_config.m_password;
        channels = m_internal->m_config.m_pubsub_channels;
    }

    auto sub = std::make_unique<cpp_redis::redis_subscriber>();
    try
    {
        sub->connect(host, static_cast<size_t>(port), [this](cpp_redis::redis_subscriber&) {
            // Runs on the cpp_redis io thread, which must not block: just wake the supervisor.
            {
                std::lock_guard<std::mutex> lock(m_internal->m_pubsub_mtx);
                m_internal->m_pubsub_lost = true;
            }
            m_internal->m_pubsub_cv.notify_one();
        });

        if (!password.empty() && !Authenticate("PubSub", [&](auto cb) {
                if (username.empty())
                    sub->auth(password, cb);
                else
                    sub->auth(username, password, cb);
                sub->commit();
            }))
        {
            return false;
        }

        auto bound = std::bind(&Redis::OnPubsub, this, _1, _2);
        for (auto& ch : channels)
        {
            LOG_INFO("PubSub: Subscribing to %s", ch);
            sub->psubscribe(ch, bound);
        }
        sub->commit();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("PubSub: could not connect to %s:%d: %s", host, port, e.what());
        return false;
    }

    std::unique_ptr<cpp_redis::redis_subscriber> old;
    {
        std::lock_guard<std::mutex> lock(m_internal->m_pubsub_mtx);
        old = std::move(m_internal->m_connection_pubsub);
        m_internal->m_connection_pubsub = std::move(sub);
    }
    return true;
}

// Owns reconnecting the pubsub connection, off the main thread.
void Redis::PubsubSupervisor()
{
    auto backoff = std::chrono::seconds(1);
    bool retrying = false;
    std::unique_lock<std::mutex> lock(m_internal->m_pubsub_mtx);

    while (!m_internal->m_pubsub_stop)
    {
        // The timeout is a backstop in case a disconnect slipped past the handler.
        m_internal->m_pubsub_cv.wait_for(lock, 5s, [this] {
            return m_internal->m_pubsub_stop || m_internal->m_pubsub_lost;
        });
        if (m_internal->m_pubsub_stop)
            break;

        auto& conn = m_internal->m_connection_pubsub;
        if (!m_internal->m_pubsub_lost && conn && conn->is_connected())
            continue;

        m_internal->m_pubsub_lost = false;
        const bool wasConnected = conn != nullptr && !retrying;
        lock.unlock();
        if (wasConnected)
            LOG_WARNING("%s", "PubSub: connection lost, reconnecting");
        const bool ok = ConnectPubsub();
        lock.lock();

        if (ok)
        {
            LOG_INFO("%s", "PubSub: connected");
            backoff = std::chrono::seconds(1);
            retrying = false;
            continue;
        }

        LOG_INFO("PubSub: retrying in %ds", (int)backoff.count());
        retrying = true;

        m_internal->m_pubsub_cv.wait_for(lock, backoff, [this] { return m_internal->m_pubsub_stop; });
        backoff = std::min<std::chrono::seconds>(backoff * 2, PUBSUB_MAX_BACKOFF);
        m_internal->m_pubsub_lost = true;
    }
}

void Redis::Reconfigure()
{
    {
        std::lock_guard<std::mutex> lock(m_internal->m_config_mtx);

        // Redis server.
        m_internal->m_config.m_host = *Config::Get<std::string>("HOST");
        m_internal->m_config.m_port = Config::Get<int>("PORT", 6379);
        m_internal->m_config.m_username = Config::Get<std::string>("USERNAME", "");
        m_internal->m_config.m_password = Config::Get<std::string>("PASSWORD", "");

        // Pubsub.
        m_internal->m_config.m_pubsub_script = Config::Get<std::string>("PUBSUB_SCRIPT", "on_pubsub");
        m_internal->m_config.m_pubsub_channels = String::Split(
            Config::Get<std::string>("PUBSUB_CHANNELS", ""), ',');

        LOG_INFO("Reconfiguring for redis at %s:%d",
                                   m_internal->m_config.m_host,
                                   m_internal->m_config.m_port);

        if (!m_internal->m_config.m_password.empty())
            LOG_INFO("Authenticating as %s", m_internal->m_config.m_username.empty() ?
                     std::string("the default user") : "user '" + m_internal->m_config.m_username + "'");
        else if (!m_internal->m_config.m_username.empty())
            LOG_WARNING("%s", "NWNX_REDIS_USERNAME is set without NWNX_REDIS_PASSWORD; not authenticating");

        LOG_INFO("PubSub: Using NWScript: %s", m_internal->m_config.m_pubsub_script);
    }

    if (ConnectPubsub())
    {
        LOG_INFO("%s", "Connected!");
    }
    else
    {
        LOG_ERROR("%s", "PubSub: initial connection failed, retrying in the background");
        std::lock_guard<std::mutex> lock(m_internal->m_pubsub_mtx);
        m_internal->m_pubsub_lost = true;
    }

    if (!m_internal->m_pubsub_thread.joinable())
        m_internal->m_pubsub_thread = std::thread(&Redis::PubsubSupervisor, this);

    m_internal->m_redis_pool.Clean();
}

}
