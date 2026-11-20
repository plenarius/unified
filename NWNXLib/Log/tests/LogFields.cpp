// Unit tests for LOG_*_F and Record fields: value encoding, duplicate and reserved keys, key order, Msg(),
// the context source, text output with and without NWNX_CORE_LOG_TEXT_FIELDS, and that a filtered call
// evaluates nothing.
#include "nwnx.hpp"
#include "Log/Json.hpp"
#include "External/json/json.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

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

// The record's members after the fixed origin keys, or the whole line if it has none.
static std::string Fields(const std::string& line)
{
    const std::string marker = "\"line\":";
    const size_t pos = line.find(marker);
    if (pos == std::string::npos)
        return line;
    const size_t start = line.find_first_not_of("0123456789", pos + marker.size());
    return line.substr(start, line.size() - start - (line.back() == '\n' ? 2 : 1));
}

static std::string Json(const std::string& line)
{
    try
    {
        return nlohmann::json::parse(line).dump();
    }
    catch (const std::exception& e)
    {
        return std::string("not JSON: ") + e.what() + ": " + line;
    }
}

static int s_evaluated;
static int Evaluate(int value)
{
    s_evaluated++;
    return value;
}

// Counts how often the message is formatted.
struct Formatted {};
static int s_formatted;
static std::ostream& operator<<(std::ostream& os, Formatted)
{
    s_formatted++;
    return os << "formatted";
}

static void TestValues()
{
    const std::string line = Capture([]
    {
        const std::string runtimeKey = "run\"time";
        LOG_NOTICE_F("values").With("i8", int8_t(-8)).With("u64", std::numeric_limits<uint64_t>::max())
            .With("i64", std::numeric_limits<int64_t>::min()).With("b", true).With("d", 0.1).With("f", 1.5f)
            .With("nan", std::nan("")).With("s", "caf\xe9").With("utf8", Log::Utf8{"\xc3\xa9\xff"})
            .With("game", Log::Game{"\xc3\xa9"}).With("std", std::string("a\0b", 3)).With("oid", Log::Oid{0x8000012a})
            .With(runtimeKey, Log::RawJson{"{\"x\":[1,2]}"}).With("empty_raw", Log::RawJson{""})
            .With("null_ptr", static_cast<const char*>(nullptr))
            .With("opt_none", std::optional<int>()).With("opt", std::optional<std::string_view>("v"))
            .With("json", nlohmann::json{{"k", "<c\xff\x01\x01>v</c>"}, {"a", {1, "t", nullptr}}, {"e", nlohmann::json::array()}})
            .Err("it \"failed\"", -632);
    });
    CHECK_EQ(Fields(line), R"(,"i8":-8,"u64":18446744073709551615,"i64":-9223372036854775808,"b":true,"d":0.1,"f":1.5,)"
                           R"("nan":null,"s":"café","utf8":"é�","game":"Ã©","std":"a\u0000b","oid":"8000012a",)"
                           R"("run\"time":{"x":[1,2]},"empty_raw":null,"null_ptr":"","opt":"v",)"
                           R"("json":{"a":[1,"t",null],"e":[],"k":"v"},"err":{"message":"it \"failed\"","code":-632})");
    CHECK_EQ(Json(line).substr(0, 1), "{");
}

