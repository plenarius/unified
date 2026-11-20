// Golden test for the text log output: drives the logger through every format and colour option and
// compares stdout, the log file and the NWNX_CORE_LOG_MESSAGE payloads byte for byte with a fixture.
// Usage: LogGolden <fixture> [--update]; on a mismatch it writes LogGolden.actual.txt to the working directory.
#include "nwnx.hpp"

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
#include <sys/wait.h>
#include <unistd.h>

using namespace NWNXLib;
using Channel = Log::Channel;

static time_t s_now = 1709611569; // 2024-03-05 04:06:09 UTC

// Fixed clock: interposes libc's time(), so the logger needs no test hook.
extern "C" time_t time(time_t* t) noexcept
{
    if (t)
        *t = s_now;
    return s_now;
}

static std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void SetColorMode(int mode)
{
    Log::SetColorOutput(mode & 1);
    Log::SetForceColor(mode & 2);
}

static void SetFormat(int combo)
{
    Log::SetPrintTimestamp(combo & 1);
    Log::SetPrintDate(combo & 2);
    Log::SetPrintPlugin(combo & 4);
    Log::SetPrintSource(combo & 8);
}

static void Section(const char* fmt, int a = 0, int b = 0)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    std::cout << "## " << buf << '\n';
}

// Defined at the end of the file under #line, so their __FILE__/__LINE__ never change.
void LogAllLevels(int combo);
void LogDirect();
void LogSpecial();
void LogLevelFilter();
void LogBus();
void LogFromSubscriber();
void LogAfterUnsubscribe();
void LogFatal();

// Runs LogFatal in a child (it aborts) and prints its output up to the assertion text, which has
// unstable file and line numbers.
static void RunFatal(int colorMode)
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
        SetColorMode(colorMode);
        SetFormat(15);
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
    auto assertPos = out.find("ASSERTION FAILURE");
    std::cout << out.substr(0, assertPos) << (assertPos != std::string::npos ? "<ASSERTION FAILURE>" : "<no assert>")
              << " signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << '\n';
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

    // Colour modes: 0 off, 1 auto (stdout is not a tty here, so no colour), 2 force with colour off, 3 force.
    for (int colorMode = 0; colorMode < 4; colorMode++)
    {
        SetColorMode(colorMode);
        for (int combo = 0; combo < 16; combo++)
        {
            SetFormat(combo);
            Section("color=%d format=%d", colorMode, combo);
            LogAllLevels(combo);
            LogDirect();
        }
    }

    Section("special");
    SetColorMode(3);
    SetFormat(15);
    LogSpecial();
    s_now = 1956527998; // 2031-12-31 23:59:58 UTC
    LogAllLevels(-1);
    s_now = 1709611569;

    Section("levels");
    SetColorMode(0);
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
    LogAfterUnsubscribe();

    Section("fatal");
    RunFatal(0);
    RunFatal(3);

    Section("file");
    Log::SetLogFile(filePath);
    SetColorMode(3);
    for (int combo = 0; combo < 16; combo++)
    {
        SetFormat(combo);
        LogAllLevels(combo);
    }

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
    std::string tmpDir = std::string(tmp && *tmp ? tmp : "/tmp") + "/nwnx-log-golden-XXXXXX";
    if (!mkdtemp(tmpDir.data()))
    {
        std::perror("mkdtemp");
        return 2;
    }

    std::string actual = Run(tmpDir);
    rmdir(tmpDir.c_str());

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
    const std::string actualPath = "LogGolden.actual.txt";
    std::ofstream(actualPath, std::ios::binary) << actual;
    std::fprintf(stderr, "Log output differs from %s at byte %zu (expected %zu bytes, got %zu); wrote %s\n",
                 fixture.c_str(), pos, expected.size(), actual.size(), actualPath.c_str());
    return 1;
}

#line 900 "/nwnx/home/NWNXLib/Log/tests/Golden.cpp"
// A type whose operator<< leaves the stream in hex mode.
struct LeaksHex {};
static std::ostream& operator<<(std::ostream& os, LeaksHex)
{
    return os << std::hex << std::showbase << 255;
}

