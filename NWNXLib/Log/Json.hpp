#pragma once

// JSON encoding for structured log records: strings in the game's code page or UTF-8, numbers and keys.
#include "nwnx.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace NWNXLib::Log::Json {

enum class Encoding : uint8_t
{
    Auto,   // valid UTF-8 passes through, anything else is transcoded from the game locale
    Locale, // always transcode from the game locale
    Utf8,   // always UTF-8; invalid sequences become U+FFFD
};

struct Options
{
    bool levelLabel = false; // "level":"info" instead of "level":30,"lvl":"info"
    Encoding encoding = Encoding::Auto;
    bool stripColors = true;
    uint32_t maxValue = 8 * 1024;  // encoded bytes per string value
    uint32_t maxLine = 64 * 1024;  // bytes per record
};

struct StringOptions
{
    Encoding encoding = Encoding::Auto;
    String::Locale locale = String::cp1252;
    bool stripColors = false;
    size_t maxBytes = SIZE_MAX; // encoded bytes between the quotes
};

// Appends s as a quoted JSON string. Returns false if it was cut at maxBytes (always on a character boundary).
bool AppendString(std::string& out, std::string_view s, const StringOptions& options);
void AppendInt(std::string& out, int64_t value);
void AppendUInt(std::string& out, uint64_t value);
// Shortest round-trip form; NaN and infinities become null, as JSON.stringify does.
void AppendDouble(std::string& out, double value);

bool IsReservedKey(std::string_view key);
// Appends ,"key": and renames a reserved key with a leading '_'. Returns true if it was renamed.
bool AppendKey(std::string& out, std::string_view key);

// Removes NWN colour tokens (<cXYZ> and </c>) into out. Returns false, leaving out alone, if there are none.
bool StripColors(std::string_view s, std::string& out);

struct Member
{
    std::string key;
    std::string encoded; // ,"key":value
};
// Encodes a JSON object given as text into members, renaming reserved keys (listed in renamed).
// Returns an error message, or an empty string on success.
std::string EncodeObjectMembers(const std::string& object, std::vector<Member>& out, std::vector<std::string>& renamed);

}
