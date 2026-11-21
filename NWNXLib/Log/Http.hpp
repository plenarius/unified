#pragma once

// Fields for HTTP request records (WebHook, HTTPClient). Paths can hold secrets (Discord webhook tokens) and
// bodies can be large or private, so by default JSON gets the path's first segment and no body.
#include "nwnx.hpp"

#include <string>
#include <string_view>

namespace NWNXLib::Log::Http {
// Internal linkage: LOG_WARNING expands to the including plugin's name.
namespace {

enum class PathMode : uint8_t { Redacted, Full, None };

struct Options
{
    PathMode path = PathMode::Redacted;
    bool body = false;
};

// Reads NWNX_<module>_LOG_PATH (full, redacted or none) and NWNX_<module>_LOG_BODY.
inline Options ReadOptions(const std::string& module)
{
    Options options;
    const auto path = Config::Get<std::string>("LOG_PATH", module);
    if (path == "full")
        options.path = PathMode::Full;
    else if (path == "none")
        options.path = PathMode::None;
    else if (path && path != "redacted")
        LOG_WARNING("%s_LOG_PATH='%s' is not full, redacted or none; using redacted.", module, *path);
    options.body = Config::Get<bool>("LOG_BODY", false, module);
    return options;
}

// The path up to its second '/' or its query, marked as cut: "/api/webhooks/1/token?wait=true" -> "/api/...".
inline std::string RedactPath(std::string_view path)
{
    const size_t cut = path.find_first_of("/?", path.empty() || path[0] != '/' ? 0 : 1);
    return cut == std::string_view::npos ? std::string(path) : std::string(path.substr(0, cut)).append("/...");
}

// The request's fields: record.With(Log::Http::Request{options, host, path, body}).
struct Request
{
    const Options& options;
    std::string_view host;
    std::string_view path;
    std::string_view body;

    void AddTo(Record& record) const
    {
        record.With("host", host);
        if (options.path == PathMode::Full)
            record.With("path", path);
        else if (options.path == PathMode::Redacted)
            record.With("path", RedactPath(path));
        if (options.body)
            record.With("body", body);
    }
};

}

}
