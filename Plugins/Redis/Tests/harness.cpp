// Standalone harness for the opt-in redis-plus-plus client (no nwserver).
// Modes:
//   compare <host> <port>               : run the same commands through cpp_redis and
//                                         ModernClient, compare reply type and string form
//   smoke <host> <port> <user> <pass> <tls 0/1> <cacert>
//   soak <host> <port> <user> <pass> <seconds> : commands + pubsub every 100ms; survive restarts
#include "../ModernClient.hpp"
#include <cpp_redis/cpp_redis>
#include <atomic>
#include <iostream>
#include <sstream>
#include <thread>
#include <cstdlib>

using namespace Redis;
static std::string str(const cpp_redis::reply& r) { std::stringstream ss; ss << r; return ss.str(); }
static int typ(const cpp_redis::reply& r) {
    switch (r.get_type()) {
        case cpp_redis::reply::type::array: return 1; case cpp_redis::reply::type::error: return 3;
        case cpp_redis::reply::type::integer: return 4; case cpp_redis::reply::type::null: return 6;
        default: return 5; }
}
static std::string dump(const cpp_redis::reply& r) {
    std::string s = std::to_string(typ(r)) + ":" + str(r);
    if (r.is_array()) { s += "["; for (auto& e : r.as_array()) s += dump(e) + ","; s += "]"; }
    return s;
}
static void log(ModernClient::LogLevel l, const std::string& s) { std::cerr << "[modern " << (int)l << "] " << s << "\n"; }

