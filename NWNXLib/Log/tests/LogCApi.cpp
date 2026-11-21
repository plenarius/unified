// Unit tests for the bridge entry points in Core/NWNXLog.h, loaded with dlopen as a bridge would: text and JSON
// output, UTF-8 under the locale JSON encoding, level and logger filtering, NULL arguments, bad and deep fields, the
// context source and calls from threads.
#include "nwnx.hpp"
#include "Log/Json.hpp"
#include "../../../Core/NWNXLog.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

using namespace NWNXLib;
using Channel = Log::Channel;

static int s_failures;
static int s_checks;

#define CHECK_EQ(actual, expected) \
    do { s_checks++; const std::string a_ = (actual), e_ = (expected); if (a_ != e_) { s_failures++; \
         std::fprintf(stderr, "%s:%d: %s\n  expected: %s\n  actual:   %s\n", __FILE__, __LINE__, #actual, e_.c_str(), a_.c_str()); } } while (0)

static std::string s_tmp;
static decltype(&NWNX_Log_Emit) Emit;
static decltype(&NWNX_Log_IsEnabled) IsEnabled;

// Runs fn with stdout going to a file and returns what it printed.
template <typename Fn>
static std::string Capture(Fn&& fn)
{
    std::cout.flush();
    std::fflush(stdout);
    const int saved = dup(1);
    const int fd = open(s_tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, 1);
    close(fd);
    fn();
    std::cout.flush();
    std::fflush(stdout);
    dup2(saved, 1);
    close(saved);
    std::ifstream in(s_tmp, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The JSON lines without their time member.
static std::string NoTime(const std::string& out)
{
    return std::regex_replace(out, std::regex("\"time\":[0-9]+,"), "");
}

static std::string Json(int level, const char* src, const char* logger, const char* msg, const char* fields)
{
    return NoTime(Capture([&] { Emit(level, src, logger, msg, fields); }));
}

static void TestJson()
{
    Log::SetFormat(Log::Format::Json);
    // Members come in key order, as nlohmann keeps them.
    CHECK_EQ(Json(6, "dotnet", "Anvil.Services.Bank", "deposit", R"({"gold":250,"who":"Ann","tags":["a"]})"),
             R"({"level":30,"lvl":"info","msg":"deposit","src":"dotnet","plugin":"NWNX_DotNET","logger":"Anvil.Services.Bank","gold":250,"tags":["a"],"who":"Ann"})" "\n");
    CHECK_EQ(Json(4, "swig", nullptr, "w", nullptr),
             R"({"level":40,"lvl":"warn","msg":"w","src":"swig","plugin":"NWNX_SWIG"})" "\n");
    CHECK_EQ(Json(3, "ruby", "", "e", "{}"),
             R"({"level":50,"lvl":"error","msg":"e","src":"ruby","plugin":"NWNX_Ruby"})" "\n");
    // Reserved keys are renamed, with a warning the first time, as for any record; UTF-8 passes through.
    CHECK_EQ(Json(6, "dotnet", nullptr, "caf\xc3\xa9", R"({"msg":"x","name":"é"})"),
             R"({"level":40,"lvl":"warn","msg":"Log field 'msg' is a reserved key; it is written as '_msg'.","src":"nwnx","plugin":"NWNX_DotNET"})" "\n"
             "{\"level\":30,\"lvl\":\"info\",\"msg\":\"caf\xc3\xa9\",\"src\":\"dotnet\",\"plugin\":\"NWNX_DotNET\",\"_msg\":\"x\",\"name\":\"\xc3\xa9\"}\n");
}

static void TestBadArguments()
{
    Log::SetFormat(Log::Format::Json);
    // NULL everything: Core's level, src "bridge", empty message.
    CHECK_EQ(Json(6, nullptr, nullptr, nullptr, nullptr),
             R"({"level":30,"lvl":"info","msg":"","src":"bridge","plugin":"NWNX_Core"})" "\n");
    CHECK_EQ(Json(6, "lua", nullptr, "m", nullptr),
             R"({"level":30,"lvl":"info","msg":"m","src":"bridge","plugin":"NWNX_Core","bridge":"lua"})" "\n");
    // Invalid JSON keeps the record with a warning field; a non-object goes under "data".
    const std::string bad = Json(6, "dotnet", nullptr, "m", "{\"a\":");
    CHECK_EQ(std::to_string(bad.find("\"msg\":\"m\"") != std::string::npos && bad.find(",\"fields_error\":\"[json.exception.parse_error") != std::string::npos), "1");
    CHECK_EQ(Json(6, "dotnet", nullptr, "m", "[1,2]"),
             R"({"level":30,"lvl":"info","msg":"m","src":"dotnet","plugin":"NWNX_DotNET","data":[1,2]})" "\n");
    CHECK_EQ(Json(6, "dotnet", nullptr, "m", "null"),
             R"({"level":30,"lvl":"info","msg":"m","src":"dotnet","plugin":"NWNX_DotNET"})" "\n");
    // Invalid UTF-8 in the fields is a parse error too, and its bytes do not reach the output.
    const std::string utf = Json(6, "dotnet", nullptr, "m", "{\"a\":\"\xff\"}");
    CHECK_EQ(std::to_string(utf.find("fields_error") != std::string::npos && utf.find('\xff') == std::string::npos), "1");
    // FATAL and below are clamped to ERROR (no abort); above DEBUG to DEBUG.
    CHECK_EQ(Json(2, "dotnet", nullptr, "f", nullptr),
             R"({"level":50,"lvl":"error","msg":"f","src":"dotnet","plugin":"NWNX_DotNET"})" "\n");
    CHECK_EQ(Json(-5, "dotnet", nullptr, "f", nullptr),
             R"({"level":50,"lvl":"error","msg":"f","src":"dotnet","plugin":"NWNX_DotNET"})" "\n");
    CHECK_EQ(Json(99, "dotnet", nullptr, "d", nullptr),
             R"({"level":20,"lvl":"debug","msg":"d","src":"dotnet","plugin":"NWNX_DotNET"})" "\n");
}

// fieldsJson: an object holding a member nested `levels` deep in all.
static std::string Nested(int levels)
{
    return "{\"d\":" + std::string(levels - 1, '[') + std::string(levels - 1, ']') + "}";
}

static void TestDepth()
{
    Log::SetFormat(Log::Format::Json);
    const std::string ok = Json(6, "dotnet", nullptr, "m", Nested(64).c_str());
    CHECK_EQ(std::to_string(ok.find("\"d\":" + std::string(63, '[')) != std::string::npos && ok.find("fields_error") == std::string::npos), "1");
    // Deeper keeps the record, without the fields; 100,000 levels must not exhaust the stack.
    for (int levels : {65, 100000})
        CHECK_EQ(Json(6, "dotnet", nullptr, "m", Nested(levels).c_str()),
                 R"({"level":30,"lvl":"info","msg":"m","src":"dotnet","plugin":"NWNX_DotNET","fields_error":"fields nested deeper than 64 levels"})" "\n");
}

static void TestFiltering()
{
    Log::SetFormat(Log::Format::Json);
    Log::SetLogLevel("NWNX_DotNET", Channel::SEV_INFO);
    CHECK_EQ(Json(7, "dotnet", nullptr, "d", "{\"a\":1}"), "");
    CHECK_EQ(std::to_string(IsEnabled(7, "dotnet", nullptr)), "0");
    CHECK_EQ(std::to_string(IsEnabled(6, "dotnet", nullptr)), "1");
    // Each bridge has its own level.
    Log::SetLogLevel("NWNX_SWIG", Channel::SEV_ERROR);
    CHECK_EQ(Json(4, "swig", nullptr, "w", nullptr), "");
    CHECK_EQ(std::to_string(IsEnabled(4, "swig", nullptr)), "0");
    // NWNX_LOG_LOGGER_LEVELS (set in main) overrides per logger, both ways.
    CHECK_EQ(std::to_string(IsEnabled(7, "dotnet", "chatty")), "1");
    CHECK_EQ(Json(7, "dotnet", "chatty", "d", nullptr),
             R"({"level":20,"lvl":"debug","msg":"d","src":"dotnet","plugin":"NWNX_DotNET","logger":"chatty"})" "\n");
    CHECK_EQ(std::to_string(IsEnabled(4, "dotnet", "quiet")), "0");
    CHECK_EQ(Json(4, "dotnet", "quiet", "w", nullptr), "");
    CHECK_EQ(std::to_string(IsEnabled(4, "dotnet", "other")), "1");
    Log::SetLogLevel("NWNX_DotNET", Channel::SEV_DEBUG);
    Log::SetLogLevel("NWNX_SWIG", Channel::SEV_DEBUG);
}

static void TestText()
{
    Log::SetFormat(Log::Format::Text);
    // Fields show in text: bridge records have no legacy line to keep.
    CHECK_EQ(Capture([] { Emit(6, "dotnet", "Bank", "deposit", R"({"gold":250,"who":"Ann Lee"})"); }),
             "I [NWNX_DotNET] deposit logger=Bank gold=250 who=\"Ann Lee\"\n");
    CHECK_EQ(Capture([] { Emit(4, nullptr, nullptr, nullptr, nullptr); }), "W [NWNX_Core] \n");
    // The message and string fields are UTF-8 in, game-locale (cp1252) bytes out, like the rest of the line.
    CHECK_EQ(Capture([] { Emit(6, "dotnet", nullptr, "caf\xc3\xa9", "{\"who\":\"Ren\xc3\xa9" "e\"}"); }),
             "I [NWNX_DotNET] caf\xe9 who=Ren\xe9" "e\n");
    Log::SetFormat(Log::Format::Json);
}

static void TestLocaleEncoding()
{
    // Bridges pass UTF-8: JSON keeps the message and string fields as they are, never re-encoding them from the locale.
    Log::SetFormat(Log::Format::Json);
    Log::Json::Options options = Log::GetJsonOptions();
    const Log::Json::Options saved = options;
    options.encoding = Log::Json::Encoding::Locale;
    Log::SetJsonOptions(options);
    CHECK_EQ(Json(6, "dotnet", nullptr, "caf\xc3\xa9", "{\"who\":\"Ren\xc3\xa9" "e\"}"),
             "{\"level\":30,\"lvl\":\"info\",\"msg\":\"caf\xc3\xa9\",\"src\":\"dotnet\",\"plugin\":\"NWNX_DotNET\",\"who\":\"Ren\xc3\xa9" "e\"}\n");
    Log::SetJsonOptions(saved);
}

static void Context(Log::Record& record)
{
    record.With("script", "bank_deposit");
}

static void TestContext()
{
    Log::SetFormat(Log::Format::Json);
    Log::SetContextSource(&Context);
    CHECK_EQ(Json(6, "dotnet", nullptr, "m", R"({"a":1})"),
             R"({"level":30,"lvl":"info","msg":"m","src":"dotnet","plugin":"NWNX_DotNET","script":"bank_deposit","a":1})" "\n");
    Log::SetContextSource(nullptr);
}

static void TestThreads()
{
    Log::SetFormat(Log::Format::Json);
    constexpr int threads = 8, records = 200;
    const std::string out = Capture([]
    {
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; t++)
            workers.emplace_back([t]
            {
                const std::string fields = "{\"t\":" + std::to_string(t) + "}";
                for (int i = 0; i < records; i++)
                {
                    if (IsEnabled(6, "dotnet", "worker"))
                        Emit(6, "dotnet", "worker", "tick", (i % 10) ? fields.c_str() : "{bad");
                }
            });
        for (auto& w : workers)
            w.join();
    });
    int lines = 0, valid = 0;
    std::istringstream in(out);
    for (std::string line; std::getline(in, line); lines++)
        valid += json::accept(line) && json::parse(line)["msg"] == "tick";
    CHECK_EQ(std::to_string(lines), std::to_string(threads * records));
    CHECK_EQ(std::to_string(valid), std::to_string(threads * records));
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <library exporting NWNX_Log_Emit>\n", argv[0]);
        return 2;
    }
    const char* tmp = std::getenv("TMPDIR");
    s_tmp = std::string(tmp && *tmp ? tmp : "/tmp") + "/nwnx-log-capi-" + std::to_string(getpid());
    setenv("NWNX_LOG_LOGGER_LEVELS", "chatty:7,quiet:3", 1);
    unsetenv("TERM");

    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib)
    {
        std::fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    Emit = reinterpret_cast<decltype(Emit)>(dlsym(lib, "NWNX_Log_Emit"));
    IsEnabled = reinterpret_cast<decltype(IsEnabled)>(dlsym(lib, "NWNX_Log_IsEnabled"));
    if (!Emit || !IsEnabled)
    {
        std::fprintf(stderr, "NWNX_Log_Emit or NWNX_Log_IsEnabled not exported\n");
        return 2;
    }

    Log::SetColorOutput(false);
    Log::SetPrintTimestamp(false);
    Log::SetPrintPlugin(true);
    Log::SetPrintSource(true);
    for (const char* plugin : {"NWNX_DotNET", "NWNX_SWIG", "NWNX_Ruby", "NWNX_Core"})
        Log::SetLogLevel(plugin, Channel::SEV_DEBUG);

    TestJson();
    TestBadArguments();
    TestDepth();
    TestFiltering();
    TestText();
    TestLocaleEncoding();
    TestContext();
    TestThreads();

    dlclose(lib);
    std::remove(s_tmp.c_str());
    std::printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
