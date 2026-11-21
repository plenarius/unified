// Checks that a log level set from any plugin or translation unit reaches every LOG_* call site of
// that plugin and no other, with plugins loaded the way NWNX loads them (dlopen, RTLD_LOCAL).
// Usage: LogLevels <pluginA.so> <pluginB.so>
#include "nwnx.hpp"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

using namespace NWNXLib;
using Channel = Log::Channel;

extern "C" void LogTest_CoreDebug();

struct TestPlugin
{
    void (*Debug)();
    void (*Debug2)();
    void (*SetLogLevel)(const char*, int);
};

static std::mutex s_mutex;
static std::map<std::string, int> s_lines; // "plugin: message" -> count
static int s_failures = 0;

static bool Load(const char* path, TestPlugin& plugin)
{
    void* handle = dlopen(path, RTLD_NOW | RTLD_NODELETE);
    if (!handle)
    {
        std::fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
        return false;
    }
    plugin.Debug = reinterpret_cast<void (*)()>(dlsym(handle, "LogTest_Debug"));
    plugin.Debug2 = reinterpret_cast<void (*)()>(dlsym(handle, "LogTest_Debug2"));
    plugin.SetLogLevel = reinterpret_cast<void (*)(const char*, int)>(dlsym(handle, "LogTest_SetLogLevel"));
    return plugin.Debug && plugin.Debug2 && plugin.SetLogLevel;
}

static void Expect(const char* step, TestPlugin& a, TestPlugin& b, bool aShown, bool bShown, bool coreShown)
{
    s_lines.clear();
    a.Debug();
    a.Debug2();
    b.Debug();
    b.Debug2();
    LogTest_CoreDebug();

    const std::map<std::string, int> expected = [&]
    {
        std::map<std::string, int> e;
        if (aShown)
        {
            e["NWNX_LogPluginA: first translation unit"] = 1;
            e["NWNX_LogPluginA: second translation unit"] = 1;
        }
        if (bShown)
        {
            e["NWNX_LogPluginB: first translation unit"] = 1;
            e["NWNX_LogPluginB: second translation unit"] = 1;
        }
        if (coreShown)
            e["NWNXLib: core"] = 1;
        return e;
    }();

    if (s_lines != expected)
    {
        s_failures++;
        std::fprintf(stderr, "FAIL %s; logged:\n", step);
        for (const auto& [line, count] : s_lines)
            std::fprintf(stderr, "  %dx %s\n", count, line.c_str());
    }
    else
    {
        std::fprintf(stderr, "ok   %s\n", step);
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <pluginA.so> <pluginB.so>\n", argv[0]);
        return 2;
    }

    // The lines themselves are checked through the message bus; keep them off the test output.
    int devNull = open("/dev/null", O_WRONLY);
    dup2(devNull, 1);
    close(devNull);

    MessageBus::Subscribe("NWNX_CORE_LOG_MESSAGE", [](const MessageBus::Message& msg)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_lines[msg[1] + ": " + msg[4]]++;
    });

    TestPlugin a{}, b{};
    if (!Load(argv[1], a) || !Load(argv[2], b))
        return 2;

    // Every call site runs (and caches its slot) before any level changes.
    Expect("default levels", a, b, false, false, false);
    a.SetLogLevel("NWNX_LogPluginB", Channel::SEV_DEBUG);
    Expect("plugin A raises plugin B", a, b, false, true, false);
    Log::SetLogLevel("NWNX_LogPluginA", Channel::SEV_DEBUG);
    Expect("the executable raises plugin A", a, b, true, true, false);
    b.SetLogLevel("NWNXLib", Channel::SEV_DEBUG);
    Expect("plugin B raises core", a, b, true, true, true);
    b.SetLogLevel("NWNX_LogPluginA", Channel::SEV_NOTICE);
    Expect("plugin B lowers plugin A", a, b, false, true, true);
    if (Log::GetLogLevel("NWNX_LogPluginB") != Channel::SEV_DEBUG || Log::GetLogLevel("NWNX_LogPluginA") != Channel::SEV_NOTICE)
    {
        s_failures++;
        std::fprintf(stderr, "FAIL GetLogLevel does not read back the levels\n");
    }

    // Levels change while another thread logs (run under TSan to check the slots are race free).
    std::atomic<bool> stop = false;
    std::thread worker([&]
    {
        while (!stop.load())
        {
            a.Debug();
            b.Debug2();
        }
    });
    for (int i = 0; i < 2000; i++)
    {
        Log::SetLogLevel("NWNX_LogPluginA", i % 2 ? Channel::SEV_DEBUG : Channel::SEV_NOTICE);
        b.SetLogLevel("NWNX_LogPluginB", i % 3 ? Channel::SEV_DEBUG : Channel::SEV_INFO);
    }
    stop = true;
    worker.join();

    return s_failures ? 1 : 0;
}
