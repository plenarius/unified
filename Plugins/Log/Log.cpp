#include "nwnx.hpp"
#include "Bindings.hpp"

#include "API/JsonEngineStructure.hpp"
#include "../../Core/EngineLogFiles.hpp"

#include <algorithm>
#include <utility>
#include <vector>

using namespace NWNXLib;
using namespace NWNXLib::API;
namespace Bindings = ScriptLog::Bindings;

namespace {

// Per-logger thresholds from NWNX_LOG_LOGGER_LEVELS=bank:7,craft:4. Usually empty.
std::vector<std::pair<std::string, uint32_t>> s_loggerLevels;

[[maybe_unused]] const bool s_configured = []
{
    if (auto levels = Config::Get<std::string>("LOGGER_LEVELS"))
    {
        for (const auto& entry : String::Split(*levels, ','))
        {
            const auto colon = entry.find(':');
            auto name = entry.substr(0, colon);
            auto value = colon == std::string::npos ? std::string() : entry.substr(colon + 1);
            const auto level = String::FromString<uint32_t>(String::Trim(value));
            if (!level)
            {
                LOG_WARNING("Ignoring '%s' in NWNX_LOG_LOGGER_LEVELS: expected logger:level.", entry);
                continue;
            }
            s_loggerLevels.emplace_back(String::Trim(name), *level);
        }
    }

    return true;
}();

// Scripts get DEBUG..ERROR; FATAL would abort the server.
Log::Channel::Enum ToChannel(int32_t level)
{
    return static_cast<Log::Channel::Enum>(std::clamp<int32_t>(level, Log::Channel::SEV_ERROR, Log::Channel::SEV_DEBUG));
}

bool IsLevelEnabled(Log::Channel::Enum channel, const std::string& logger)
{
    uint32_t threshold = Log::PluginLogLevel().load(std::memory_order_relaxed);
    if (!s_loggerLevels.empty())
    {
        const std::string* name = logger.empty() ? Bindings::FindLogger() : &logger;
        if (name)
        {
            auto it = std::find_if(s_loggerLevels.begin(), s_loggerLevels.end(), [&](const auto& l) { return l.first == *name; });
            if (it != s_loggerLevels.end())
                threshold = it->second;
        }
    }
    return static_cast<uint32_t>(channel) <= threshold;
}

const json& GetJson(const JsonEngineStructure& j)
{
    static const json s_null;
    return j.m_shared ? j.m_shared->m_json : s_null;
}

}

NWNX_EXPORT ArgumentStack Write(ArgumentStack&& args)
{
    const auto channel = ToChannel(args.extract<int32_t>());
    const auto message = args.extract<std::string>();
    const auto fields = args.extract<JsonEngineStructure>();
    const auto logger = args.extract<std::string>();

    if (!IsLevelEnabled(channel, logger))
        return {};

    // Text shows the fields too: a script record has no legacy line to keep. The context comes first, from Core.
    // No file/line: this plugin's own would only mislead; the script context says where it came from.
    Log::Record record(channel, PLUGIN_NAME, nullptr, 0);
    record.Src("script").TextFields();
    if (const std::string* name = logger.empty() ? Bindings::FindLogger() : &logger)
        record.Origin("logger", *name);
    Bindings::AddTo(record);
    if (const json& values = GetJson(fields); !values.is_null() && record.HasFields())
    {
        // A non-object is logged under "data", as the documentation promises.
        if (!values.is_object())
            Bindings::AddField(record, "data", values);
        else
        {
            for (auto it = values.begin(); it != values.end(); ++it)
                Bindings::AddField(record, it.key(), it.value());
        }
    }
    record.Stream().write(message.data(), static_cast<std::streamsize>(message.size()));
    record.Write();
    return {};
}

NWNX_EXPORT ArgumentStack IsEnabled(ArgumentStack&& args)
{
    const auto channel = args.extract<int32_t>();
    const auto logger = args.extract<std::string>();
    // Out-of-range levels behave like Write(), which clamps them.
    return static_cast<int32_t>(IsLevelEnabled(ToChannel(channel), logger));
}

NWNX_EXPORT ArgumentStack Bind(ArgumentStack&& args)
{
    const auto structure = args.extract<JsonEngineStructure>();
    const auto& fields = GetJson(structure);
    if (fields.is_object())
        Bindings::Bind(fields);
    else if (!fields.is_null())
        LOG_WARNING("NWNX_Log_Bind: expected a JSON object, got %s.", fields.type_name());
    return {};
}

NWNX_EXPORT ArgumentStack Unbind(ArgumentStack&& args)
{
    Bindings::Unbind(args.extract<std::string>());
    return {};
}

NWNX_EXPORT ArgumentStack GetBindings(ArgumentStack&&)
{
    return JsonEngineStructure(Bindings::Get(), CExoString(""));
}

NWNX_EXPORT ArgumentStack SetGlobal(ArgumentStack&& args)
{
    const auto structure = args.extract<JsonEngineStructure>();
    const auto& fields = GetJson(structure);
    if (!fields.is_object() && !fields.is_null())
        LOG_WARNING("NWNX_Log_SetGlobal: expected a JSON object, got %s; clearing the global bindings.", fields.type_name());
    Bindings::SetGlobal(fields);
    return {};
}

NWNX_EXPORT ArgumentStack Rotate(ArgumentStack&& args)
{
    // NWNX_LOG_ROTATE_NWNX (1) is accepted but does nothing yet: NWNX's own log file comes later.
    const auto targets = args.extract<int32_t>();
    namespace Files = Core::EngineLogFiles;
    bool ok = true;
    if (targets & 2)
        ok &= Files::Rotate(Files::File::Server).has_value();
    if (targets & 4)
        ok &= Files::Rotate(Files::File::Engine).has_value();
    return static_cast<int32_t>(ok);
}
