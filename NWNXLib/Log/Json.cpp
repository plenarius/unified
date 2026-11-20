#include "Log/Json.hpp"
#include "External/json/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>

namespace NWNXLib::Log::Json {

namespace {

enum ByteClass : uint8_t { Plain, Escape, High };

constexpr std::array<uint8_t, 256> s_byteClass = []
{
    std::array<uint8_t, 256> table{};
    for (int c = 0; c < 256; c++)
        table[c] = c >= 0x80 ? High : (c < 0x20 || c == '"' || c == '\\' || c == 0x7f) ? Escape : Plain;
    return table;
}();

// Length of the well-formed UTF-8 sequence at p, or minus the length of its maximal ill-formed prefix
// (Unicode 3.9, "U+FFFD substitution of maximal subparts").
int Utf8Step(const uint8_t* p, const uint8_t* end)
{
    const uint8_t c = *p;
    int need;
    uint8_t lo = 0x80, hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf)
        need = 1;
    else if (c >= 0xe0 && c <= 0xef)
    {
        need = 2;
        if (c == 0xe0) lo = 0xa0;
        if (c == 0xed) hi = 0x9f; // no surrogates
    }
    else if (c >= 0xf0 && c <= 0xf4)
    {
        need = 3;
        if (c == 0xf0) lo = 0x90;
        if (c == 0xf4) hi = 0x8f; // nothing above U+10FFFF
    }
    else
        return -1;

    for (int i = 1; i <= need; i++)
    {
        if (p + i >= end || p[i] < lo || p[i] > hi)
            return -i;
        lo = 0x80;
        hi = 0xbf;
    }
    return need + 1;
}

bool IsValidUtf8(const uint8_t* p, const uint8_t* end)
{
    while (p < end)
    {
        if (*p < 0x80)
        {
            p++;
            continue;
        }
        const int n = Utf8Step(p, end);
        if (n < 0)
            return false;
        p += n;
    }
    return true;
}

// Appends string contents, refusing anything that would take them past the limit.
class ContentWriter
{
public:
    ContentWriter(std::string& out, size_t limit) : m_out(out), m_end(limit > SIZE_MAX - out.size() ? SIZE_MAX : out.size() + limit) {}

    bool Cut() const { return m_cut; }

    bool Raw(const char* p, size_t n)
    {
        if (m_out.size() + n > m_end)
            return Stop();
        m_out.append(p, n);
        return true;
    }

    // As many plain ASCII bytes as fit; they are one character each.
    bool Plain(const char* p, size_t n)
    {
        const size_t room = m_end - m_out.size();
        if (n > room)
        {
            m_out.append(p, room);
            return Stop();
        }
        m_out.append(p, n);
        return true;
    }

    bool Escape(uint8_t c)
    {
        char buf[6] = {'\\', 0, 0, 0, 0, 0};
        switch (c)
        {
            case '"':  buf[1] = '"';  return Raw(buf, 2);
            case '\\': buf[1] = '\\'; return Raw(buf, 2);
            case '\n': buf[1] = 'n';  return Raw(buf, 2);
            case '\r': buf[1] = 'r';  return Raw(buf, 2);
            case '\t': buf[1] = 't';  return Raw(buf, 2);
            case '\b': buf[1] = 'b';  return Raw(buf, 2);
            case '\f': buf[1] = 'f';  return Raw(buf, 2);
            default:
                static constexpr char hex[] = "0123456789abcdef";
                buf[1] = 'u';
                buf[2] = '0';
                buf[3] = '0';
                buf[4] = hex[c >> 4];
                buf[5] = hex[c & 0xf];
                return Raw(buf, 6);
        }
    }

private:
    bool Stop()
    {
        m_cut = true;
        return false;
    }