static void TestKeys()
{
    // A later value replaces an earlier one at the later position; reserved keys get a '_' and one warning each.
    const std::string out = Capture([]
    {
        LOG_NOTICE_F("dupes").With("a", 1).With("b", 2).With("a", "three").With("c", 4).With("b", 5);
        LOG_NOTICE_F("reserved").With("msg", "x").With("level", 1).With("msg", "y").With("_msg", "z");
        LOG_NOTICE_F("reserved again").With("msg", 2);
    });
    std::istringstream lines(out);
    std::string dupes, warnMsg, warnLevel, reserved, again, extra;
    std::getline(lines, dupes);
    std::getline(lines, warnMsg);
    std::getline(lines, warnLevel);
    std::getline(lines, reserved);
    std::getline(lines, again);
    CHECK_EQ(Fields(dupes), R"(,"a":"three","c":4,"b":5)");
    CHECK_EQ(Json(warnMsg).find("\"msg\":\"Log field 'msg' is a reserved key; it is written as '_msg'.\"") != std::string::npos ? "warned" : warnMsg, "warned");
    CHECK_EQ(Json(warnLevel).find("'level' is a reserved key") != std::string::npos ? "warned" : warnLevel, "warned");
    // "_msg" given directly is the same key as the renamed "msg", so it replaces it.
    CHECK_EQ(Fields(reserved), R"(,"_level":1,"_msg":"z")");
    CHECK_EQ(Fields(again), R"(,"_msg":2)");
    CHECK_EQ(std::getline(lines, extra) ? extra : "", "");
}

static void TestMsgSrcOrigin()
{
    s_formatted = 0;
    const std::string line = Capture([]
    {
        LOG_NOTICE_F("legacy %s", Formatted{}).Msg(std::string("stable")).Src("script").Origin("logger", "bank").With("k", 1);
    });
    CHECK_EQ(std::to_string(s_formatted), "0"); // JSON with Msg() never formats the legacy text
    CHECK_EQ(line.substr(line.find("\"msg\"")), R"("msg":"stable","src":"script","plugin":"NWNX_LogFields","file":"LogFields.cpp","line":)"
                                                + line.substr(line.find("\"line\":") + 7, 3) + R"(,"logger":"bank","k":1})" "\n");

    const std::string plain = Capture([] { LOG_NOTICE_F("legacy %s", Formatted{}).With("k", 1); });
    CHECK_EQ(std::to_string(s_formatted), "1");
    CHECK_EQ(plain.substr(plain.find("\"msg\""), 25), R"("msg":"legacy formatted",)");
}

static void TestContext()
{
    Log::SetContextSource([](Log::Record& record) { record.With("script", "ctx_script").With("oid", Log::Oid{1}); });
    const std::string line = Capture([] { LOG_NOTICE_F("ctx").With("oid", "call wins").With("n", 1); });
    Log::SetContextSource(nullptr);
    CHECK_EQ(Fields(line), R"(,"script":"ctx_script","oid":"call wins","n":1)");
}

static void TestFiltered()
{
    Log::SetLogLevel(PLUGIN_NAME, Channel::SEV_WARNING);
    s_evaluated = 0;
    s_formatted = 0;
    const std::string out = Capture([]
    {
        LOG_INFO_F("filtered %d %s", Evaluate(1), Formatted{}).With("a", Evaluate(2)).Msg(std::to_string(Evaluate(3)));
        LOG_DEBUG_F("filtered").With("a", Evaluate(4));
    });
    CHECK_EQ(out, "");
    CHECK_EQ(std::to_string(s_evaluated) + " " + std::to_string(s_formatted), "0 0");

    // An enabled call evaluates each argument once.
    Capture([] { LOG_WARNING_F("enabled %d", Evaluate(1)).With("a", Evaluate(2)); });
    CHECK_EQ(std::to_string(s_evaluated), "2");
    Log::SetLogLevel(PLUGIN_NAME, Channel::SEV_DEBUG);

    // The macro is one expression, so it works in an unbraced if/else.
    bool elseTaken = false;
    if (s_evaluated < 0)
        LOG_INFO_F("never").With("a", 1);
    else
        elseTaken = true;
    CHECK_EQ(elseTaken ? "else" : "if", "else");
}