// A type whose operator<< logs while the outer line is being formatted.
struct LogsWhileFormatting {};
static std::ostream& operator<<(std::ostream& os, LogsWhileFormatting)
{
    LOG_NOTICE("nested %s", "inner");
    return os << "outer-arg";
}

#line 1000 "/nwnx/home/NWNXLib/Log/tests/Golden.cpp"
void LogAllLevels(int combo)
{
    LOG_DEBUG("debug %d: %s", combo, std::string("std::string"));
    LOG_INFO("info %u %.3f %x %%", 42u, 3.14159, 255);
    LOG_NOTICE("notice %-8s|%+d|%08.2f|%#x", "left", 7, -2.5, 48879);
    LOG_WARNING("warning bool=%s char=%c %s", true, 'Z', 1.0 / 3.0);
    LOG_ERROR("error");
}

#line 2000 "/nwnx/home/NWNXLib/Log/tests/Golden.cpp"
void LogDirect()
{
    Log::Trace(Channel::SEV_NOTICE, "NWNX_Direct", "/abs/path/Direct.cpp", 12, "absolute path");
    Log::Trace(Channel::SEV_INFO, "NWNX_Direct", "Plain.cpp", 0, "no directory %s", "here");
    Log::Trace(Channel::SEV_WARNING, "NWNX_Direct", "trailing/", -1, "trailing slash");
    Log::Trace(Channel::SEV_ERROR, "NWNX_Direct", "", 99999, "empty file");
}

#line 3000 "/nwnx/home/NWNXLib/Log/tests/Golden.cpp"
void LogSpecial()
{
    LOG_NOTICE("%s", std::string(10000, 'x') + "end");
    LOG_NOTICE("short after long");
    LOG_NOTICE("");
    LOG_NOTICE("two\nlines");
    LOG_NOTICE("embedded %s nul", std::string("a\0b", 3));
    LOG_NOTICE("%s then %s", LeaksHex{}, 255);
    LOG_NOTICE("next line %s %d %f", 255, 255, 1.5);
    LOG_NOTICE("%s", LogsWhileFormatting{});
    LOG_NOTICE("%5.1s|%-5c|%05d|%e|%g|%o|%X", "abc", 'q', -42, 12345.678, 0.0001, 8, 0xbeef);
    LOG_NOTICE("unicode \xc3\xa9 and cp1252 \xe9");
}

#line 4000 "/nwnx/home/NWNXLib/Log/tests/Golden.cpp"
void LogLevelFilter()
{
    for (int level : {0, 1, 2, 4, 5, 7, 255})
    {
        Log::SetLogLevel(PLUGIN_NAME, static_cast<Channel::Enum>(level));
        std::cout << "level " << level << " reads back " << Log::GetLogLevel(PLUGIN_NAME) << '\n';
        if (level < Channel::SEV_FATAL)
            LOG_FATAL("fatal at level %d", level);
        LOG_ERROR("error at level %d", level);
        LOG_WARNING("warning at level %d", level);
        LOG_NOTICE("notice at level %d", level);
        LOG_INFO("info at level %d", level);
        LOG_DEBUG("debug at level %d", level);
    }
    std::cout << "unknown plugin reads " << Log::GetLogLevel("NWNX_NeverSet") << '\n';
    Log::Trace(Channel::SEV_NOTICE, "NWNX_NeverSet", "x.cpp", 1, "default notice is shown");
    Log::Trace(Channel::SEV_INFO, "NWNX_NeverSet", "x.cpp", 2, "default info is hidden");
}

#line 5000 "/nwnx/home/NWNXLib/Log/tests/Golden.cpp"
void LogBus()
{
    LOG_INFO("bus %d", 1);
    Log::Trace(Channel::SEV_WARNING, "NWNX_Direct", "/abs/path/Direct.cpp", 7, "bus %s", "direct");
    LOG_NOTICE("bus nested");
    LOG_DEBUG("bus %s", "debug");
}

void LogFromSubscriber()
{
    LOG_NOTICE("logged from a subscriber");
}

void LogAfterUnsubscribe()
{
    LOG_INFO("after unsubscribe");
}

#line 6000 "/nwnx/home/NWNXLib/Log/tests/Golden.cpp"
void LogFatal()
{
    LOG_FATAL("fatal %d", 1);
}
