// Stdio capture: stdout and stderr lines arrive on the real fds as JSON records, JSON objects pass through,
// ANSI codes are stripped, engine console lines are dropped while hooked, and nothing is lost at the stop.
#include "nwnx.hpp"
#include "External/json/json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

using namespace NWNXLib;

static int s_failures;

#define CHECK(cond, ...) \
    do { if (!(cond)) { s_failures++; std::fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond); \
         std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

static std::vector<nlohmann::json> ReadRecords(const char* path, int& bad)
{
    std::vector<nlohmann::json> records;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
    {
        auto json = nlohmann::json::parse(line, nullptr, false);
        if (json.is_discarded() || !json.is_object())
            bad++;
        else
            records.push_back(std::move(json));
    }
    return records;
}

int main()
{
    char outPath[] = "/tmp/logcapture-out-XXXXXX";
    char errPath[] = "/tmp/logcapture-err-XXXXXX";
    const int out = mkstemp(outPath), err = mkstemp(errPath);
    const int savedErr = dup(STDERR_FILENO);
    dup2(out, STDOUT_FILENO);
    dup2(err, STDERR_FILENO);

    Log::SetFormat(Log::Format::Json);
    const bool started = Log::StartStdioCapture();
    Log::StartAsync({});

    std::printf("plain stdout line\n");
    std::printf("\033[31mred\033[0m text\n");
    std::printf("{\"level\":30,\"msg\":\"from anvil\"}\n");
    std::printf("{not json}\n");
    std::printf("I [21:00:27] Network (exonetinternal.cpp:293:InitializeUDP): unhooked engine line\n");
    std::fflush(stdout);
    std::fprintf(stderr, "W [21:00:27] (nwsync.cpp:26:operator()): general engine line\n");
    std::fprintf(stderr, "stderr line\n\n   \n");
    std::fprintf(stderr, "!! Userdata log emitted from non-main thread discarded !!\n");
    std::cout << "iostream line" << std::endl;
    usleep(200 * 1000);
    Log::SetEngineLinesHooked(true);
    std::fprintf(stderr, "E [21:00:50] ScriptVM (x.cpp:1:f): hooked engine line\033[00m\n");
    std::fprintf(stderr, "!! Userdata log emitted from non-main thread discarded !!\n");
    LOG_NOTICE("an nwnx record");
    // Lines from several threads, and a partial line written just before the stop.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
        threads.emplace_back([t] { for (int i = 0; i < 1000; i++) std::fprintf(stderr, "thread %d line %d\n", t, i); });
    for (auto& thread : threads)
        thread.join();
    std::printf("%s", "no newline at the end");
    Log::Shutdown();
    Log::StopStdioCapture();
    std::printf("after the stop\n");
    std::fflush(stdout);

    dup2(savedErr, STDERR_FILENO);
    CHECK(started, "StartStdioCapture failed");
    int bad = 0;
    auto outRecords = ReadRecords(outPath, bad);
    auto errRecords = ReadRecords(errPath, bad);
    CHECK(bad == 1, "%d unparsable lines (want 1: the one after the stop)", bad);

    auto find = [](const std::vector<nlohmann::json>& records, const std::string& msg) -> const nlohmann::json*
    {
        for (const auto& record : records)
            if (record.value("msg", "") == msg)
                return &record;
        return nullptr;
    };
    auto expect = [&](const std::vector<nlohmann::json>& records, const char* msg, const char* src, int level)
    {
        const auto* record = find(records, msg);
        CHECK(record, "missing '%s'", msg);
        if (record)
            CHECK(record->value("src", "") == src && record->value("level", 0) == level, "'%s': %s", msg, record->dump().c_str());
    };
    expect(outRecords, "plain stdout line", "stdout", 30);
    expect(outRecords, "red text", "stdout", 30);
    expect(outRecords, "{not json}", "stdout", 30);
    expect(outRecords, "iostream line", "stdout", 30);
    expect(outRecords, "no newline at the end", "stdout", 30);
    expect(outRecords, "an nwnx record", "nwnx", 35);
    const auto* anvil = find(outRecords, "from anvil");
    CHECK(anvil && anvil->size() == 2, "JSON line not passed through unchanged");
    const auto* unhooked = find(outRecords, "I [21:00:27] Network (exonetinternal.cpp:293:InitializeUDP): unhooked engine line");
    CHECK(unhooked && unhooked->value("cat", "") == "Network", "unhooked engine line without cat");
    expect(errRecords, "W [21:00:27] (nwsync.cpp:26:operator()): general engine line", "stderr", 40);
    expect(errRecords, "stderr line", "stderr", 30);
    CHECK(!find(errRecords, "E [21:00:50] ScriptVM (x.cpp:1:f): hooked engine line"), "hooked engine line not dropped");
    // Wrapped before the hook, dropped after it.
    const auto notices = std::count_if(errRecords.begin(), errRecords.end(),
        [](const auto& r) { return r.value("msg", "") == "!! Userdata log emitted from non-main thread discarded !!"; });
    CHECK(notices == 1, "%zd userdata notices, want 1", static_cast<ssize_t>(notices));
    CHECK(errRecords.size() == 4003, "%zu stderr records, want 4003 (no blank ones)", errRecords.size());
    std::vector<int> next(4, 0);
    for (const auto& record : errRecords)
    {
        int t, i;
        if (std::sscanf(record.value("msg", "").c_str(), "thread %d line %d", &t, &i) == 2)
        {
            CHECK(t >= 0 && t < 4 && i == next[t], "thread %d: line %d, want %d", t, i, next[t]);
            next[t] = i + 1;
        }
    }
    CHECK(fcntl(out, F_GETFD) >= 0, "fd closed");
    unlink(outPath);
    unlink(errPath);
    std::fprintf(stderr, "LogCapture: %s\n", s_failures ? "FAILED" : "ok");
    return s_failures ? 1 : 0;
}
