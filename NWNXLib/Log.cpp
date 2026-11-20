#include "nwnx.hpp"
#include "API/Globals.hpp"
#include "API/CExoBase.hpp"

#include "Log/Json.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "External/rang/rang.hpp"

namespace NWNXLib::Log {

static bool s_PrintTimestamp;
static bool s_PrintDate;
static bool s_PrintPlugin;
static bool s_PrintSource;
static bool s_ColorOutput;
static bool s_ForceColor;

void SetPrintTimestamp(bool value)
{
    s_PrintTimestamp = value;
}
bool GetPrintTimestamp()
{
    return s_PrintTimestamp;
}
void SetPrintDate(bool value)
{
    s_PrintDate = value;
}
bool GetPrintDate()
{
    return s_PrintDate;
}
void SetPrintPlugin(bool value)
{
    s_PrintPlugin = value;
}
bool GetPrintPlugin()
{
    return s_PrintPlugin;
}
void SetPrintSource(bool value)
{
    s_PrintSource = value;
}
bool GetPrintSource()
{
    return s_PrintSource;
}
void SetColorOutput(bool value)
{
    s_ColorOutput = value;
    rang::setControlMode(s_ColorOutput ? s_ForceColor ? rang::control::Force : rang::control::Auto : rang::control::Off);
}
bool GetColorOutput()
{
    return s_ColorOutput;
}
void SetForceColor(bool value)
{
    s_ForceColor = value;
    rang::setControlMode(s_ColorOutput ? s_ForceColor ? rang::control::Force : rang::control::Auto : rang::control::Off);
}
bool GetForceColor()
{
    return s_ForceColor;
}
// Function-local, so they are constructed before NWNXCore's constructor configures them.
static std::string& LogFilePath()
{
    static std::string s_path;
    return s_path;
}
static std::vector<Json::Member>& JsonStatic()
{
    static std::vector<Json::Member> s_members;
    return s_members;
}

static Format s_Format = Format::Text;
static Format s_FileFormat = Format::Text;
static Json::Options s_JsonOptions;
static std::mutex s_SinkMutex;
static FILE* s_File;

void SetLogFile(const std::string& logfilepath)
{
    LogFilePath() = logfilepath;
}

void SetFormat(Format format)
{
    s_Format = format;
}
Format GetFormat()
{
    return s_Format;
}
void SetFileFormat(Format format)
{
    s_FileFormat = format;
}
Format GetFileFormat()
{
    return s_FileFormat;
}
void SetJsonOptions(const Json::Options& options)
{
    s_JsonOptions = options;
}
const Json::Options& GetJsonOptions()
{
    return s_JsonOptions;
}

std::string SetJsonStatic(const std::string& object, std::vector<std::string>& renamed)
{
    std::vector<Json::Member> members;
    std::string error = Json::EncodeObjectMembers(object, members, renamed);
    if (error.empty())
        JsonStatic() = std::move(members);
    return error;
}

namespace {

enum Sink : uint8_t
{
    StdoutText = 1,
    StdoutJson = 2,
    FileText = 4,
    FileJson = 8,
    JsonSinks = StdoutJson | FileJson,
};

uint8_t ActiveSinks()
{
    uint8_t sinks = s_Format == Format::Json ? StdoutJson : StdoutText;
    if (!LogFilePath().empty())
        sinks |= s_FileFormat == Format::Json ? FileJson : FileText;
    return sinks;
}

const char* Basename(const char* file)
{
    const char* slash = std::strrchr(file, '/');
    return slash ? slash + 1 : file;
}

int64_t NowMs()
{
    timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// The parts of a record the JSON encoder needs beyond the message.
struct JsonRecord
{
    Channel::Enum channel;
    const char* plugin;
    const char* file;
    int line;
    std::string_view msg;
    std::string_view fields;            // pre-encoded ,"key":value members
    const std::vector<std::pair<const char*, bool>>* keys; // their keys and whether they were renamed
    bool fieldsTruncated;
};

// One line of JSON (without the newline) in key order: time, level, lvl, msg, src, origin, truncated, static, fields.
void EncodeJson(std::string& out, const JsonRecord& record)
{
    static constexpr int s_levels[] = { 60, 60, 60, 50, 40, 35, 30, 20 };
    static constexpr std::string_view s_labels[] = { "fatal", "fatal", "fatal", "error", "warn", "notice", "info", "debug" };
    const size_t index = std::min<size_t>(static_cast<size_t>(record.channel), 7);
    const Json::Options& options = s_JsonOptions;
    constexpr std::string_view truncatedKey = ",\"truncated\":true";
    const Json::StringOptions plain = { Json::Encoding::Utf8, String::cp1252, false, SIZE_MAX };
    const int64_t now = NowMs();

    size_t msgLimit = options.maxValue;
    bool withExtras = true;
    for (int pass = 0; pass < 2; pass++)
    {
        out.clear();
        out += "{\"time\":";
        Json::AppendInt(out, now);
        out += ",\"level\":";
        if (options.levelLabel)
        {
            out += '"';
            out += s_labels[index];
            out += '"';
        }
        else
        {
            Json::AppendInt(out, s_levels[index]);
            out += ",\"lvl\":\"";
            out += s_labels[index];
            out += '"';
        }
        out += ",\"msg\":";
        const size_t msgStart = out.size();
        bool truncated = !Json::AppendString(out, record.msg,
            { options.encoding, String::GetDefaultLocale(), options.stripColors, msgLimit });
        const size_t msgLength = out.size() - msgStart - 2;
        out += ",\"src\":\"nwnx\"";
        if (record.plugin)
        {
            out += ",\"plugin\":";
            Json::AppendString(out, record.plugin, plain);
        }
        if (record.file && s_PrintSource)
        {
            out += ",\"file\":";
            Json::AppendString(out, Basename(record.file), plain);
            out += ",\"line\":";
            Json::AppendInt(out, record.line);
        }
        truncated |= record.fieldsTruncated || !withExtras;
        if (truncated)
            out += truncatedKey;

        const size_t extrasStart = out.size();
        if (withExtras)
        {
            for (const auto& member : JsonStatic())
            {
                // A call field overrides a static member of the same name.
                const bool overridden = record.keys && std::any_of(record.keys->begin(), record.keys->end(),
                    [&member](const auto& key)
                    {
                        const std::string_view name = member.key;
                        return key.second ? name.size() > 1 && name[0] == '_' && name.substr(1) == key.first : name == key.first;
                    });
                if (!overridden)
                    out += member.encoded;
            }
            out += record.fields;
        }
        out += '}';

        if (pass == 1 || out.size() <= options.maxLine)
            break;

        // Too long: shorten the message, and drop the fields too if that is not enough.
        const size_t extras = out.size() - extrasStart - 1;
        const size_t fixed = out.size() - msgLength - extras - (truncated ? truncatedKey.size() : 0);
        const size_t budget = options.maxLine > fixed + truncatedKey.size() ? options.maxLine - fixed - truncatedKey.size() : 0;
        withExtras = budget >= extras;
        msgLimit = withExtras ? budget - extras : budget;
    }
}

void OpenLogFile()
{
    s_File = std::fopen(LogFilePath().c_str(), "a+");
    if (!s_File)
        return;

    if (s_FileFormat == Format::Json)
    {
        std::string banner;
        EncodeJson(banner, { Channel::SEV_INFO, nullptr, nullptr, 0, "NWNX secondary log file opened", {}, nullptr, false });
        banner += '\n';
        std::fwrite(banner.data(), 1, banner.size(), s_File);
        return;
    }
    std::fprintf(s_File,
        "=====================================================================\n"
        "       NWNX secondary log file. This log file may be incomplete!     \n"
        " Please attach stdout output instead of this file to any bug reports.\n"
        "=====================================================================\n");
}

void WriteText(Channel::Enum channel, const char* message)
{
    switch (channel)
    {
        case Channel::SEV_DEBUG:   std::cout << rang::fg::cyan << rang::style::dim;  break;
        case Channel::SEV_INFO:    std::cout << rang::fg::gray;                      break;
        case Channel::SEV_NOTICE:  /*default*/                                       break;
        case Channel::SEV_WARNING: std::cout << rang::fg::yellow;                    break;
        case Channel::SEV_ERROR:   std::cout << rang::fg::red;                       break;
        case Channel::SEV_FATAL:   std::cout << rang::fg::red << rang::style::bold;  break;
    }
    std::cout << message << rang::style::reset << rang::fg::reset  << std::endl;
}

// Writes the finished lines to the given sinks. The text line stops at an embedded NUL, as it always has;
// the JSON line ends with a newline.
void WriteSinks(Channel::Enum channel, uint8_t sinks, const char* text, const std::string& json)
{
    std::unique_lock<std::mutex> lock(s_SinkMutex, std::defer_lock);
    // A crashing thread may already hold the lock: FATAL records wait for it only so long.
    if (channel == Channel::SEV_FATAL)
    {
        for (int i = 0; i < 1000 && !lock.try_lock(); i++)
            std::this_thread::yield();
    }
    else
        lock.lock();

    if (sinks & StdoutText)
    {
        INSTR_MESSAGE(text, strlen(text));
        WriteText(channel, text);
    }
    else if (sinks & StdoutJson)
    {
        INSTR_MESSAGE(json.data(), json.size() - 1);
        std::cout.write(json.data(), static_cast<std::streamsize>(json.size()));
        std::cout.flush();
    }

    if (!(sinks & (FileText | FileJson)))
        return;
    if (!s_File)
        OpenLogFile();
    if (!s_File)
        return;
    if (sinks & FileText)
        std::fprintf(s_File, "%s\n", text);
    else
        std::fwrite(json.data(), 1, json.size(), s_File);
    std::fflush(s_File);
}

// Writes a message that has no prefix in text: InternalTrace and WriteToLogFile.
void WriteUnprefixed(Channel::Enum channel, uint8_t sinks, const char* message)
{
    thread_local std::string t_json;
    if (sinks & JsonSinks)
    {
        EncodeJson(t_json, { channel, nullptr, nullptr, 0, message, {}, nullptr, false });
        t_json += '\n';
    }
    WriteSinks(channel, sinks, message, t_json);
    if (t_json.capacity() > 64 * 1024)
        std::string().swap(t_json);
}

}

void InternalTrace(Channel::Enum channel, Channel::Enum allowedChannel, const char* message)
{
    if (channel > allowedChannel)
    {
        // No need to do anything. Our log level is lower.
        return;
    }

    WriteUnprefixed(channel, ActiveSinks(), message);
    if (channel == Channel::SEV_FATAL)
    {
        ASSERT_FAIL();
        std::abort();
    }
}

void WriteToLogFile(const char* message)
{
    if (const uint8_t sinks = ActiveSinks() & (FileText | FileJson))
        WriteUnprefixed(Channel::SEV_NOTICE, sinks, message);
}

static std::mutex s_LogLevelMutex;

std::atomic<uint32_t>& GetLogLevelSlot(const char* plugin)
{
    // Never freed: call sites keep references, and plugins log during static destruction.
    static auto* s_LogLevels = new std::unordered_map<std::string, std::atomic<uint32_t>>();
    std::lock_guard<std::mutex> lock(s_LogLevelMutex);
    return s_LogLevels->try_emplace(plugin, Channel::SEV_NOTICE).first->second;
}

Channel::Enum GetLogLevel(const char* plugin)
{
    return static_cast<Channel::Enum>(GetLogLevelSlot(plugin).load(std::memory_order_relaxed));
}

void SetLogLevel(const char* plugin, Channel::Enum logLevel)
{
    GetLogLevelSlot(plugin).store(static_cast<uint32_t>(logLevel), std::memory_order_relaxed);
}

namespace {
// Writes into a std::string, which keeps its capacity from line to line.
class StringSink final : public std::streambuf
{
public:
    explicit StringSink(std::string& str) : m_str(str) {}

protected:
    int_type overflow(int_type ch) override
    {
        if (!traits_type::eq_int_type(ch, traits_type::eof()))
            m_str.push_back(traits_type::to_char_type(ch));
        return traits_type::not_eof(ch);
    }

