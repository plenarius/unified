// Unit tests for the JSON string, number and key encoders. Every encoded value is parsed back with nlohmann
// and checked as valid UTF-8; code page output is compared with glibc's iconv.
#include "nwnx.hpp"
#include "Log/Json.hpp"
#include "External/json/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iconv.h>
#include <limits>
#include <random>
#include <string>

using namespace NWNXLib;
using namespace NWNXLib::Log;

static int s_failures;
static int s_checks;

#define CHECK(cond, ...) \
    do { s_checks++; if (!(cond)) { s_failures++; std::fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond); \
         std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

static std::string Hex(std::string_view s)
{
    std::string out;
    char buf[8];
    for (unsigned char c : s)
    {
        std::snprintf(buf, sizeof(buf), c >= 0x20 && c < 0x7f ? "%c" : "\\x%02x", c);
        out += buf;
    }
    return out;
}

static bool IsValidUtf8(std::string_view s)
{
    // Deliberately written differently from the encoder: decode, then check the code point and its length.
    for (size_t i = 0; i < s.size();)
    {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size())
            return false;
        uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1f : n == 3 ? c & 0x0f : c & 0x07;
        for (size_t k = 1; k < n; k++)
        {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        static const uint32_t minimum[] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (cp < minimum[n] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
        i += n;
    }
    return true;
}

// Encodes s and parses it back. Returns the decoded string, or fails the test.
static std::string RoundTrip(std::string_view s, const Json::StringOptions& options, bool* complete = nullptr)
{
    std::string out;
    const bool done = Json::AppendString(out, s, options);
    if (complete)
        *complete = done;
    CHECK(IsValidUtf8(out), "invalid UTF-8 for input %s: %s", Hex(s).c_str(), Hex(out).c_str());
    try
    {
        return nlohmann::json::parse(out).get<std::string>();
    }
    catch (const std::exception& e)
    {
        CHECK(false, "input %s gave %s: %s", Hex(s).c_str(), Hex(out).c_str(), e.what());
        return "";
    }
}

static std::string Utf8(uint32_t cp)
{
    std::string out;
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
    else
    {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
    return out;
}

// glibc's conversion of one byte, or the C1 code point for a byte the code page leaves undefined.
static std::string IconvByte(const char* codepage, unsigned char byte)
{
    iconv_t cd = iconv_open("UTF-8", codepage);
    if (cd == (iconv_t)-1)
    {
        CHECK(false, "iconv_open %s", codepage);
        return "";
    }
    char in = static_cast<char>(byte);
    char out[8];
    char* inPtr = &in;
    char* outPtr = out;
    size_t inLeft = 1, outLeft = sizeof(out);
    const size_t result = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
    iconv_close(cd);
    if (result == (size_t)-1)
        return Utf8(byte);
    return std::string(out, sizeof(out) - outLeft);
}

static std::string Escaped(unsigned char c)
{
    switch (c)
    {
        case '"':  return "\\\"";
        case '\\': return "\\\\";
        case '\n': return "\\n";
        case '\r': return "\\r";
        case '\t': return "\\t";
        case '\b': return "\\b";
        case '\f': return "\\f";
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
    return buf;
}

static void TestEveryByte()
{
    const struct { String::Locale locale; const char* iconv; } locales[] =
    {
        { String::cp1250, "CP1250" }, { String::cp1251, "CP1251" }, { String::cp1252, "CP1252" }
    };
    const Json::Encoding encodings[] = { Json::Encoding::Auto, Json::Encoding::Locale, Json::Encoding::Utf8 };

    for (const auto& locale : locales)
    {
        for (auto encoding : encodings)
        {
            const Json::StringOptions options = { encoding, locale.locale, false, SIZE_MAX };
            for (int b = 0; b < 256; b++)
            {
                const auto byte = static_cast<unsigned char>(b);
                const std::string input = std::string("a") + static_cast<char>(byte) + "z";
                std::string expected;
                if (byte < 0x80)
                    expected = input;
                else if (encoding == Json::Encoding::Utf8)
                    expected = "a\xef\xbf\xbdz";
                else
                    expected = "a" + IconvByte(locale.iconv, byte) + "z";

                const std::string decoded = RoundTrip(input, options);
                CHECK(decoded == expected, "%s mode %d byte 0x%02x: got %s, expected %s", locale.iconv,
                      static_cast<int>(encoding), b, Hex(decoded).c_str(), Hex(expected).c_str());

                // The exact escape form of control characters, quotes and backslashes.
                std::string raw;
                Json::AppendString(raw, input, options);
                if (byte < 0x20 || byte == '"' || byte == '\\' || byte == 0x7f)
                    CHECK(raw == "\"a" + Escaped(byte) + "z\"", "byte 0x%02x escaped as %s", b, raw.c_str());
                else if (byte < 0x80)
                    CHECK(raw == "\"" + input + "\"", "byte 0x%02x should pass through: %s", b, Hex(raw).c_str());
            }
        }
    }
}

static void TestUtf8()
{
    const Json::StringOptions autoMode = { Json::Encoding::Auto, String::cp1252, false, SIZE_MAX };
    const Json::StringOptions utf8Mode = { Json::Encoding::Utf8, String::cp1252, false, SIZE_MAX };
    const Json::StringOptions localeMode = { Json::Encoding::Locale, String::cp1252, false, SIZE_MAX };
    const std::string fffd = "\xef\xbf\xbd";

    // Valid UTF-8, up to four bytes, passes through in auto and utf8 modes.
    for (const std::string& valid : std::initializer_list<std::string>{ "caf\xc3\xa9", "\xe2\x82\xac 5", "\xf0\x9f\x98\x80!", "\xf4\x8f\xbf\xbf", "\xef\xbb\xbf" "bom",
                                     "\xe2\x80\xa8 line separator", std::string("nul\0byte", 8) })
    {
        CHECK(RoundTrip(valid, autoMode) == valid, "auto %s", Hex(valid).c_str());
        CHECK(RoundTrip(valid, utf8Mode) == valid, "utf8 %s", Hex(valid).c_str());
    }
    // Locale mode transcodes UTF-8 too, byte by byte.
    CHECK(RoundTrip("caf\xc3\xa9", localeMode) == "caf\xc3\x83\xc2\xa9", "locale mode transcodes everything");

    // Ill-formed input: one U+FFFD per maximal subpart in utf8 mode.
    const struct { std::string in; std::string utf8; } invalid[] =
    {
        { "\xc0\x80", fffd + fffd },                       // overlong NUL
        { "\xc1\xbf", fffd + fffd },                       // overlong
        { "\xe0\x80\x80", fffd + fffd + fffd },            // overlong three bytes
        { "\xe0\x9f\xbf", fffd + fffd + fffd },
        { "\xf0\x80\x80\x80", fffd + fffd + fffd + fffd }, // overlong four bytes
        { "\xed\xa0\x80", fffd + fffd + fffd },            // surrogate D800
        { "\xed\xbf\xbf", fffd + fffd + fffd },            // surrogate DFFF
        { "\xf4\x90\x80\x80", fffd + fffd + fffd + fffd }, // above U+10FFFF
        { "\xf5\x80", fffd + fffd },
        { "\xff", fffd },
        { "\x80", fffd },                                  // lone continuation
        { "\xe2\x82", fffd },                              // truncated at the end
        { "\xe2\x82" "A", fffd + "A" },                    // truncated before ASCII
        { "\xf0\x9f\x98", fffd },
        { "\xf0\x9f\x98" "\xc3\xa9", fffd + "\xc3\xa9" },
        { "a\xc3", "a" + fffd },
    };
    for (const auto& c : invalid)
    {
        CHECK(RoundTrip(c.in, utf8Mode) == c.utf8, "utf8 %s gave %s", Hex(c.in).c_str(), Hex(RoundTrip(c.in, utf8Mode)).c_str());
        // Auto falls back to the code page for the whole string.
        std::string expected;
        for (unsigned char b : c.in)
            expected += b < 0x80 ? std::string(1, static_cast<char>(b)) : IconvByte("CP1252", b);
        CHECK(RoundTrip(c.in, autoMode) == expected, "auto %s", Hex(c.in).c_str());
    }

    // One invalid byte makes auto transcode the valid sequences too.
    CHECK(RoundTrip("\xc3\xa9\xe9", autoMode) == "\xc3\x83\xc2\xa9\xc3\xa9", "mixed");
}

static void TestTruncation()
{
    Json::StringOptions options = { Json::Encoding::Auto, String::cp1252, false, SIZE_MAX };
    bool complete;

    for (size_t limit = 0; limit < 24; limit++)
    {
        options.maxBytes = limit;
        // Two-byte characters: never split.
        const std::string accents = "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
        std::string decoded = RoundTrip(accents, options, &complete);
        CHECK(decoded.size() == std::min<size_t>(limit / 2 * 2, accents.size()), "accents limit %zu: %zu", limit, decoded.size());
        CHECK(complete == (limit >= accents.size()), "accents limit %zu complete", limit);

        // Four-byte characters.
        const std::string emoji = "\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80";
        decoded = RoundTrip(emoji, options, &complete);
        CHECK(decoded.size() == std::min<size_t>(limit / 4 * 4, emoji.size()), "emoji limit %zu: %zu", limit, decoded.size());

        // Code page bytes are cut after transcoding: 0x80 becomes three bytes, 0xe9 two.
        decoded = RoundTrip("\x80\xe9\x80\xe9", options, &complete);
        const size_t sizes[] = { 0, 3, 5, 8, 10 };
        size_t expected = 0;
        for (size_t s : sizes)
            if (s <= limit)
                expected = s;
        CHECK(decoded.size() == expected, "cp1252 limit %zu: %zu", limit, decoded.size());

        // Escapes are never split either; the limit counts encoded bytes.
        std::string raw;
        complete = Json::AppendString(raw, "a\nb\x01" "c", options);
        const size_t encodedSizes[] = { 0, 1, 3, 4, 10, 11 };
        expected = 0;
        for (size_t s : encodedSizes)
            if (s <= limit)
                expected = s;
        CHECK(raw.size() == expected + 2, "escape limit %zu: %s", limit, raw.c_str());
        CHECK(complete == (limit >= 11), "escape limit %zu complete", limit);
    }
}

static void TestColors()
{
    std::string out;
    const struct { std::string in; std::string out; } cases[] =
    {
        { "<c\xfe\x01\x01>red</c> plain", "red plain" },
        { "<c\x10\x20\x30><cabc>two</c></c>", "two" },
        { "a<count AND b>3", "a<count AND b>3" },
        { "<c>", "<c>" },
        { "<cab", "<cab" },
        { "x</c", "x</c" },
        { "</c>", "" },
        { "1 < 2 > 0", "1 < 2 > 0" },
    };
    for (const auto& c : cases)
    {
        out = "stale";
        const bool found = Json::StripColors(c.in, out);
        CHECK((found ? out : c.in) == c.out, "strip %s gave %s", Hex(c.in).c_str(), Hex(found ? out : c.in).c_str());
        if (!found)
            CHECK(out == "stale", "out is untouched when there are no tokens");
    }

    Json::StringOptions options = { Json::Encoding::Auto, String::cp1252, true, SIZE_MAX };
    // The colour bytes don't count against UTF-8 validity once stripped.
    CHECK(RoundTrip("<c\xff\x80\x80>caf\xc3\xa9</c>", options) == "caf\xc3\xa9", "strip then validate");
    options.stripColors = false;
    CHECK(RoundTrip("<c\xff\x80\x80>x</c>", options) == "<c\xc3\xbf\xe2\x82\xac\xe2\x82\xac>x</c>", "keep colours");
}

static void TestNumbers()
{
    std::string out;
    for (double value : { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity() })
    {
        out.clear();
        Json::AppendDouble(out, value);
        CHECK(out == "null", "%f gave %s", value, out.c_str());
    }

    const struct { double value; const char* text; } exact[] =
    {
        { 0.0, "0" }, { -0.0, "-0" }, { 0.1, "0.1" }, { 3.0, "3" }, { 1e300, "1e+300" }, { 5e-324, "5e-324" },
        { 0.84, "0.84" }, { 123456.789, "123456.789" },
    };
    for (const auto& e : exact)
    {
        out.clear();
        Json::AppendDouble(out, e.value);
        CHECK(out == e.text, "%g gave %s", e.value, out.c_str());
    }

    std::mt19937_64 rng(1234);
    for (int i = 0; i < 10000; i++)
    {
        uint64_t bits = rng();
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value))
            continue;
        out.clear();
        Json::AppendDouble(out, value);
        const double back = nlohmann::json::parse(out).get<double>();
        CHECK(std::memcmp(&back, &value, sizeof(value)) == 0, "round trip %s", out.c_str());
    }

    out.clear();
    Json::AppendInt(out, std::numeric_limits<int64_t>::min());
    out += ',';
    Json::AppendUInt(out, std::numeric_limits<uint64_t>::max());
    CHECK(out == "-9223372036854775808,18446744073709551615", "%s", out.c_str());
}

static void TestKeys()
{
    for (const char* key : { "time", "level", "lvl", "msg", "src", "plugin", "file", "line", "cat", "logger", "err", "truncated" })
    {
        std::string out;
        CHECK(Json::AppendKey(out, key), "%s is reserved", key);
        CHECK(out == std::string(",\"_") + key + "\":", "%s", out.c_str());
    }
    for (const char* key : { "script", "oid", "tag", "area", "query", "levels", "" })
    {
        std::string out;
        CHECK(!Json::AppendKey(out, key), "%s is not reserved", key);
        CHECK(out == std::string(",\"") + key + "\":", "%s", out.c_str());
    }

    std::vector<Json::Member> members;
    std::vector<std::string> renamed;
    const std::string error = Json::EncodeObjectMembers(
        R"({"server":"main","level":1,"n":{"msg":[1,-2,2.5,null,true,"é\n"]},"big":18446744073709551615})", members, renamed);
    CHECK(error.empty(), "%s", error.c_str());
    std::string joined;
    for (const auto& member : members)
        joined += member.key + "=" + member.encoded + ";";
    CHECK(joined == R"(big=,"big":18446744073709551615;_level=,"_level":1;n=,"n":{"msg":[1,-2,2.5,null,true,"é\n"]};server=,"server":"main";)",
          "%s", joined.c_str());
    CHECK(renamed.size() == 1 && renamed[0] == "level", "renamed");

    members.clear();
    CHECK(!Json::EncodeObjectMembers("[1]", members, renamed).empty(), "an array is refused");
    CHECK(!Json::EncodeObjectMembers("{bad", members, renamed).empty(), "bad JSON is refused");
    CHECK(!Json::EncodeObjectMembers("{\"a\":\"\xff\"}", members, renamed).empty(), "invalid UTF-8 is refused");
    CHECK(members.empty(), "nothing is added on error");
}

int main()
{
    TestEveryByte();
    TestUtf8();
    TestTruncation();
    TestColors();
    TestNumbers();
    TestKeys();
    std::printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
