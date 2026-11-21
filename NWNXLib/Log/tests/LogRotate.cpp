// Rotation of NWNX's own log file: under load from 8 threads, sync and async, every line lands exactly once,
// whole and in order per thread across the archives, and each file names the archive it continues from;
// the size and time triggers; reopen on a signal.
#include "nwnx.hpp"
#include "Log/Rotation.hpp"
#include "External/json/json.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace NWNXLib;
using Channel = Log::Channel;

static int s_failures;
#define CHECK(cond, ...) \
    do { if (!(cond)) { s_failures++; std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
         std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

// A clock the test moves, for archive names and the time trigger.
static std::atomic<time_t> s_now{1709611569}; // 2024-03-05 04:06:09 UTC
extern "C" time_t time(time_t* t) noexcept
{
    const time_t now = s_now.load();
    if (t)
        *t = now;
    return now;
}

static std::string s_tmpDir;

static std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The log file's archives, oldest first, then the file itself.
static std::vector<std::string> Parts(const std::string& name)
{
    std::vector<std::pair<std::string, unsigned long>> archives;
    if (DIR* d = opendir(s_tmpDir.c_str()))
    {
        while (auto* entry = readdir(d))
        {
            const std::string file = entry->d_name;
            if (file.size() > name.size() + 1 && !file.compare(0, name.size() + 1, name + "."))
            {
                const auto dash = file.find('-', name.size());
                archives.emplace_back(file, dash == std::string::npos ? 0 : std::strtoul(file.c_str() + dash + 1, nullptr, 10));
            }
        }
        closedir(d);
    }
    std::sort(archives.begin(), archives.end(), [&name](const auto& a, const auto& b)
    {
        const int cmp = a.first.compare(0, name.size() + 17, b.first, 0, name.size() + 17);
        return cmp ? cmp < 0 : a.second < b.second;
    });
    std::vector<std::string> parts;
    for (const auto& archive : archives)
        parts.push_back(s_tmpDir + "/" + archive.first);
    parts.push_back(s_tmpDir + "/" + name);
    return parts;
}

static void RemoveAll(const std::string& name)
{
    for (const auto& part : Parts(name))
        std::remove(part.c_str());
}

// The message of a record line, or "" for banner lines; false for a line that is not whole.
static bool Message(const std::string& line, Log::Format format, std::string& msg)
{
    msg.clear();
    if (format == Log::Format::Json)
    {
        const auto record = nlohmann::json::parse(line, nullptr, false);
        if (record.is_discarded() || !record.is_object())
            return false;
        msg = record.value("msg", "");
        return true;
    }
    if (line.empty() || line[0] == '=' || line[0] == ' ')
        return true; // banner
    if (line.size() < 3 || !std::strchr("FEWNID", line[0]) || line[1] != ' ')
        return false;
    const auto at = line.rfind("] ");
    msg = line.substr(at == std::string::npos ? 2 : at + 2);
    return true;
}

// The archive a banner line names, or "".
static std::string RotatedFrom(const std::string& line, Log::Format format)
{
    if (format == Log::Format::Json)
        return line.find("\"rotated_from\"") == std::string::npos ? "" : nlohmann::json::parse(line).value("rotated_from", "");
    const std::string prefix = " Rotated from: ";
    return line.compare(0, prefix.size(), prefix) ? "" : line.substr(prefix.size());
}

static Log::AsyncOptions Options()
{
    Log::AsyncOptions options;
    options.flushMs = 5;
    options.maxBytes = 256 * 1024;
    return options;
}

// 8 threads log while the main loop ticks (size trigger) and rotates on demand.
static void TestUnderLoad(Log::Format format, bool async)
{
    const std::string name = std::string("load-") + (format == Log::Format::Json ? "json" : "text") + (async ? "-async" : "-sync") + ".log";
    const std::string path = s_tmpDir + "/" + name;
    RemoveAll(name);
    Log::SetFileFormat(format);
    Log::SetLogFile(path);
    Log::FileRotation rotation;
    rotation.size = 32 * 1024;
    rotation.keep = 0;
    Log::SetFileRotation(rotation);
    if (async)
        Log::StartAsync(Options());

    constexpr int threads = 8, count = 4000;
    std::atomic<int> done{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < threads; t++)
    {
        producers.emplace_back([t, &done]
        {
            for (int i = 0; i < count; i++)
                LOG_INFO("seq %d %d %s end", t, i, std::string(static_cast<size_t>(i % 97), 'x'));
            done++;
        });
    }
    int rotations = 0;
    while (done < threads)
    {
        Log::Tick();
        if (++rotations % 64 == 0)
        {
            Log::RotateFile();
            s_now += rotations % 128 ? 0 : 1; // mostly the same second: -N names
        }
        std::this_thread::yield();
    }
    for (auto& producer : producers)
        producer.join();
    Log::Shutdown();
    Log::SetLogFile("");
    Log::SetFileRotation({});

    const auto parts = Parts(name);
    std::vector<int> next(threads, 0);
    int torn = 0, rotatingNotices = 0;
    for (size_t p = 0; p < parts.size(); p++)
    {
        std::istringstream in(ReadFile(parts[p]));
        int banners = 0;
        for (std::string line, msg; std::getline(in, line);)
        {
            // A rotated file names the archive before it; the first file names none.
            const std::string from = RotatedFrom(line, format);
            if (!from.empty())
            {
                banners++;
                CHECK(p && from == parts[p - 1].substr(s_tmpDir.size() + 1), "%s: part %zu rotated from '%s'",
                      name.c_str(), p, from.c_str());
            }
            if (!Message(line, format, msg))
            {
                if (!torn)
                    std::fprintf(stderr, "%s: torn line '%s'\n", name.c_str(), line.c_str());
                torn++;
                continue;
            }
            int t, i;
            if (std::sscanf(msg.c_str(), "seq %d %d", &t, &i) == 2)
            {
                const bool whole = t >= 0 && t < threads && msg.size() > 4 && !msg.compare(msg.size() - 4, 4, " end");
                CHECK(whole && i == next[t], "%s: '%s' after %d", name.c_str(), msg.c_str(), whole ? next[t] : -1);
                if (whole)
                    next[t] = i + 1;
            }
            else if (msg.rfind("Rotating", 0) == 0)
                rotatingNotices++;
        }
        CHECK(banners == (p ? 1 : 0), "%s: part %zu has %d rotated-from banners", name.c_str(), p, banners);
    }
    for (int t = 0; t < threads; t++)
        CHECK(next[t] == count, "%s: thread %d: %d of %d lines", name.c_str(), t, next[t], count);
    CHECK(torn == 0, "%s: %d torn lines", name.c_str(), torn);
    // Each archive ends with its Rotating notice.
    CHECK(parts.size() > 10 && rotatingNotices == static_cast<int>(parts.size()) - 1, "%s: %zu parts, %d notices",
          name.c_str(), parts.size(), rotatingNotices);
    RemoveAll(name);
}

// The time trigger rotates on the UTC boundary, by the time of the rotation, and leaves a file without records.
static void TestTime()
{
    const std::string name = "time.log";
    const std::string path = s_tmpDir + "/" + name;
    RemoveAll(name);
    Log::SetFileFormat(Log::Format::Text);
    Log::SetLogFile(path);
    Log::FileRotation rotation;
    rotation.period = 3600;
    rotation.keep = 2;
    Log::SetFileRotation(rotation);

    s_now = 1709611569; // 04:06:09
    Log::Tick();
    LOG_INFO("before the hour");
    s_now = 1709614799; // 04:59:59
    Log::Tick();
    CHECK(Parts(name).size() == 1, "rotated before the hour");
    s_now = 1709614800; // 05:00:00
    Log::Tick();
    auto parts = Parts(name);
    CHECK(parts.size() == 2 && parts[0] == path + ".20240305T050000Z", "%zu parts, first %s", parts.size(), parts[0].c_str());
    CHECK(ReadFile(parts[0]).find("before the hour") != std::string::npos, "archive lacks the line");

    // An hour with no records keeps its file.
    s_now = 1709618400; // 06:00:00
    Log::Tick();
    CHECK(Parts(name).size() == 2, "rotated a file without records");

    // KEEP=2 prunes the oldest.
    for (time_t hour = 1709622000; hour <= 1709629200; hour += 3600)
    {
        LOG_INFO("in hour %d", static_cast<int>(hour));
        s_now = hour;
        Log::Tick();
    }
    parts = Parts(name);
    CHECK(parts.size() == 3 && parts[0] == path + ".20240305T080000Z", "%zu parts, first %s", parts.size(), parts[0].c_str());

    // The size trigger fires once ROTATE_SIZE bytes of records are written.
    rotation.period = 0;
    rotation.size = 4096;
    rotation.keep = 0;
    Log::SetFileRotation(rotation);
    Log::RotateFile();
    const size_t before = Parts(name).size();
    for (int i = 0; i < 20; i++)
        LOG_INFO("%s", std::string(100, 's'));
    Log::Tick();
    CHECK(Parts(name).size() == before, "rotated below the size");
    for (int i = 0; i < 20; i++)
        LOG_INFO("%s", std::string(100, 's'));
    Log::Tick();
    CHECK(Parts(name).size() == before + 1, "not rotated at the size");

    Log::SetLogFile("");
    Log::SetFileRotation({});
    RemoveAll(name);
}

// After an external rename, a signal reopens the path; lines go to the moved file until then and none twice.
static void TestReopen(bool async)
{
    const std::string name = async ? "reopen-async.log" : "reopen-sync.log";
    const std::string path = s_tmpDir + "/" + name;
    const std::string moved = path + "-moved";
    RemoveAll(name);
    std::remove(moved.c_str());
    Log::SetFileFormat(Log::Format::Text);
    Log::SetLogFile(path);
    if (async)
        Log::StartAsync(Options());

    struct sigaction action = {};
    action.sa_handler = [](int) { Log::RequestReopen(); };
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, nullptr);

    LOG_INFO("line 0");
    CHECK(!Log::Tick(), "a reopen without a signal");
    CHECK(!Log::ReopenFile(), "%s: reopened a file that was not moved", name.c_str());
    Log::Flush();
    CHECK(rename(path.c_str(), moved.c_str()) == 0, "rename");
    LOG_INFO("line 1");
    raise(SIGUSR1);
    CHECK(Log::Tick(), "the signal did not ask for a reopen");
    LOG_INFO("line 2");
    Log::Shutdown();
    signal(SIGUSR1, SIG_DFL);
    Log::SetLogFile("");

    const std::string first = ReadFile(moved), second = ReadFile(path);
    CHECK(first.find("line 0") != std::string::npos && first.find("line 1") != std::string::npos && first.find("line 2") == std::string::npos,
          "moved file:\n%s", first.c_str());
    CHECK(second.find("Reopened") != std::string::npos && second.find("line 2") != std::string::npos && second.find("line 1") == std::string::npos,
          "new file:\n%s", second.c_str());
    std::remove(moved.c_str());
    RemoveAll(name);
}

int main()
{
    const char* tmp = std::getenv("TMPDIR");
    s_tmpDir = std::string(tmp && *tmp ? tmp : "/tmp") + "/nwnx-log-rotate-XXXXXX";
    if (!mkdtemp(s_tmpDir.data()))
        return 2;

    // Only the file is checked.
    const int null = open("/dev/null", O_WRONLY);
    dup2(null, 1);
    close(null);

    Log::SetLogLevel(PLUGIN_NAME, Channel::SEV_DEBUG);
    Log::SetLogLevel("NWNXLib", Channel::SEV_DEBUG);
    Log::SetColorOutput(false);

    TestTime();
    for (bool async : { false, true })
    {
        TestReopen(async);
        for (auto format : { Log::Format::Text, Log::Format::Json })
            TestUnderLoad(format, async);
    }

    rmdir(s_tmpDir.c_str());
    std::fprintf(stderr, "%s\n", s_failures ? "FAILED" : "ok");
    return s_failures ? 1 : 0;
}
