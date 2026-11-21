#include "NWNXLog.h"
#include "nwnx.hpp"

#include <algorithm>
#include <cstring>

using namespace NWNXLib;

namespace {

struct Bridge
{
    const char* src;
    const char* plugin;
    std::atomic<uint32_t>* level;
    bool known;
};

// The known bridges; anything else logs as src "bridge" under Core's level. Slots are looked up once.
const Bridge& FindBridge(const char* src)
{
    static const Bridge s_bridges[] = {
        {"dotnet", "NWNX_DotNET", &Log::GetLogLevelSlot("NWNX_DotNET"), true},
        {"swig", "NWNX_SWIG", &Log::GetLogLevelSlot("NWNX_SWIG"), true},
        {"ruby", "NWNX_Ruby", &Log::GetLogLevelSlot("NWNX_Ruby"), true},
        {"bridge", PLUGIN_NAME, &Log::PluginLogLevel(), false},
    };
    const auto last = std::end(s_bridges) - 1;
    if (!src)
        return *last;
    return *std::find_if(std::begin(s_bridges), last, [&](const Bridge& b) { return std::strcmp(b.src, src) == 0; });
}

// Bridges get DEBUG..ERROR; FATAL would abort the server.
Log::Channel::Enum ToChannel(int level)
{
    return static_cast<Log::Channel::Enum>(std::clamp<int>(level, Log::Channel::SEV_ERROR, Log::Channel::SEV_DEBUG));
}

bool IsEnabled(Log::Channel::Enum channel, const Bridge& bridge, const char* logger)
{
    uint32_t threshold = bridge.level->load(std::memory_order_relaxed);
    if (logger && *logger && Log::HasLoggerLevels())
        threshold = Log::GetLoggerLevel(logger).value_or(threshold);
    return static_cast<uint32_t>(channel) <= threshold;
}

// Deeper fields would only be there to exhaust the stack when they are freed or encoded.
constexpr int MaxFieldsDepth = 64;

void AddFields(Log::Record& record, const char* fieldsJson)
{
    json fields;
    bool tooDeep = false;
    try
    {
        fields = json::parse(fieldsJson, [&](int depth, json::parse_event_t event, json&)
        {
            if (depth < MaxFieldsDepth || (event != json::parse_event_t::object_start && event != json::parse_event_t::array_start))
                return true;
            tooDeep = true;
            return false;
        });
    }
    catch (const json::exception& e)
    {
        // Keep the record: a bad field list should not lose the message.
        record.With("fields_error", Log::Utf8{e.what()});
        return;
    }
    if (tooDeep)
    {
        record.With("fields_error", "fields nested deeper than 64 levels");
        return;
    }
    if (!fields.is_object())
    {
        if (!fields.is_null())
            record.With("data", fields);
        return;
    }
    for (auto it = fields.begin(); it != fields.end(); ++it)
    {
        if (it->is_string())
            record.With(it.key(), Log::Utf8{it->get_ref<const std::string&>()});
        else
            record.With(it.key(), *it);
    }
}

// Text lines are game-locale bytes: the UTF-8 message is converted only if a text sink or subscriber needs it.
struct LocaleText { const char* utf8; };
std::ostream& operator<<(std::ostream& out, const LocaleText& text)
{
    if (text.utf8)
        out << String::FromUTF8(text.utf8);
    return out;
}

class BridgeRecord final : public Log::Record
{
public:
    BridgeRecord(Log::Channel::Enum channel, const char* plugin, const char* msg)
        : Record(channel, plugin, nullptr, 0), m_text{msg}, m_list(m_text)
    {
        m_format = "%s";
        m_args = &m_list;
        // JSON keeps the UTF-8 whatever NWNX_CORE_LOG_JSON_ENCODING says.
        if (msg)
            Msg(Log::Utf8{msg});
    }

private:
    LocaleText m_text;
    tfm::detail::FormatListN<1> m_list;
};

}

extern "C" int NWNX_Log_IsEnabled(int syslogLevel, const char* src, const char* logger)
{
    try
    {
        return IsEnabled(ToChannel(syslogLevel), FindBridge(src), logger);
    }
    catch (...)
    {
        return 0;
    }
}

extern "C" void NWNX_Log_Emit(int syslogLevel, const char* src, const char* logger, const char* msg, const char* fieldsJson)
{
    try
    {
        const auto channel = ToChannel(syslogLevel);
        const Bridge& bridge = FindBridge(src);
        if (!IsEnabled(channel, bridge, logger))
            return;

        // As for script records: no file/line, and text shows the fields since there is no legacy line to keep.
        BridgeRecord record(channel, bridge.plugin, msg);
        record.Src(bridge.src).TextFields();
        if (logger && *logger)
            record.Origin("logger", logger);
        if (src && !bridge.known)
            record.With("bridge", Log::Utf8{src});
        if (fieldsJson && record.HasFields())
            AddFields(record, fieldsJson);
        record.Write();
    }
    catch (...)
    {
        // Nothing may unwind into the caller's runtime.
    }
}