    std::string& m_out;
    size_t m_end;
    bool m_cut = false;
};

// The string between the quotes. Returns false if it was cut.
bool AppendContents(std::string& out, std::string_view s, const StringOptions& options)
{
    thread_local std::string t_stripped;
    if (options.stripColors && std::memchr(s.data(), '<', s.size()) && StripColors(s, t_stripped))
    {
        const bool complete = AppendContents(out, t_stripped, { options.encoding, options.locale, false, options.maxBytes });
        // Don't let one huge value pin its memory for the life of the thread.
        if (t_stripped.capacity() > 64 * 1024)
            std::string().swap(t_stripped);
        return complete;
    }

    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    const auto* end = p + s.size();
    const bool transcode = options.encoding == Encoding::Locale ||
                           (options.encoding == Encoding::Auto && !IsValidUtf8(p, end));
    const auto& codepage = String::CodepageToUTF8(options.locale);

    ContentWriter writer(out, options.maxBytes);
    while (p < end)
    {
        const auto* run = p;
        while (p < end && s_byteClass[*p] == Plain)
            p++;
        if (p != run && !writer.Plain(reinterpret_cast<const char*>(run), static_cast<size_t>(p - run)))
            break;
        if (p == end)
            break;

        bool ok;
        if (s_byteClass[*p] == Escape)
            ok = writer.Escape(*p++);
        else if (transcode)
        {
            const auto& utf8 = codepage[*p++ - 0x80];
            ok = writer.Raw(reinterpret_cast<const char*>(utf8.bytes), utf8.length);
        }
        else
        {
            const int n = Utf8Step(p, end);
            if (n > 0)
            {
                ok = writer.Raw(reinterpret_cast<const char*>(p), static_cast<size_t>(n));
                p += n;
            }
            else
            {
                ok = writer.Raw("\xef\xbf\xbd", 3); // U+FFFD
                p += -n;
            }
        }
        if (!ok)
            break;
    }
    return !writer.Cut();
}

constexpr StringOptions s_utf8Options = { Encoding::Utf8, String::cp1252, false, SIZE_MAX };

void AppendValue(std::string& out, const nlohmann::json& value)
{
    switch (value.type())
    {
        case nlohmann::json::value_t::boolean:
            out += value.get<bool>() ? "true" : "false";
            break;
        case nlohmann::json::value_t::number_integer:
            AppendInt(out, value.get<int64_t>());
            break;
        case nlohmann::json::value_t::number_unsigned:
            AppendUInt(out, value.get<uint64_t>());
            break;
        case nlohmann::json::value_t::number_float:
            AppendDouble(out, value.get<double>());
            break;
        case nlohmann::json::value_t::string:
            AppendString(out, value.get_ref<const std::string&>(), s_utf8Options);
            break;
        case nlohmann::json::value_t::array:
        {
            out += '[';
            bool first = true;
            for (const auto& element : value)
            {
                if (!first)
                    out += ',';
                first = false;
                AppendValue(out, element);
            }
            out += ']';
            break;
        }
        case nlohmann::json::value_t::object:
        {
            out += '{';
            bool first = true;
            for (const auto& item : value.items())
            {
                if (!first)
                    out += ',';
                first = false;
                AppendString(out, item.key(), s_utf8Options);
                out += ':';
                AppendValue(out, item.value());
            }
            out += '}';
            break;
        }
        default:
            out += "null";
            break;
    }
}

}

bool AppendString(std::string& out, std::string_view s, const StringOptions& options)
{
    out += '"';
    const bool complete = AppendContents(out, s, options);
    out += '"';
    return complete;
}

void AppendInt(std::string& out, int64_t value)
{
    char buf[24];
    out.append(buf, std::to_chars(buf, buf + sizeof(buf), value).ptr);
}

void AppendUInt(std::string& out, uint64_t value)
{
    char buf[24];
    out.append(buf, std::to_chars(buf, buf + sizeof(buf), value).ptr);
}

void AppendDouble(std::string& out, double value)
{
    if (!std::isfinite(value))
    {
        out += "null";
        return;
    }
    char buf[32];
    out.append(buf, std::to_chars(buf, buf + sizeof(buf), value).ptr);
}

bool IsReservedKey(std::string_view key)
{
    static constexpr std::string_view reserved[] =
    {
        "cat", "err", "file", "level", "line", "logger", "lvl", "msg", "plugin", "src", "time", "truncated"
    };
    return std::binary_search(std::begin(reserved), std::end(reserved), key);
}

bool AppendKey(std::string& out, std::string_view key)
{
    const bool reserved = IsReservedKey(key);
    out += ",\"";
    if (reserved)
        out += '_';
    AppendContents(out, key, s_utf8Options);
    out += "\":";
    return reserved;
}

bool StripColors(std::string_view s, std::string& out)
{
    // The engine's tokens: "<c" with three colour bytes and ">", and "</c>".
    auto tokenLength = [&s](size_t i) -> size_t
    {
        if (s.size() - i >= 6 && s[i + 1] == 'c' && s[i + 5] == '>')
            return 6;
        if (s.compare(i, 4, "</c>") == 0)
            return 4;
        return 0;
    };

    size_t copied = 0;
    bool found = false;
    for (size_t i = s.find('<'); i != std::string_view::npos; i = s.find('<', i))
    {
        const size_t length = tokenLength(i);
        if (!length)
        {
            i++;
            continue;
        }
        if (!found)
            out.clear();
        found = true;
        out.append(s.data() + copied, i - copied);
        i += length;
        copied = i;
    }
    if (found)
        out.append(s.data() + copied, s.size() - copied);
    return found;
}

std::string EncodeObjectMembers(const std::string& object, std::vector<Member>& out, std::vector<std::string>& renamed)
{
    nlohmann::json parsed;
    try
    {
        parsed = nlohmann::json::parse(object);
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
    if (!parsed.is_object())
        return "not a JSON object";

    for (const auto& item : parsed.items())
    {
        Member member;
        member.key = item.key();
        if (AppendKey(member.encoded, member.key))
        {
            renamed.push_back(member.key);
            member.key.insert(0, 1, '_');
        }
        AppendValue(member.encoded, item.value());
        out.push_back(std::move(member));
    }
    return "";
}

}
