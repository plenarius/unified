// Golden test for the JSON log output: the scenarios of LogGolden in JSON, plus the JSON options, compared
// byte for byte with a fixture. Every record line is also parsed with nlohmann.
// Usage: LogGoldenJson <fixture> [--update]; on a mismatch it writes LogGoldenJson.actual.txt to the working directory.
#include "nwnx.hpp"
#include "Log/Json.hpp"
#include "External/json/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace NWNXLib;
using Channel = Log::Channel;

static time_t s_now = 1709611569; // 2024-03-05 04:06:09 UTC

// Fixed clocks: interposes libc's time() for text and the realtime clock_gettime() for JSON.
extern "C" time_t time(time_t* t) noexcept
{
    if (t)
        *t = s_now;
    return s_now;
}

extern "C" int clock_gettime(clockid_t clock, timespec* ts) noexcept
{
    if (clock != CLOCK_REALTIME)
        return static_cast<int>(syscall(SYS_clock_gettime, clock, ts));
    ts->tv_sec = s_now;
    ts->tv_nsec = 123456789;
    return 0;
}

static std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void SetFormat(int combo)
{
    Log::SetPrintTimestamp(combo & 1);
    Log::SetPrintDate(combo & 2);
    Log::SetPrintPlugin(combo & 4);
    Log::SetPrintSource(combo & 8);
}

static void Section(const char* name)
{
    std::cout << "## " << name << '\n';
}

static Log::Json::Options s_defaults;

static void SetJson(void (*change)(Log::Json::Options&) = nullptr)
{
    Log::Json::Options options = s_defaults;
    if (change)
        change(options);
    Log::SetJsonOptions(options);
}

// Defined at the end of the file under #line, so their __FILE__/__LINE__ never change.
void LogAllLevels(int combo);
void LogDirect();
void LogSpecial();
void LogStrings();
void LogFields();
void LogLevelFilter();
void LogBus();
void LogFromSubscriber();
void LogFatal();
void LogAssert();
void LogFieldsF();

