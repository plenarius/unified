#include "nwnx.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace NWNXLib::Log {

namespace {

using LoggerLevels = std::vector<std::pair<std::string, uint32_t>>;

// Parsed once, on first use; usually empty.
const LoggerLevels& Levels()
{
    static const LoggerLevels s_levels = []
    {
        LoggerLevels levels;
        if (auto value = Config::GetRaw("LOGGER_LEVELS", "NWNX_LOG"))
        {
            for (const auto& entry : String::Split(*value, ','))
            {
                const auto colon = entry.find(':');
                auto name = entry.substr(0, colon);
                auto number = colon == std::string::npos ? std::string() : entry.substr(colon + 1);
                const auto level = String::FromString<uint32_t>(String::Trim(number));
                if (!level)
                {
                    LOG_WARNING("Ignoring '%s' in NWNX_LOG_LOGGER_LEVELS: expected logger:level.", entry);
                    continue;
                }
                levels.emplace_back(String::Trim(name), *level);
            }
        }
        return levels;
    }();
    return s_levels;
}

}

bool HasLoggerLevels()
{
    return !Levels().empty();
}

std::optional<uint32_t> GetLoggerLevel(std::string_view logger)
{
    const LoggerLevels& levels = Levels();
    auto it = std::find_if(levels.begin(), levels.end(), [&](const auto& l) { return l.first == logger; });
    return it != levels.end() ? std::optional<uint32_t>(it->second) : std::nullopt;
}

}
