#include "ModernClient.hpp"

#include <sw/redis++/redis++.h>
#include <hiredis/hiredis.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Redis
{

namespace
{

// Converts a hiredis reply into the cpp_redis value type the rest of the
// plugin (and NWScript) already understands. RESP2 maps one to one; RESP3-only
// types are folded into the closest RESP2 shape.
cpp_redis::reply ToCppRedis(const redisReply* r)
{
    if (!r)
        return cpp_redis::reply();

    switch (r->type)
    {
        case REDIS_REPLY_STRING:
            return cpp_redis::reply(std::string(r->str, r->len), cpp_redis::reply::string_type::bulk_string);
        case REDIS_REPLY_STATUS:
            return cpp_redis::reply(std::string(r->str, r->len), cpp_redis::reply::string_type::simple_string);
        case REDIS_REPLY_ERROR:
            return cpp_redis::reply(std::string(r->str, r->len), cpp_redis::reply::string_type::error);
        case REDIS_REPLY_INTEGER:
            return cpp_redis::reply(static_cast<int64_t>(r->integer));
        case REDIS_REPLY_NIL:
            return cpp_redis::reply();
        // RESP3 types (only seen with NWNX_REDIS_RESP=3).
        case REDIS_REPLY_BOOL:
            return cpp_redis::reply(static_cast<int64_t>(r->integer));
        case REDIS_REPLY_DOUBLE:
        case REDIS_REPLY_BIGNUM:
        case REDIS_REPLY_VERB:
            return cpp_redis::reply(std::string(r->str, r->len), cpp_redis::reply::string_type::bulk_string);
        case REDIS_REPLY_ARRAY:
        case REDIS_REPLY_SET:
        case REDIS_REPLY_MAP:   // hiredis stores maps flattened: k1, v1, k2, v2...
        case REDIS_REPLY_PUSH:
        case REDIS_REPLY_ATTR:
        {
            std::vector<cpp_redis::reply> rows;
            rows.reserve(r->elements);
            for (size_t i = 0; i < r->elements; ++i)
                rows.push_back(ToCppRedis(r->element[i]));
            return cpp_redis::reply(rows);
        }
        default:
            return cpp_redis::reply("NWNX_Redis: unknown reply type " + std::to_string(r->type),
                                    cpp_redis::reply::string_type::error);
    }
}

cpp_redis::reply ErrorReply(const std::string& msg)
{
    return cpp_redis::reply(msg, cpp_redis::reply::string_type::error);
}

}

struct ModernClient::Impl
{
    ModernOptions opts;
    LogFn log;
    sw::redis::ConnectionOptions conn;
    std::unique_ptr<sw::redis::Redis> redis;

    std::atomic<bool> stopping{false};

    // Async worker.
    std::thread worker;
    std::mutex worker_mtx;
    std::condition_variable worker_cv;
    std::deque<std::pair<std::vector<std::string>, ReplyFn>> worker_queue;

    // Pubsub.
    std::thread pubsub;
    PubSubFn pubsub_cb;

    void Log(LogLevel l, const std::string& s) { if (log) log(l, s); }

    // While Redis is down every command fails; log the first failure and then
    // at most one summary every 10 seconds instead of one line per command.
    std::mutex conn_log_mtx;
    std::chrono::steady_clock::time_point conn_log_last{};
    uint64_t conn_log_suppressed = 0;
    bool conn_down = false;

    void LogConnError(const std::string& what)
    {
        std::lock_guard<std::mutex> lock(conn_log_mtx);
        auto now = std::chrono::steady_clock::now();
        if (!conn_down || now - conn_log_last > std::chrono::seconds(10))
        {
            std::string msg = "Connection error: " + what;
            if (conn_log_suppressed)
                msg += " (" + std::to_string(conn_log_suppressed) + " more since last report)";
            Log(LogLevel::Warning, msg);
            conn_log_last = now;
            conn_log_suppressed = 0;
            conn_down = true;
        }
        else
            conn_log_suppressed++;
    }

    void ConnOk()
    {
        if (!conn_down)
            return;
        std::lock_guard<std::mutex> lock(conn_log_mtx);
        if (conn_down)
        {
            Log(LogLevel::Info, "Connection to Redis restored");
            conn_down = false;
            conn_log_suppressed = 0;
        }
    }

    cpp_redis::reply Execute(const std::vector<std::string>& cmd)
    {
        if (cmd.empty())
            return ErrorReply("NWNX_Redis: empty command");
        try
        {
            auto reply = redis->command(cmd.begin(), cmd.end());
            ConnOk();
            return ToCppRedis(reply.get());
        }
        catch (const sw::redis::ReplyError& e)
        {
            // The server answered with an error reply (WRONGTYPE, ...).
            // Auth failures happen while connecting, so treat them like
            // connection problems for logging.
            std::string what = e.what();
            if (what.rfind("WRONGPASS", 0) == 0 || what.rfind("NOAUTH", 0) == 0 || what.rfind("NOPERM", 0) == 0)
                LogConnError(what);
            else
                ConnOk();
            return ErrorReply(what);
        }
        catch (const sw::redis::Error& e)
        {
            // Connection problem (refused, timeout, closed). The pool
            // reconnects on the next command; report it like a server error so
            // NWScript sees NWNX_REDIS_RESULT_ERROR instead of the server crashing.
            LogConnError(e.what());
            return ErrorReply(std::string("NWNX_Redis: ") + e.what());
        }
        catch (const std::exception& e)
        {
            Log(LogLevel::Error, std::string("Unexpected error: ") + e.what());
            return ErrorReply(std::string("NWNX_Redis: ") + e.what());
        }
    }

    void WorkerLoop()
    {
        for (;;)
        {
            std::pair<std::vector<std::string>, ReplyFn> job;
            {
                std::unique_lock<std::mutex> lock(worker_mtx);
                worker_cv.wait(lock, [this] { return stopping || !worker_queue.empty(); });
                if (worker_queue.empty())
                    return;
                job = std::move(worker_queue.front());
                worker_queue.pop_front();
            }
            auto r = Execute(job.first);
            if (job.second)
            {
                try { job.second(r); }
                catch (const std::exception& e) { Log(LogLevel::Error, std::string("Async callback threw: ") + e.what()); }
            }
        }
    }

    void PubSubLoop()
    {
        // A dedicated connection with a short read timeout, so consume()
        // returns regularly and we notice Stop().
        auto subopts = conn;
        subopts.socket_timeout = std::chrono::milliseconds(1000);
        auto backoff = std::chrono::milliseconds(250);

        while (!stopping)
        {
            try
            {
                sw::redis::Redis subredis(subopts);
                auto sub = subredis.subscriber();
                sub.on_pmessage([this](std::string, std::string channel, std::string msg) {
                    if (pubsub_cb)
                        pubsub_cb(channel, msg);
                });
                sub.on_message([this](std::string channel, std::string msg) {
                    if (pubsub_cb)
                        pubsub_cb(channel, msg);
                });
                sub.psubscribe(opts.pubsub_channels.begin(), opts.pubsub_channels.end());

                bool announced = false;
                while (!stopping)
                {
                    try
                    {
                        sub.consume();
                        if (!announced)
                        {
                            Log(LogLevel::Info, "PubSub: subscribed");
                            announced = true;
                            backoff = std::chrono::milliseconds(250);
                        }
                    }
                    catch (const sw::redis::TimeoutError&)
                    {
                        // Idle; loop to check stopping.
                    }
                }
            }
            catch (const sw::redis::Error& e)
            {
                if (stopping)
                    break;
                Log(LogLevel::Warning, std::string("PubSub: connection lost (") + e.what() +
                    "), reconnecting in " + std::to_string(backoff.count()) + "ms");
                // Sleep in small steps so Stop() stays responsive.
                auto until = std::chrono::steady_clock::now() + backoff;
                while (!stopping && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                backoff = std::min(backoff * 2, std::chrono::milliseconds(2000));
            }
        }
    }
};

ModernClient::ModernClient(const ModernOptions& opts, LogFn log)
    : m(std::make_unique<Impl>())
{
    m->opts = opts;
    m->log = std::move(log);

    auto& c = m->conn;
    c.host = opts.host;
    c.port = opts.port;
    if (!opts.username.empty())
        c.user = opts.username;
    c.password = opts.password;
    c.db = opts.database;
    c.resp = opts.resp;
    c.keep_alive = true;
    c.connect_timeout = opts.connect_timeout;
    c.socket_timeout = opts.socket_timeout;
#ifdef REDIS_PLUS_PLUS_TLS_VERIFY_MODE
    c.tls.enabled = opts.tls;
    c.tls.cacert = opts.tls_cacert;
    c.tls.cacertdir = opts.tls_cacertdir;
    c.tls.cert = opts.tls_cert;
    c.tls.key = opts.tls_key;
    c.tls.sni = opts.tls_sni;
    c.tls.verify_mode = opts.tls_verify ? REDIS_SSL_VERIFY_PEER : REDIS_SSL_VERIFY_NONE;
#else
    if (opts.tls)
        throw std::runtime_error("NWNX_REDIS_TLS is set, but this build has no TLS support (OpenSSL not found at build time)");
#endif

    sw::redis::ConnectionPoolOptions pool;
    pool.size = opts.pool_size ? opts.pool_size : 1;
    pool.wait_timeout = opts.pool_wait_timeout;

    // Connections are made lazily; a Redis that is down at startup is not fatal.
    m->redis = std::make_unique<sw::redis::Redis>(c, pool);

    m->worker = std::thread([this] { m->WorkerLoop(); });
}

ModernClient::~ModernClient()
{
    Stop();
}

cpp_redis::reply ModernClient::Execute(const std::vector<std::string>& cmd)
{
    return m->Execute(cmd);
}

void ModernClient::ExecuteAsync(std::vector<std::string> cmd, ReplyFn cb)
{
    {
        std::lock_guard<std::mutex> lock(m->worker_mtx);
        m->worker_queue.emplace_back(std::move(cmd), std::move(cb));
    }
    m->worker_cv.notify_one();
}

void ModernClient::StartPubSub(PubSubFn cb)
{
    if (m->opts.pubsub_channels.empty() || m->pubsub.joinable())
        return;
    m->pubsub_cb = std::move(cb);
    m->pubsub = std::thread([this] { m->PubSubLoop(); });
}

void ModernClient::Stop()
{
    if (!m)
        return;
    {
        std::lock_guard<std::mutex> lock(m->worker_mtx);
        m->stopping = true;
    }
    m->worker_cv.notify_all();
    if (m->worker.joinable())
        m->worker.join();
    if (m->pubsub.joinable())
        m->pubsub.join();
}

}
