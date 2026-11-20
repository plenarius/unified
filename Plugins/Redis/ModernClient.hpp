#pragma once

// Opt-in client backend built on redis-plus-plus + hiredis
// (NWNX_REDIS_CLIENT=redis-plus-plus).
//
// This header deliberately does not include anything from NWNX or from
// redis-plus-plus, so it can be compiled into a standalone test harness.
// Replies are converted to cpp_redis::reply so that the NWScript API, the
// metrics and the C++ interface in Redis.hpp behave exactly as with the
// legacy cpp_redis backend.

#include <cpp_redis/reply.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Redis
{

struct ModernOptions
{
    std::string host = "127.0.0.1";
    int port = 6379;

    // ACL user (Redis 6+/Valkey). Empty means "default", i.e. AUTH <password>.
    std::string username;
    std::string password;
    int database = 0;

    // RESP protocol version: 2 (default, identical reply types to cpp_redis) or 3.
    int resp = 2;

    bool tls = false;
    std::string tls_cacert;
    std::string tls_cacertdir;
    std::string tls_cert;
    std::string tls_key;
    std::string tls_sni;
    bool tls_verify = true;

    size_t pool_size = 4;
    std::chrono::milliseconds connect_timeout{1000};
    // 0 = block forever. Commands from NWScript run on the game's main thread,
    // so a finite timeout bounds how long a dead Redis can freeze the server.
    std::chrono::milliseconds socket_timeout{2000};
    // After a command times out, fail every command at once for this long
    // instead of blocking each caller for socket_timeout again (0 = off).
    std::chrono::milliseconds failfast{3000};
    // How long a caller waits for a free pooled connection.
    std::chrono::milliseconds pool_wait_timeout{1000};

    std::vector<std::string> pubsub_channels;
};

class ModernClient
{
public:
    enum class LogLevel { Error, Warning, Info, Debug };
    using LogFn = std::function<void(LogLevel, const std::string&)>;
    using PubSubFn = std::function<void(const std::string& channel, const std::string& message)>;
    using ReplyFn = std::function<void(cpp_redis::reply&)>;

    ModernClient(const ModernOptions& opts, LogFn log);
    ~ModernClient();

    ModernClient(const ModernClient&) = delete;
    ModernClient& operator=(const ModernClient&) = delete;

    // Runs a command on a pooled connection. Thread-safe. Never throws:
    // server errors and connection problems both come back as an error reply.
    cpp_redis::reply Execute(const std::vector<std::string>& cmd);

    // Runs a command on the client's worker thread and calls cb there.
    void ExecuteAsync(std::vector<std::string> cmd, ReplyFn cb);

    // Starts the pubsub thread: PSUBSCRIBEs to every configured pattern and
    // reconnects/resubscribes on its own after connection loss.
    // cb runs on the pubsub thread.
    void StartPubSub(PubSubFn cb);

    // Stops the pubsub and worker threads. Called by the destructor.
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}