static void TestText()
{
    Log::SetFormat(Log::Format::Text);
    s_evaluated = 0;
    // Text lines of _F calls are the same as LOG_* lines; fields only show with TEXT_FIELDS.
    const std::string plain = Capture([] { LOG_NOTICE("text %d", 1); });
    const std::string fields = Capture([] { LOG_NOTICE_F("text %d", 1).With("a", Evaluate(1)).Msg("stable"); });
    CHECK_EQ(fields.substr(fields.find("] text")), plain.substr(plain.find("] text")));
    CHECK_EQ(std::to_string(s_evaluated), "1");

    Log::SetTextFields(true);
    const std::string shown = Capture([]
    {
        LOG_NOTICE_F("text %d", 1).With("n", 2).With("bare", "word").With("spaced", "two words").With("q", "a\"b")
            .With("empty", "").With("obj", Log::RawJson{"{\"x\":1}"}).With("msg", "renamed").With("n", 3);
    });
    CHECK_EQ(shown.substr(shown.find("text 1")), "text 1 bare=word spaced=\"two words\" q=\"a\\\"b\" empty=\"\" obj={\"x\":1} _msg=renamed n=3\n");
    // Strings keep the game-locale bytes and colours of the rest of the line; UTF-8 values are converted.
    const std::string locale = Capture([]
    {
        LOG_NOTICE_F("caf\xe9").With("name", "caf\xe9").With("c", "<c\xff\x80\xfe>red</c>").With("u", Log::Utf8{"caf\xc3\xa9"})
            .With("nl", "a\nb").With("j", nlohmann::json{{"s", "\xc3\xa9"}}).With("o", Log::Oid{0x8000012a});
    });
    CHECK_EQ(locale.substr(locale.find("caf")), "caf\xe9 name=caf\xe9 c=<c\xff\x80\xfe>red</c> u=caf\xe9 nl=\"a\\nb\" j={\"s\":\"\xc3\xa9\"} o=8000012a\n");
    Log::SetTextFields(false);

    // TextFields() shows them for one record only.
    const std::string one = Capture([] { Log::Record r(Channel::SEV_NOTICE, PLUGIN_NAME, __FILE__, __LINE__); r.TextFields().With("k", 1); r.Stream() << "own"; r.Write(); });
    CHECK_EQ(one.substr(one.find("own")), "own k=1\n");
    // Origin keys come first, as in JSON.
    const std::string origin = Capture([] { Log::Record r(Channel::SEV_NOTICE, PLUGIN_NAME, __FILE__, __LINE__); r.TextFields().Origin("logger", "a b").With("k", 1); r.Stream() << "own"; r.Write(); });
    CHECK_EQ(origin.substr(origin.find("own")), "own logger=\"a b\" k=1\n");
    // ... with the context, which text records otherwise don't get.
    Log::SetContextSource([](Log::Record& record) { record.With("script", "ctx"); });
    const std::string ctx = Capture([] { Log::Record r(Channel::SEV_NOTICE, PLUGIN_NAME, __FILE__, __LINE__); r.TextFields().With("k", 1); r.Stream() << "own"; r.Write(); });
    const std::string noCtx = Capture([] { LOG_NOTICE_F("plain").With("k", 1); });
    Log::SetContextSource(nullptr);
    CHECK_EQ(ctx.substr(ctx.find("own")), "own script=ctx k=1\n");
    CHECK_EQ(noCtx.substr(noCtx.find("plain")), "plain\n");
    const std::string textOnly = Capture([] { LOG_NOTICE_F("text only %d", 1).TextOnly(); });
    CHECK_EQ(textOnly.substr(textOnly.find("text only")), "text only 1\n");
    Log::SetFormat(Log::Format::Json);
    CHECK_EQ(Capture([] { LOG_NOTICE_F("text only %d", 1).TextOnly(); }), "");
}

int main()
{
    const char* tmp = std::getenv("TMPDIR");
    s_tmp = std::string(tmp && *tmp ? tmp : "/tmp") + "/nwnx-log-fields-" + std::to_string(getpid());
    unsetenv("TERM");
    Log::SetColorOutput(false);
    Log::SetPrintTimestamp(false);
    Log::SetPrintPlugin(true);
    Log::SetPrintSource(true);
    Log::SetLogLevel(PLUGIN_NAME, Channel::SEV_DEBUG);
    Log::SetFormat(Log::Format::Json);

    TestValues();
    TestKeys();
    TestMsgSrcOrigin();
    TestContext();
    TestFiltered();
    TestText();

    std::remove(s_tmp.c_str());
    std::printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