// Runs LogFatal in a child (it aborts). The assertion's own line in Log.cpp is masked.
static void RunFatal()
{
    std::cout.flush();
    std::fflush(stdout);
    int fds[2];
    if (pipe(fds) != 0)
        std::abort();
    pid_t pid = fork();
    if (pid == 0)
    {
        rlimit noCore = {0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        LogFatal();
        _exit(0);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    const std::string logCpp = R"("file":"Log.cpp","line":)";
    for (size_t pos = out.find(logCpp); pos != std::string::npos; pos = out.find(logCpp, pos + 1))
    {
        const size_t digits = pos + logCpp.size();
        out.replace(digits, out.find_first_not_of("0123456789", digits) - digits, "0");
    }
    std::cout << out << "signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << '\n';
}

static std::string Run(const std::string& tmpDir)
{
    const std::string stdoutPath = tmpDir + "/stdout.txt";
    const std::string filePath = tmpDir + "/file.log";
    std::string bus;

    std::cout.flush();
    std::fflush(stdout);
    int savedStdout = dup(1);
    int fd = open(stdoutPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, 1);
    close(fd);

    Log::SetLogLevel(PLUGIN_NAME, Channel::SEV_DEBUG);
    Log::SetLogLevel("NWNX_Direct", Channel::SEV_DEBUG);
    Log::SetColorOutput(true);
    Log::SetForceColor(true);
    Log::SetFormat(Log::Format::Json);
    Log::SetFileFormat(Log::Format::Json);
    SetJson();

    // Only SOURCE changes JSON; the other options and colour are ignored.
    for (int combo : {0, 7, 8, 15})
    {
        SetFormat(combo);
        Section(("format=" + std::to_string(combo)).c_str());
        LogAllLevels(combo);
        LogDirect();
    }

    SetFormat(15);
    Section("special");
    LogSpecial();

    Section("strings");
    LogStrings();
    Section("strings: keep colours, locale encoding");
    SetJson([](auto& o) { o.stripColors = false; o.encoding = Log::Json::Encoding::Locale; });
    LogStrings();
    Section("strings: utf8 encoding, cp1251");
    SetJson([](auto& o) { o.encoding = Log::Json::Encoding::Utf8; });
    String::SetDefaultLocale(String::cp1251);
    LogStrings();
    Section("strings: auto, cp1251");
    SetJson();
    LogStrings();
    String::SetDefaultLocale(String::cp1252);

    Section("level labels");
    SetJson([](auto& o) { o.levelLabel = true; });
    LogAllLevels(-1);
    SetJson();

    Section("fields and static");
    std::vector<std::string> renamed;
    std::cout << "static error: '" << Log::SetJsonStatic(R"({"server":"main","level":9,"tags":["a",1]})", renamed) << "'";
    for (const auto& key : renamed)
        std::cout << " renamed " << key;
    std::cout << '\n';
    LogFields();
    std::cout << "bad static: '" << Log::SetJsonStatic("[]", renamed) << "'\n";
    LogAllLevels(-2);
    Log::SetJsonStatic("{}", renamed);

    Section("_F records");
    LogFieldsF();

    Section("limits");
    SetJson([](auto& o) { o.maxValue = 64; o.maxLine = 1024; });
    LogSpecial();
    LogFields();
    // The line limit shortens the message first, then drops the static members and fields.
    SetJson([](auto& o) { o.maxLine = 1024; });
    LogSpecial();
    Log::SetJsonStatic("{\"big\":\"" + std::string(2000, 'z') + "\"}", renamed);
    LogFields();
    Log::SetJsonStatic("{}", renamed);
    SetJson();

    Section("levels");
    LogLevelFilter();
    Log::SetLogLevel(PLUGIN_NAME, Channel::SEV_DEBUG);

    Section("bus");
    auto id = MessageBus::Subscribe("NWNX_CORE_LOG_MESSAGE", [&bus](const MessageBus::Message& msg)
    {
        for (const auto& part : msg)
            bus += part + "|";
        bus += '\n';
        if (msg.size() == 5 && msg[4] == "bus nested")
            LogFromSubscriber();
    });
    LogBus();
    MessageBus::Unsubscribe(id);

    Section("assert");
    LogAssert();

    Section("fatal");
    RunFatal();

    Section("raw");
    Log::InternalTrace(Channel::SEV_WARNING, Channel::SEV_DEBUG, "internal trace");
    Log::WriteToLogFile("not written: no file yet");

    // Text on stdout with a JSON file: stdout is exactly the text format.
    Section("text stdout, json file");
    Log::SetFormat(Log::Format::Text);
    Log::SetLogFile(filePath);
    Log::SetColorOutput(false);
    LogAllLevels(15);
    LogAssert();
    LogFieldsF();
    Log::InternalTrace(Channel::SEV_NOTICE, Channel::SEV_DEBUG, "internal trace to the file");
    Log::WriteToLogFile("written to the file");

    std::cout.flush();
    std::fflush(stdout);
    dup2(savedStdout, 1);
    close(savedStdout);

    std::string result = ReadFile(stdoutPath);
    result += "=== file ===\n" + ReadFile(filePath);
    result += "=== bus ===\n" + bus;
    std::remove(stdoutPath.c_str());
    std::remove(filePath.c_str());
    return result;
}

// Every line that starts with '{' must be one JSON object.
static int CheckRecords(const std::string& output)
{
    int bad = 0;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.empty() || line[0] != '{')
            continue;
        try
        {
            if (!nlohmann::json::parse(line).is_object())
                throw std::runtime_error("not an object");
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "Not a JSON record (%s): %s\n", e.what(), line.c_str());
            bad++;
        }
    }
    return bad;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <fixture> [--update]\n", argv[0]);
        return 2;
    }
    const std::string fixture = argv[1];
    const bool update = argc > 2 && std::strcmp(argv[2], "--update") == 0;

    setenv("TZ", "UTC", 1);
    tzset();
    unsetenv("TERM");

    const char* tmp = std::getenv("TMPDIR");
    std::string tmpDir = std::string(tmp && *tmp ? tmp : "/tmp") + "/nwnx-log-golden-json-XXXXXX";
    if (!mkdtemp(tmpDir.data()))
    {
        std::perror("mkdtemp");
        return 2;
    }

    std::string actual = Run(tmpDir);
    rmdir(tmpDir.c_str());

    if (CheckRecords(actual))
        return 1;

    if (update)
    {
        std::ofstream(fixture, std::ios::binary) << actual;
        std::printf("Wrote %s (%zu bytes)\n", fixture.c_str(), actual.size());
        return 0;
    }

    std::string expected = ReadFile(fixture);
    if (actual == expected)
    {
        std::printf("Log output matches %s (%zu bytes)\n", fixture.c_str(), actual.size());
        return 0;
    }

    size_t pos = 0;
    while (pos < actual.size() && pos < expected.size() && actual[pos] == expected[pos])
        pos++;
    const std::string actualPath = "LogGoldenJson.actual.txt";
    std::ofstream(actualPath, std::ios::binary) << actual;
    std::fprintf(stderr, "Log output differs from %s at byte %zu (expected %zu bytes, got %zu); wrote %s\n",
                 fixture.c_str(), pos, expected.size(), actual.size(), actualPath.c_str());
    return 1;
}

#line 900 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
// A type whose operator<< logs while the outer line is being formatted.
struct LogsWhileFormatting {};
static std::ostream& operator<<(std::ostream& os, LogsWhileFormatting)
{
    LOG_NOTICE("nested %s", "inner");
    return os << "outer-arg";
}