    std::streamsize xsputn(const char* s, std::streamsize count) override
    {
        m_str.append(s, static_cast<size_t>(count));
        return count;
    }

private:
    std::string& m_str;
};
}

struct Record::Buffers
{
    std::string message;
    std::string text;
    std::string json;
    std::string fields;
    std::vector<std::pair<const char*, bool>> keys;
    StringSink sink{message};
    std::ostream stream{&sink};
    bool fieldsTruncated = false;
    bool inUse = false;
};

Record::Record(Channel::Enum channel, const char* plugin, const char* file, int line)
    : m_plugin(plugin), m_file(file), m_line(line), m_channel(channel)
{
    thread_local Buffers t_buffers;
    m_nested = t_buffers.inUse;
    m_buffers = m_nested ? new Buffers() : &t_buffers;
    m_buffers->inUse = true;
    m_buffers->message.clear();
    m_buffers->fields.clear();
    m_buffers->keys.clear();
    m_buffers->fieldsTruncated = false;

    // Same state as the fresh std::ostringstream each line used to get.
    m_stream = &m_buffers->stream;
    m_stream->clear();
    m_stream->flags(std::ios_base::skipws | std::ios_base::dec);
    m_stream->precision(6);
    m_stream->width(0);
    m_stream->fill(' ');
}

Record::~Record()
{
    if (m_nested)
    {
        delete m_buffers;
        return;
    }

    m_buffers->inUse = false;
    // Don't let one huge line pin its memory for the life of the thread.
    constexpr size_t maxKeptCapacity = 64 * 1024;
    for (std::string* buffer : { &m_buffers->message, &m_buffers->text, &m_buffers->json, &m_buffers->fields })
    {
        if (buffer->capacity() > maxKeptCapacity)
            std::string().swap(*buffer);
    }
}

Record& Record::With(const char* key, std::string_view value)
{
    if (ActiveSinks() & JsonSinks)
    {
        const bool renamed = Json::AppendKey(m_buffers->fields, key);
        const Json::StringOptions options = { s_JsonOptions.encoding, String::GetDefaultLocale(),
                                              s_JsonOptions.stripColors, s_JsonOptions.maxValue };
        m_buffers->fieldsTruncated |= !Json::AppendString(m_buffers->fields, value, options);
        m_buffers->keys.emplace_back(key, renamed);
    }
    return *this;
}

Record& Record::With(const char* key, int64_t value)
{
    if (ActiveSinks() & JsonSinks)
    {
        const bool renamed = Json::AppendKey(m_buffers->fields, key);
        Json::AppendInt(m_buffers->fields, value);
        m_buffers->keys.emplace_back(key, renamed);
    }
    return *this;
}

Record& Record::WithStack(const char* key, std::string_view trace)
{
    if (!(ActiveSinks() & JsonSinks))
        return *this;

    // Frames are the lines indented by four spaces.
    const Json::StringOptions options = { s_JsonOptions.encoding, String::GetDefaultLocale(), false, s_JsonOptions.maxValue };
    std::string& out = m_buffers->fields;
    const bool renamed = Json::AppendKey(out, key);
    out += '[';
    bool first = true;
    for (size_t pos = 0; pos < trace.size();)
    {
        size_t end = trace.find('\n', pos);
        if (end == std::string_view::npos)
            end = trace.size();
        const std::string_view frame = trace.substr(pos, end - pos);
        if (frame.size() > 4 && frame.compare(0, 4, "    ") == 0)
        {
            if (!first)
                out += ',';
            first = false;
            m_buffers->fieldsTruncated |= !Json::AppendString(out, frame.substr(4), options);
        }
        pos = end + 1;
    }
    out += ']';
    m_buffers->keys.emplace_back(key, renamed);
    return *this;
}

static void AppendPrefix(std::string& out, Channel::Enum channel, const char* plugin, const char* file, int line)
{
    static constexpr const char * SEVERITY_NAMES[] = { "", "", "F", "E", "W", "N", "I", "D" };
    out += SEVERITY_NAMES[static_cast<size_t>(channel)];
    out += ' ';

    char buffer[96];
    if (s_PrintTimestamp)
    {
        time_t now = std::time(nullptr);
        tm timeinfo = {};
        localtime_r(&now, &timeinfo);

        int length = s_PrintDate
            ? std::snprintf(buffer, sizeof(buffer), "[%04d-%02d-%02d %02d:%02d:%02d] ", 1900 + timeinfo.tm_year,
                            1 + timeinfo.tm_mon, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec)
            : std::snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d] ", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        out.append(buffer, static_cast<size_t>(length));
    }
    if (s_PrintPlugin)
    {
        out += '[';
        out += plugin ? plugin : "";
        out += "] ";
    }
    if (s_PrintSource)
    {
        out += '[';
        out += file ? Basename(file) : "";
        out += ':';
        out.append(buffer, std::to_chars(buffer, buffer + sizeof(buffer), line).ptr);
        out += "] ";
    }
}

static void WriteRecord(Record::Buffers& buffers, Channel::Enum channel, const char* plugin, const char* file, int line, uint8_t sinks)
{
    if (sinks & (StdoutText | FileText))
    {
        buffers.text.clear();
        AppendPrefix(buffers.text, channel, plugin, file, line);
        buffers.text += buffers.message;
    }
    if (sinks & JsonSinks)
    {
        EncodeJson(buffers.json, { channel, plugin, file, line, buffers.message, buffers.fields, &buffers.keys, buffers.fieldsTruncated });
        buffers.json += '\n';
    }
    WriteSinks(channel, sinks, buffers.text.c_str(), buffers.json);
}

void Record::Write()
{
    const std::string& message = m_buffers->message;

    static const std::atomic<bool>& s_hasSubscribers = MessageBus::GetSubscribedFlag("NWNX_CORE_LOG_MESSAGE");
    if (s_hasSubscribers.load(std::memory_order_relaxed))
    {
        MessageBus::Broadcast("NWNX_CORE_LOG_MESSAGE", {
            std::to_string((int) m_channel),
            m_plugin ? m_plugin : "",
            m_file ? m_file : "",
            std::to_string(m_line),
            message
        });
    }

    WriteRecord(*m_buffers, m_channel, m_plugin, m_file, m_line, ActiveSinks());

    if (m_channel == Channel::SEV_FATAL)
    {
        ASSERT_FAIL();
        std::abort();
    }
}

void Record::WriteJson()
{
    if (const uint8_t sinks = ActiveSinks() & JsonSinks)
        WriteRecord(*m_buffers, m_channel, m_plugin, m_file, m_line, sinks);
}

}

#include "API/API/CExoString.hpp"
#include "API/API/CResRef.hpp"

std::ostream& operator<<(std::ostream& out, const CExoString& str)
{
    out << str.CStr();
    return out;
}

std::ostream& operator<<(std::ostream& out, const CResRef& str)
{
    out << str.GetResRefStr();
    return out;
}
