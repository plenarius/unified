// Async writer: the same bytes as sync writing, whole lines on a pipe shared with another process, order within
// a thread, drop accounting and the drains (FATAL, Shutdown, exit, EmergencyFlush). Run it under TSan too.
#include "nwnx.hpp"
#include "External/json/json.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace NWNXLib;
using Channel = Log::Channel;

static int s_failures;
#define CHECK(cond, ...) \
    do { if (!(cond)) { s_failures++; std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
         std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

// Fixed clocks, so sync and async runs give the same bytes.
extern "C" time_t time(time_t* t) noexcept
{
    if (t)
        *t = 1709611569;
    return 1709611569;
}

extern "C" int clock_gettime(clockid_t clock, timespec* ts) noexcept
{
    if (clock != CLOCK_REALTIME)
        return static_cast<int>(syscall(SYS_clock_gettime, clock, ts));
    ts->tv_sec = 1709611569;
    ts->tv_nsec = 123456789;
    return 0;
}

static std::string s_tmpDir;

static std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

// Points fd 1 at fd until the object goes away.
struct RedirectStdout
{
    int saved;
    explicit RedirectStdout(int fd)
    {
        std::cout.flush();
        std::fflush(stdout);
        saved = dup(1);
        dup2(fd, 1);
    }
    ~RedirectStdout()
    {
        std::cout.flush();
        std::fflush(stdout);
        dup2(saved, 1);
        close(saved);
    }
};

// Reads a pipe on a thread until every write end is closed.
struct PipeReader
{
    int fds[2];
    std::string data;
    std::thread thread;
    std::atomic<bool> go{true};

    explicit PipeReader(bool start = true)
    {
        if (pipe(fds) != 0)
            std::abort();
        if (start)
            Start();
    }
    void Start()
    {
        thread = std::thread([this]
        {
            char buf[65536];
            ssize_t n;
            while ((n = read(fds[0], buf, sizeof(buf))) > 0)
                data.append(buf, static_cast<size_t>(n));
        });
    }
    void CloseWrite() { close(fds[1]); }
    std::string Finish()
    {
        if (!thread.joinable())
            Start();
        thread.join();
        close(fds[0]);
        return data;
    }
};

static void LogSequence()
{
    LOG_DEBUG("debug %d", 1);
    LOG_INFO("info %s", "two");
    LOG_NOTICE("notice <c\xff\x01\x01>caf\xe9</c>");
    LOG_WARNING("warning %d", 4);
    LOG_ERROR("error %d", 5);
    LOG_NOTICE("long %s", std::string(10000, 'y'));
    Log::InternalTrace(Channel::SEV_NOTICE, Channel::SEV_NOTICE, "internal trace");
    Log::WriteToLogFile("to the file only");
    Log::Record record(Channel::SEV_INFO, PLUGIN_NAME, __FILE__, __LINE__);
    record.Stream() << "with fields";
    record.With("n", 7).With("s", "v").Write();
}

static Log::AsyncOptions Options(uint32_t flushMs = 50, size_t maxBytes = 8 * 1024 * 1024, bool drop = false)
{
    Log::AsyncOptions options;
    options.flushMs = flushMs;
    options.maxBytes = maxBytes;
    options.dropWhenFull = drop;
    return options;
}

// Async writes the same bytes as sync, to stdout and the file, in every format and colour mode.
static void TestSameBytes()
{
    const std::string outPath = s_tmpDir + "/same.out";
    const std::string filePath = s_tmpDir + "/same.log";
    Log::SetLogFile(filePath);
    const int fd = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    RedirectStdout redirect(fd);
    close(fd);

    struct Mode { Log::Format out, file; bool color, force; const char* name; };
    for (const Mode& mode : { Mode{ Log::Format::Text, Log::Format::Text, false, false, "text" },
                              Mode{ Log::Format::Text, Log::Format::Json, true, true, "text colour, json file" },
                              Mode{ Log::Format::Json, Log::Format::Text, true, false, "json, text file" },
                              Mode{ Log::Format::Json, Log::Format::Json, false, false, "json" } })
    {
        Log::SetFormat(mode.out);
        Log::SetFileFormat(mode.file);
        Log::SetColorOutput(mode.color);
        Log::SetForceColor(mode.force);
        Log::WriteToLogFile("opens the file");
        std::string out[2], file[2];
        for (int async = 0; async < 2; async++)
        {
            std::cout.flush();
            const size_t outStart = ReadFile(outPath).size(), fileStart = ReadFile(filePath).size();
            if (async)
                Log::StartAsync(Options());
            LogSequence();
            Log::Shutdown();
            std::cout.flush();
            out[async] = ReadFile(outPath).substr(outStart);
            file[async] = ReadFile(filePath).substr(fileStart);
        }
        CHECK(out[0] == out[1] && out[0].size() > 5000, "%s: stdout differs (%zu/%zu bytes)", mode.name, out[0].size(), out[1].size());
        CHECK(file[0] == file[1] && file[0].size() > 5000, "%s: file differs (%zu/%zu bytes)", mode.name, file[0].size(), file[1].size());
    }
    Log::SetColorOutput(false);
    Log::SetForceColor(false);
    Log::SetLogFile("");
}

// Checks a record line, text or JSON, and returns its message.
static bool ParseRecord(const std::string& line, Log::Format format, std::string& msg, nlohmann::json* record = nullptr)
{
    if (format == Log::Format::Json)
    {
        try
        {
            auto parsed = nlohmann::json::parse(line);
            msg = parsed.at("msg").get<std::string>();
            if (record)
                *record = std::move(parsed);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
    // "L [time] [plugin] [file:line] msg", from this test or from the logger itself.
    size_t source = line.find("] [LogAsync.cpp:");
    if (source == std::string::npos)
        source = line.find("] [Log.cpp:");
    const size_t end = source == std::string::npos ? source : line.find("] ", source + 2);
    if (line.compare(1, 15, " [04:06:09] [NW") != 0 || end == std::string::npos)
        return false;
    msg = line.substr(end + 2);
    return true;
}

// 8 threads log through the writer into a pipe that another process writes too: no line is torn, and each
// thread's lines arrive in order.
static void TestPipe(Log::Format format)
{
    constexpr int threads = 8, lines = 3000, otherLines = 20000;
    const char* name = format == Log::Format::Json ? "json" : "text";
    Log::SetFormat(format);
    PipeReader reader;

    const pid_t other = fork();
    if (other == 0)
    {
        // Writes of at most PIPE_BUF bytes, which the kernel keeps whole.
        char buf[4096];
        for (int i = 0; i < otherLines; i++)
        {
            const int length = 24 + (i * 37) % 3000;
            const int prefix = std::snprintf(buf, sizeof(buf), "other %d %d ", i, length);
            std::memset(buf + prefix, 'o', static_cast<size_t>(length - prefix - 1));
            buf[length - 1] = '\n';
            if (write(reader.fds[1], buf, static_cast<size_t>(length)) != length)
                _exit(1);
        }
        _exit(0);
    }
    {
        RedirectStdout redirect(reader.fds[1]);
        Log::StartAsync(Options());
        std::vector<std::thread> producers;
        for (int t = 0; t < threads; t++)
        {
            producers.emplace_back([t]
            {
                for (int i = 0; i < lines; i++)
                    LOG_NOTICE("t %d i %d %s", t, i, std::string(static_cast<size_t>(i * 13 % 1500), 'x'));
            });
        }
        for (auto& producer : producers)
            producer.join();
        Log::Shutdown();
    }
    reader.CloseWrite();
    int status = 0;
    waitpid(other, &status, 0);
    const std::string data = reader.Finish();

    int others = 0, torn = 0;
    std::vector<int> next(threads, 0);
    for (const auto& line : SplitLines(data))
    {
        int i = 0, t = 0, length = 0, consumed = 0;
        if (std::sscanf(line.c_str(), "other %d %d %n", &i, &length, &consumed) == 2)
        {
            const bool whole = static_cast<int>(line.size()) + 1 == length &&
                               line.find_first_not_of('o', static_cast<size_t>(consumed)) == std::string::npos;
            others += whole;
            torn += !whole;
            continue;
        }
        std::string msg;
        if (!ParseRecord(line, format, msg) || std::sscanf(msg.c_str(), "t %d i %d %n", &t, &i, &consumed) != 2 ||
            t < 0 || t >= threads || msg.size() - static_cast<size_t>(consumed) != static_cast<size_t>(i * 13 % 1500))
        {
            if (torn++ < 3)
                std::fprintf(stderr, "torn: %.120s\n", line.c_str());
            continue;
        }
        CHECK(i == next[t], "%s: thread %d: line %d after %d", name, t, i, next[t] - 1);
        next[t] = i + 1;
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "%s: the other writer failed", name);
    CHECK(torn == 0, "%s: %d torn lines", name, torn);
    CHECK(others == otherLines, "%s: %d of %d lines from the other process", name, others, otherLines);
    for (int t = 0; t < threads; t++)
        CHECK(next[t] == lines, "%s: thread %d: %d of %d lines", name, t, next[t], lines);
}

// A pipe whose reader stalls: with drop, callers never wait and every record is either written or counted
// in a dropped record; with block, nothing is lost. WARNING and up are never dropped.
static void TestFull(Log::Format format, bool drop)
{
    constexpr int records = 20000;
    const std::string name = std::string(format == Log::Format::Json ? "json" : "text") + (drop ? " drop" : " block");
    Log::SetFormat(format);
    PipeReader reader(false);
    fcntl(reader.fds[1], F_SETPIPE_SZ, 4096);

    double longest = 0;
    {
        RedirectStdout redirect(reader.fds[1]);
        Log::StartAsync(Options(5, 64 * 1024, drop));
        std::thread late([&reader] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); reader.Start(); });
        for (int i = 0; i < records; i++)
        {
            // WARNING always waits for room, so only INFO calls are timed.
            if (i % 1000 == 999)
            {
                LOG_WARNING("full %d %s", i, std::string(100, 'w'));
                continue;
            }
            const auto start = std::chrono::steady_clock::now();
            LOG_INFO("full %d %s", i, std::string(100, 'f'));
            longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        late.join();
        Log::Shutdown();
    }
    reader.CloseWrite();
    const std::string data = reader.Finish();

    int written = 0, warnings = 0, bad = 0;
    int64_t dropped = 0;
    for (const auto& line : SplitLines(data))
    {
        std::string msg;
        nlohmann::json record;
        int i = 0;
        if (!ParseRecord(line, format, msg, &record))
            bad++;
        else if (std::sscanf(msg.c_str(), "full %d", &i) == 1)
        {
            written++;
            warnings += i % 1000 == 999;
        }
        else if (msg.find("Dropped ") == 0)
        {
            const int64_t count = std::atoll(msg.c_str() + 8);
            dropped += count;
            if (format == Log::Format::Json)
                CHECK(record.value("dropped", int64_t(-1)) == count && record.value("level", 0) == 40, "%s: dropped record %s", name.c_str(), line.c_str());
        }
        else
            bad++;
    }
    std::printf("%s: %d written, %lld dropped, longest INFO call %.2f ms\n", name.c_str(), written, static_cast<long long>(dropped), longest);
    CHECK(bad == 0, "%s: %d bad lines", name.c_str(), bad);
    CHECK(written + dropped == records, "%s: %d written + %lld dropped != %d", name.c_str(), written, static_cast<long long>(dropped), records);
    CHECK(warnings == records / 1000, "%s: %d of %d warnings", name.c_str(), warnings, records / 1000);
    if (drop)
        CHECK(dropped > 0 && longest < 100, "%s: %lld dropped, longest call %.2f ms", name.c_str(), static_cast<long long>(dropped), longest);
    else
        CHECK(dropped == 0 && longest > 100, "%s: %lld dropped, longest call %.2f ms", name.c_str(), static_cast<long long>(dropped), longest);
}

// Runs fn in a child whose stdout is a pipe; returns its output and wait status.
template <typename Fn>
static std::string RunChild(Fn fn, int& status)
{
    // The reader starts after the fork: TSan does not let a child of a multi-threaded process start threads.
    PipeReader reader(false);
    std::cout.flush();
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0)
    {
        rlimit noCore = { 0, 0 };
        setrlimit(RLIMIT_CORE, &noCore);
        dup2(reader.fds[1], 1);
        close(reader.fds[0]);
        close(reader.fds[1]);
        fn();
        _exit(0);
    }
    reader.CloseWrite();
    reader.Start();
    waitpid(pid, &status, 0);
    return reader.Finish();
}

// Counts "queued N" lines in order, and whether the line with last follows them.
static void CheckDrained(const char* name, const std::string& data, Log::Format format, int count, const char* last)
{
    int next = 0;
    bool lastSeen = false;
    for (const auto& line : SplitLines(data))
    {
        std::string msg;
        int i = 0;
        if (!ParseRecord(line, format, msg))
            continue;
        if (std::sscanf(msg.c_str(), "queued %d", &i) == 1)
        {
            CHECK(i == next && !lastSeen, "%s: queued %d out of order", name, i);
            next = i + 1;
        }
        lastSeen |= msg == last;
    }
    CHECK(next == count && lastSeen, "%s: %d of %d queued lines, last line %s", name, next, count, lastSeen ? "seen" : "missing");
}

// Lines queued with a long flush interval still come out: on FATAL, Shutdown, exit(), EmergencyFlush and Flush.
static void TestDrains(Log::Format format)
{
    constexpr int count = 500;
    Log::SetFormat(format);
    auto queue = [] { Log::StartAsync(Options(60000)); for (int i = 0; i < count; i++) LOG_INFO("queued %d", i); };
    int status = 0;

    std::string data = RunChild([&] { queue(); LOG_FATAL("the end"); }, status);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "fatal: status %d", status);
    CheckDrained("fatal", data, format, count, "the end");

    data = RunChild([&] { queue(); LOG_NOTICE("the end"); Log::Shutdown(); }, status);
    CheckDrained("shutdown", data, format, count, "the end");

    data = RunChild([&] { queue(); LOG_NOTICE("the end"); std::exit(0); }, status);
    CheckDrained("exit", data, format, count, "the end");

    // After EmergencyFlush records are written at once: _exit() would lose queued ones.
    data = RunChild([&] { queue(); Log::EmergencyFlush(); LOG_NOTICE("the end"); _exit(0); }, status);
    CheckDrained("emergency", data, format, count, "the end");

    data = RunChild([&] { queue(); LOG_NOTICE("the end"); Log::Flush(); _exit(0); }, status);
    CheckDrained("flush", data, format, count, "the end");
}

int main()
{
    const char* tmp = std::getenv("TMPDIR");
    s_tmpDir = std::string(tmp && *tmp ? tmp : "/tmp") + "/nwnx-log-async-XXXXXX";
    if (!mkdtemp(s_tmpDir.data()))
        return 2;

    Log::SetLogLevel(PLUGIN_NAME, Channel::SEV_DEBUG);
    Log::SetPrintTimestamp(true);
    Log::SetPrintPlugin(true);
    Log::SetPrintSource(true);
    Log::SetColorOutput(false);

    TestSameBytes();
    for (auto format : { Log::Format::Json, Log::Format::Text })
    {
        TestPipe(format);
        TestFull(format, true);
        TestFull(format, false);
        TestDrains(format);
    }

    std::remove((s_tmpDir + "/same.out").c_str());
    std::remove((s_tmpDir + "/same.log").c_str());
    rmdir(s_tmpDir.c_str());
    std::printf("%s\n", s_failures ? "FAILED" : "ok");
    return s_failures ? 1 : 0;
}