int main(int argc, char** argv) {
    std::string mode = argv[1];
    ModernOptions o; o.host = argv[2]; o.port = std::stoi(argv[3]);
    if (mode == "compare") {
        // Each client gets its own database so mutating commands see the same state.
        cpp_redis::redis_client legacy; legacy.connect(o.host, o.port);
        legacy.send({"SELECT","1"}).send({"FLUSHDB"}).sync_commit();
        o.database = 2; o.pool_size = 1;
        ModernClient modern(o, log);
        modern.Execute({"FLUSHDB"});
        std::vector<std::vector<std::string>> cmds = {
            {"DEL","k","l","h","z","s"}, {"PING"}, {"SET","k","v"}, {"GET","k"}, {"GET","missing"},
            {"INCR","n"}, {"INCRBYFLOAT","f","1.5"}, {"LPUSH","l","a","b","c"}, {"LRANGE","l","0","-1"},
            {"HSET","h","f1","1","f2","2"}, {"HGETALL","h"}, {"HGET","h","nope"}, {"LPOP","k"},
            {"NOSUCHCMD"}, {"ZADD","z","1","a","2","b"}, {"ZRANGE","z","0","-1","WITHSCORES"},
            {"SADD","s","x"}, {"SMEMBERS","s"}, {"EXISTS","k","nope"}, {"TYPE","h"}, {"MGET","k","missing"},
            {"SCAN","0","MATCH","nothing*"}, {"EVAL","return {1,'two',{3}}","0"}, {"EVAL","return redis.error_reply('MYERR x')","0"},
            {"SET","bin",std::string("a\0b",3)}, {"STRLEN","bin"}, {"OBJECT","ENCODING","k"}, {"DEL","n","f"} };
        int fails = 0;
        for (auto& c : cmds) {
            cpp_redis::reply lr; legacy.send(c, [&](cpp_redis::reply& r){ lr = r; }).sync_commit();
            auto mr = modern.Execute(c);
            bool same = dump(lr) == dump(mr);
            if (!same) fails++;
            std::cout << (same ? "SAME " : "DIFF ") << c[0] << "  legacy=" << dump(lr) << "  modern=" << dump(mr) << "\n";
        }
        std::cout << "compare: " << fails << " differences\n";
        return fails ? 1 : 0;
    }
    if (mode == "legacysoak") {
        // The legacy client exactly as the plugin uses it: one connection, send + sync_commit().
        int secs = std::stoi(argv[4]);
        cpp_redis::redis_client c; c.connect(o.host, o.port);
        std::atomic<long> callStart{0};
        std::thread watchdog([&]{ for (;;) { std::this_thread::sleep_for(std::chrono::milliseconds(200));
            long s0 = callStart; long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (s0 && now - s0 > 10000) { std::cout << "legacysoak: HUNG - a sync_commit() has blocked for >10s (the game main thread would be frozen)" << std::endl; std::_Exit(3); } } });
        watchdog.detach();
        int ok = 0, err = 0;
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
        for (int i = 0; std::chrono::steady_clock::now() < end; i++) {
            try {
                callStart = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                cpp_redis::reply rt; c.send({"INCR","legacycounter"}, [&](cpp_redis::reply& r){ rt = r; }).sync_commit();
                callStart = 0;
                if (rt.is_error() || rt.is_null()) { err++; } else ok++;
            } catch (const std::exception& e) { callStart = 0; err++; if (err < 3) std::cout << "t=" << i << " exception: " << e.what() << std::endl; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "legacysoak: ok=" << ok << " err=" << err << " connected=" << c.is_connected() << std::endl;
        return 0;
    }
    o.username = argv[4]; o.password = argv[5];
    if (mode == "smoke") {
        o.tls = std::string(argv[6]) == "1"; o.tls_cacert = argv[7]; o.tls_sni = "localhost";
        o.pubsub_channels = {"smoke*"};
        ModernClient modern(o, log);
        std::atomic<int> got{0};
        modern.StartPubSub([&](const std::string& ch, const std::string& m){ std::cout << "pubsub " << ch << " " << m << "\n"; got++; });
        std::cout << "PING " << dump(modern.Execute({"PING"})) << "\n";
        std::cout << "SET " << dump(modern.Execute({"SET","smokekey","1"})) << "\n";
        std::cout << "GET " << dump(modern.Execute({"GET","smokekey"})) << "\n";
        std::cout << "WHOAMI " << dump(modern.Execute({"ACL","WHOAMI"})) << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        modern.Execute({"PUBLISH","smoke.a","hello"});
        std::atomic<int> async{0};
        modern.ExecuteAsync({"INCR","smokecount"}, [&](cpp_redis::reply& r){ std::cout << "async " << dump(r) << "\n"; async++; });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        bool ok = got == 1 && async == 1;
        std::cout << "smoke: " << (ok ? "OK" : "FAIL") << " pubsub=" << got << " async=" << async << "\n";
        return ok ? 0 : 1;
    }
    if (mode == "soak") {
        int secs = std::stoi(argv[6]);
        o.pubsub_channels = {"soak*"}; o.pool_size = 4; o.socket_timeout = std::chrono::milliseconds(500); o.connect_timeout = std::chrono::milliseconds(300);
        ModernClient modern(o, log);
        std::atomic<int> got{0}; std::atomic<int> lastseq{-1};
        modern.StartPubSub([&](const std::string&, const std::string& m){ got++; lastseq = std::stoi(m); });
        int ok = 0, err = 0, pubs = 0, firstOkAfterErr = -1; bool inErr = false; long maxMs = 0;
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
        for (int i = 0; std::chrono::steady_clock::now() < end; i++) {
            auto t0 = std::chrono::steady_clock::now();
            auto r = modern.Execute({"INCR","soakcounter"});
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            maxMs = std::max<long>(maxMs, ms);
            if (r.is_error()) { err++; if (!inErr) std::cout << "t=" << i << " error: " << str(r) << " (" << ms << "ms)\n"; inErr = true; }
            else { ok++; if (inErr) { std::cout << "t=" << i << " recovered\n"; inErr = false; firstOkAfterErr = i; } }
            auto p = modern.Execute({"PUBLISH","soak.x",std::to_string(i)});
            if (!p.is_error() && p.as_integer() > 0) pubs++;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::cout << "soak: ok=" << ok << " err=" << err << " max_call_ms=" << maxMs << " published_with_subscriber=" << pubs
                  << " received=" << got << " last_seq_received=" << lastseq << " recovered_at=" << firstOkAfterErr << "\n";
        return (ok > 0 && got > 0) ? 0 : 1;
    }
    return 2;
}