#line 1000 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogAllLevels(int combo)
{
    LOG_DEBUG("debug %d: %s", combo, std::string("std::string"));
    LOG_INFO("info %u %.3f %x %%", 42u, 3.14159, 255);
    LOG_NOTICE("notice %-8s|%+d|%08.2f|%#x", "left", 7, -2.5, 48879);
    LOG_WARNING("warning bool=%s char=%c %s", true, 'Z', 1.0 / 3.0);
    LOG_ERROR("error");
}

#line 2000 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogDirect()
{
    Log::Trace(Channel::SEV_NOTICE, "NWNX_Direct", "/abs/path/Direct.cpp", 12, "absolute path");
    Log::Trace(Channel::SEV_INFO, "NWNX_Direct", "Plain.cpp", 0, "no directory %s", "here");
    Log::Trace(Channel::SEV_WARNING, "NWNX_Direct", "trailing/", -1, "trailing slash");
    Log::Trace(Channel::SEV_ERROR, "NWNX_Direct", "", 99999, "empty file");
    Log::Trace(Channel::SEV_NOTICE, "NWNX_\"Quoted\"\\", "a\"b.cpp", 1, "plugin and file are escaped");
}

#line 3000 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogSpecial()
{
    LOG_NOTICE("%s", std::string(10000, 'x') + "end");
    LOG_NOTICE("short after long");
    LOG_NOTICE("");
    LOG_NOTICE("two\nlines\r\tand \"quotes\" \\ \x01\x1f\x7f");
    LOG_NOTICE("embedded %s nul", std::string("a\0b", 3));
    LOG_NOTICE("%s", LogsWhileFormatting{});
    LOG_NOTICE("%s", std::string(100, '\n'));
    LOG_NOTICE("%s", std::string(3000, '\xe9'));
}

#line 3500 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogStrings()
{
    LOG_NOTICE("unicode \xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80");
    LOG_NOTICE("cp125x \xe9 \x80 \xc0\xff \x81\x98");
    LOG_NOTICE("invalid \xc0\x80 \xed\xa0\x80 \xe2\x82");
    LOG_NOTICE("<c\xfe\x01\x01>red</c> and <c\xff\xff\xff>white</c>, a<count AND b>3");
    LOG_NOTICE("<c\xff\xff\xff>colour bytes around utf8 caf\xc3\xa9</c>");
}

#line 3700 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogFields()
{
    Log::Record record(Channel::SEV_INFO, PLUGIN_NAME, __FILE__, __LINE__);
    record.Stream() << "with fields";
    record.With("server", "overrides the static server")
          .With("rows", 3)
          .With("level", "a reserved key")
          .With("text", std::string(200, 'y') + "<c\xff\x01\x01>\xe9</c>")
          .WithStack("stack", "\n  Backtrace:\n    ./nwserver(frame+0x1) [0x1]\n    /lib/libc.so.6(\"q\"+0x2) [0x2]\n")
          .Write();
    LOG_INFO("no fields after fields");
}

#line 4000 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogLevelFilter()
{
    for (int level : {0, 2, 5, 7, 255})
    {
        Log::SetLogLevel(PLUGIN_NAME, static_cast<Channel::Enum>(level));
        std::cout << "level " << level << '\n';
        if (level < Channel::SEV_FATAL)
            LOG_FATAL("fatal at level %d", level);
        LOG_ERROR("error at level %d", level);
        LOG_NOTICE("notice at level %d", level);
        LOG_DEBUG("debug at level %d", level);
    }
}

#line 5000 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogBus()
{
    LOG_INFO("bus %d", 1);
    Log::Trace(Channel::SEV_WARNING, "NWNX_Direct", "/abs/path/Direct.cpp", 7, "bus %s", "direct");
    LOG_NOTICE("bus nested");
}

void LogFromSubscriber()
{
    LOG_NOTICE("logged from a subscriber");
}

#line 6000 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogFatal()
{
    LOG_FATAL("fatal %d", 1);
}

#line 7000 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogAssert()
{
    Assert::FailInternal("a == b", "/abs/path/Checked.cpp", 42, "the message");
    Assert::FailInternal(nullptr, "Checked.cpp", 43, nullptr);
}

#line 8000 "/nwnx/home/NWNXLib/Log/tests/GoldenJson.cpp"
void LogFieldsF()
{
    LOG_INFO_F("Successful SQL query. Query ID: '%i', Query: '%s', Rows affected: '%u'.", 412, "SELECT 1", 1u)
        .Msg("sql query").With("query_id", 412).With("query", "SELECT 1").With("rows", 1u).With("ms", 0.84).With("db", "MYSQL");
    LOG_WARNING_F("Failed SQL query. Query ID: '%i', Query: '%s'.", 413, "SELEC")
        .Msg("sql query failed").With("query_id", 413).With("query", "SELEC").Err("syntax error", 1064);
    LOG_NOTICE_F("no fields, no Msg %d", 1);
    LOG_DEBUG_F("chat").With("channel", "talk").With("speaker", Log::Oid{0x7f000000}).With("text", "<c\xff\x01\x01>hi</c> \xe9");
}
