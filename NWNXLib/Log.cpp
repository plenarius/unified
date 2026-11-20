#include "nwnx.hpp"
#include "API/Globals.hpp"
#include "API/CExoBase.hpp"

#include <charconv>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "External/rang/rang.hpp"

namespace NWNXLib::Log {

static bool s_PrintTimestamp;
static bool s_PrintDate;
static bool s_PrintPlugin;
static bool s_PrintSource;
static bool s_ColorOutput;
static bool s_ForceColor;
static std::string s_LogFile;

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
void SetLogFile(const std::string& logfilepath)
{
    s_LogFile = logfilepath;
}

static void WriteLine(Channel::Enum channel, const char* message)
{
    INSTR_MESSAGE(message, strlen(message));

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

    // Also write to a file if configured
    WriteToLogFile(message);

    if (channel == Channel::SEV_FATAL)
    {
        ASSERT_FAIL();
        std::abort();
    }
}

void InternalTrace(Channel::Enum channel, Channel::Enum allowedChannel, const char* message)
{
    if (channel > allowedChannel)
    {
        // No need to do anything. Our log level is lower.
        return;
    }

    WriteLine(channel, message);
}

void WriteToLogFile(const char* message)
{
    static FILE* logFile;

    if (!logFile && s_LogFile != "")
    {
        logFile = std::fopen(s_LogFile.c_str(), "a+");
        if (logFile)
        {
            std::fprintf(logFile,
        "=====================================================================\n"
        "       NWNX secondary log file. This log file may be incomplete!     \n"
        " Please attach stdout output instead of this file to any bug reports.\n"
        "=====================================================================\n");
        }
    }
    if (logFile)
    {
        std::fprintf(logFile, "%s\n", message);
        std::fflush(logFile);
    }
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

struct Detail::LineFormatter::Buffer
{
    std::string message;
    std::string line;
    StringSink sink{message};
    std::ostream stream{&sink};
    bool inUse = false;
};

Detail::LineFormatter::LineFormatter()
{
    thread_local Buffer t_buffer;
    m_nested = t_buffer.inUse;
    m_buffer = m_nested ? new Buffer() : &t_buffer;
    m_buffer->inUse = true;
    m_buffer->message.clear();

    // Same state as the fresh std::ostringstream each line used to get.
    m_stream = &m_buffer->stream;
    m_stream->clear();
    m_stream->flags(std::ios_base::skipws | std::ios_base::dec);
    m_stream->precision(6);
    m_stream->width(0);
    m_stream->fill(' ');
}

Detail::LineFormatter::~LineFormatter()
{
    if (m_nested)
    {
        delete m_buffer;
        return;
    }

    m_buffer->inUse = false;
    // Don't let one huge line pin its memory for the life of the thread.
    constexpr size_t maxKeptCapacity = 64 * 1024;
    if (m_buffer->message.capacity() > maxKeptCapacity)
        std::string().swap(m_buffer->message);
    if (m_buffer->line.capacity() > maxKeptCapacity)
        std::string().swap(m_buffer->line);
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
        out += plugin;
        out += "] ";
    }
    if (s_PrintSource)
    {
        // Get filename without the full path.
        const char* slash = std::strrchr(file, '/');
        out += '[';
        out += slash ? slash + 1 : file;
        out += ':';
        out.append(buffer, std::to_chars(buffer, buffer + sizeof(buffer), line).ptr);
        out += "] ";
    }
}

void Detail::LineFormatter::Write(Channel::Enum channel, const char* plugin, const char* file, int line)
{
    const std::string& message = m_buffer->message;

    static const std::atomic<bool>& s_hasSubscribers = MessageBus::GetSubscribedFlag("NWNX_CORE_LOG_MESSAGE");
    if (s_hasSubscribers.load(std::memory_order_relaxed))
    {
        MessageBus::Broadcast("NWNX_CORE_LOG_MESSAGE", {
            std::to_string((int) channel),
            plugin,
            file,
            std::to_string(line),
            message
        });
    }

    std::string& out = m_buffer->line;
    out.clear();
    AppendPrefix(out, channel, plugin, file, line);
    out += message;

    WriteLine(channel, out.c_str());
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
