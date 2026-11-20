#include "nwnx.hpp"
#include "API/Globals.hpp"
#include "API/CExoBase.hpp"
#include "API/API/CExoString.hpp"
#include "API/API/CResRef.hpp"

#include "Log/Json.hpp"
#include "Log/Rotation.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <condition_variable>
#include <deque>
#include <csignal>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

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
static bool s_TextFields;
static std::atomic<ContextSource> s_ContextSource;
static Format s_FileFormat = Format::Text;
static Json::Options s_JsonOptions;
static std::mutex s_SinkMutex;

const std::string& GetLogFile()
{
    return LogFilePath();
}

void SetFormat(Format format)
{
    s_Format = format;
}
Format GetFormat()
{
    return s_Format;
}
void SetTextFields(bool value)
{
    s_TextFields = value;
}
bool GetTextFields()
{
    return s_TextFields;
}
void SetContextSource(ContextSource source)
{
    s_ContextSource.store(source, std::memory_order_relaxed);
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
    TextSinks = StdoutText | FileText,
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

// A field's place in Record::Buffers: its final key (renamed if reserved) in the key arena, and where its
// ,"key":value member starts in the fields buffer. A member ends where the next one starts.
struct FieldIndex
{
    uint32_t keyOffset;
    uint32_t keyLength;
    uint32_t start;
    uint32_t valueStart;
    uint32_t rawStart = UINT32_MAX; // a string's text-mode bytes in Buffers::raw, if kept
    uint32_t rawLength = 0;
};

// The parts of a record the JSON encoder needs beyond the message.
struct JsonRecord
{
    Channel::Enum channel;
    const char* plugin;
    const char* file;
    int line;
    std::string_view msg;
    std::string_view fields;            // pre-encoded ,"key":value members
    const std::vector<FieldIndex>* index; // their keys, so static members can be overridden
    std::string_view keys;              // the key arena FieldIndex points into
    bool fieldsTruncated;
    const char* src = "nwnx";
    std::string_view origin = {};       // pre-encoded ,"key":value origin members
    bool msgUtf8 = false;               // msg is UTF-8 whatever the JSON encoding option says
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
            { record.msgUtf8 ? Json::Encoding::Utf8 : options.encoding, String::GetDefaultLocale(), options.stripColors, msgLimit });
        const size_t msgLength = out.size() - msgStart - 2;
        out += ",\"src\":";
        Json::AppendString(out, record.src, plain);
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
        out += record.origin;
        truncated |= record.fieldsTruncated || !withExtras;
        if (truncated)
            out += truncatedKey;

        const size_t extrasStart = out.size();
        if (withExtras)
        {
            for (const auto& member : JsonStatic())
            {
                // A call field overrides a static member of the same name.
                const bool overridden = record.index && std::any_of(record.index->begin(), record.index->end(),
                    [&](const FieldIndex& field)
                    {
                        return record.keys.substr(field.keyOffset, field.keyLength) == member.key;
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

// Writes all of data, waiting out EINTR and a non-blocking fd that is full. False on any other error.
bool WriteAll(int fd, const char* data, size_t size)
{
    while (size)
    {
        const ssize_t written = ::write(fd, data, size);
        if (written > 0)
        {
            data += written;
            size -= static_cast<size_t>(written);
        }
        else if (written < 0 && errno == EAGAIN)
        {
            pollfd pfd = { fd, POLLOUT, 0 };
            ::poll(&pfd, 1, -1);
        }
        else if (written == 0 || errno != EINTR)
            return false;
    }
    return true;
}

// Writes whole lines. On a pipe or socket each write(2) is at most PIPE_BUF bytes and ends on a newline, so
// another writer of the same pipe can never land inside one of our lines; a longer line is written alone.
void WriteLines(int fd, bool chunked, std::string_view data)
{
    while (!data.empty())
    {
        size_t size = data.size();
        if (chunked && size > PIPE_BUF)
        {
            size = data.rfind('\n', PIPE_BUF - 1);
            if (size == std::string_view::npos)
                size = std::min(data.find('\n'), data.size() - 1);
            size++;
        }
        if (!WriteAll(fd, data.data(), size))
            return;
        data.remove_prefix(size);
    }
}

bool IsPipe(int fd)
{
    struct stat st = {};
    return ::fstat(fd, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
}

// The file's fd belongs to whoever holds s_SinkMutex while the writer is not writing it (AsyncWriter::busyFile).
int s_FileFd = -1;
bool s_FileChunked;
FileRotation s_FileRotation;
std::atomic<off_t> s_FileWritten; // record bytes in the current file, for the triggers
std::atomic<bool> s_RotateDue;
std::atomic<bool> s_ReopenDue;
std::mutex s_RotateMutex;

// Opens the file's path and writes its banner, naming the archive it continues from after a rotation.
// The old fd is closed only once the new one is open.
bool OpenLogFile(const char* rotatedFrom = nullptr)
{
    const int fd = ::open(LogFilePath().c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0)
        return false;
    if (s_FileFd >= 0)
        ::close(s_FileFd);
    s_FileFd = fd;
    s_FileChunked = IsPipe(s_FileFd);
    s_FileWritten.store(0, std::memory_order_relaxed);

    if (s_FileFormat == Format::Json)
    {
        std::string fields, banner;
        if (rotatedFrom)
        {
            fields = ",\"rotated_from\":";
            Json::AppendString(fields, Basename(rotatedFrom), { Json::Encoding::Utf8, String::cp1252, false, SIZE_MAX });
        }
        EncodeJson(banner, { Channel::SEV_INFO, nullptr, nullptr, 0, "NWNX secondary log file opened", fields, nullptr, {}, false });
        banner += '\n';
        WriteLines(s_FileFd, s_FileChunked, banner);
        return true;
    }
    static constexpr std::string_view s_rule = "=====================================================================\n";
    std::string banner;
    banner += s_rule;
    banner += "       NWNX secondary log file. This log file may be incomplete!     \n"
              " Please attach stdout output instead of this file to any bug reports.\n";
    if (rotatedFrom)
    {
        banner += " Rotated from: ";
        banner += Basename(rotatedFrom);
        banner += '\n';
    }
    banner += s_rule;
    WriteLines(s_FileFd, s_FileChunked, banner);
    return true;
}

void WriteFile(std::string_view lines)
{
    if (s_FileFd < 0 && !OpenLogFile())
        return;
    WriteLines(s_FileFd, s_FileChunked, lines);
    const off_t written = s_FileWritten.load(std::memory_order_relaxed) + static_cast<off_t>(lines.size());
    s_FileWritten.store(written, std::memory_order_relaxed);
    if (s_FileRotation.size && written >= s_FileRotation.size)
        s_RotateDue.store(true, std::memory_order_relaxed);
}

void WriteText(Channel::Enum channel, std::string_view message)
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

// The bytes WriteText would send: rang's codes when it would colour std::cout.
struct TextColors
{
    std::string_view on;
    std::string_view off;
};
TextColors GetTextColors(Channel::Enum channel)
{
    const rang::control mode = rang::rang_implementation::controlMode();
    if (mode != rang::control::Force && (mode != rang::control::Auto || !rang::rang_implementation::supportsColor() ||
                                          !rang::rang_implementation::isTerminal(std::cout.rdbuf())))
        return {};
    constexpr std::string_view off = "\033[0m\033[39m";
    switch (channel)
    {
        case Channel::SEV_DEBUG:   return { "\033[36m\033[2m", off };
        case Channel::SEV_INFO:    return { "\033[37m", off };
        case Channel::SEV_WARNING: return { "\033[33m", off };
        case Channel::SEV_ERROR:   return { "\033[31m", off };
        case Channel::SEV_FATAL:   return { "\033[31m\033[1m", off };
        default:                   return { "", off };
    }
}

// Lines per sink, each with its newline, for writes on the calling thread.
struct Batch
{
    std::string out;
    std::string file;

    void Clear()
    {
        // Keep the capacity for the next batch, unless a burst made it large.
        for (std::string* lines : { &out, &file })
        {
            if (lines->capacity() > 1024 * 1024)
                std::string().swap(*lines);
            else
                lines->clear();
        }
    }
};

// What one record adds to a batch.
struct Lines
{
    uint8_t sinks;
    std::string_view text; // without newline
    std::string_view json; // with newline
    TextColors colors;

    size_t OutSize() const
    {
        if (sinks & StdoutText)
            return colors.on.size() + text.size() + colors.off.size() + 1;
        return sinks & StdoutJson ? json.size() : 0;
    }
    size_t FileSize() const
    {
        if (sinks & FileText)
            return text.size() + 1;
        return sinks & FileJson ? json.size() : 0;
    }

    void AppendOut(std::string& out) const
    {
        if (sinks & StdoutText)
        {
            out += colors.on;
            out += text;
            out += colors.off;
            out += '\n';
        }
        else if (sinks & StdoutJson)
            out += json;
    }
    void AppendFile(std::string& file) const
    {
        if (sinks & FileText)
        {
            file += text;
            file += '\n';
        }
        else if (sinks & FileJson)
            file += json;
    }
    void AppendTo(Batch& batch) const
    {
        AppendOut(batch.out);
        AppendFile(batch.file);
    }
};

// The writer takes one block at a time, so room comes back as each is written, not after the whole queue.
constexpr size_t s_BlockBytes = 64 * 1024;

// The writer thread and its queue. Guarded by s_SinkMutex, except where noted.
struct AsyncWriter
{
    AsyncOptions options;
    std::thread thread;
    std::condition_variable wake;     // to the writer: urgent lines, a flush or stop
    std::condition_variable progress; // to callers: room in the queue, or a block written
    // Per sink, blocks of up to s_BlockBytes that hold whole lines, so each write(2) keeps lines whole.
    std::deque<std::string> out;
    std::deque<std::string> file;
    std::vector<std::string> spare;   // written blocks, kept for reuse
    size_t bytes = 0;                 // queued, plus the block being written
    uint64_t dropped = 0;             // since the last report
    int outFd = -1;                   // the writer's dup of stdout; set while no writer runs
    bool outChunked = false;
    bool running = false;
    bool stop = false;
    bool urgent = false;
    bool busy = false;                // writing a block outside the lock
    bool busyFile = false;            // ... to the file
    bool draining = false;            // a caller waits to write the queue itself
    bool direct = false;              // after EmergencyFlush: nothing is queued any more
};

// Never freed: plugins log during static destruction.
AsyncWriter& Writer()
{
    static auto* s_writer = new AsyncWriter();
    return *s_writer;
}

thread_local bool t_IsWriter;

// The block to append size bytes to: a new one when the last has no room, so a line never spans two.
std::string& Tail(std::deque<std::string>& blocks, size_t size)
{
    AsyncWriter& w = Writer();
    if (blocks.empty() || (!blocks.back().empty() && blocks.back().size() + size > s_BlockBytes))
    {
        if (w.spare.empty())
            blocks.emplace_back().reserve(s_BlockBytes);
        else
        {
            blocks.push_back(std::move(w.spare.back()));
            w.spare.pop_back();
        }
    }
    return blocks.back();
}

void Recycle(std::string&& block)
{
    AsyncWriter& w = Writer();
    // Up to 1 MiB stays for bursts; a line longer than a block got a block of its own size.
    if (w.spare.size() < 16 && block.capacity() <= s_BlockBytes * 2)
    {
        block.clear();
        w.spare.push_back(std::move(block));
    }
}

void WriteBatch(const Batch& batch)
{
    AsyncWriter& w = Writer();
    if (!batch.out.empty())
        WriteLines(w.outFd, w.outChunked, batch.out);
    if (!batch.file.empty())
        WriteFile(batch.file);
}

// The writing done on the calling thread when there is no writer: std::cout for stdout, so lines keep their
// order with other std::cout users.
void WriteSync(Channel::Enum channel, const Lines& lines)
{
    if (lines.sinks & StdoutText)
        WriteText(channel, lines.text);
    else if (lines.sinks & StdoutJson)
    {
        std::cout.write(lines.json.data(), static_cast<std::streamsize>(lines.json.size()));
        std::cout.flush();
    }

    if (lines.sinks & (FileText | FileJson))
    {
        thread_local Batch t_file;
        Lines{ static_cast<uint8_t>(lines.sinks & (FileText | FileJson)), lines.text, lines.json, {} }.AppendTo(t_file);
        WriteFile(t_file.file);
        t_file.Clear();
    }
}

// Writes the queue on the calling thread, after the block the writer has in hand. FATAL and the crash handler
// wait for that only so long; without the lock at all this is a best effort, as the thread holding it is most
// likely the one crashing.
void Drain(std::unique_lock<std::mutex>& lock, bool bounded)
{
    AsyncWriter& w = Writer();
    if (lock.owns_lock())
    {
        // The writer stops taking blocks while this waits.
        w.draining = true;
        if (bounded)
            w.progress.wait_for(lock, std::chrono::seconds(1), [&w] { return !w.busy; });
        else
            w.progress.wait(lock, [&w] { return !w.busy; });
        w.draining = false;
    }
    for (auto& block : w.out)
        WriteLines(w.outFd, w.outChunked, block);
    for (auto& block : w.file)
        WriteFile(block);
    for (auto* blocks : { &w.out, &w.file })
    {
        for (auto& block : *blocks)
            Recycle(std::move(block));
        blocks->clear();
    }
    w.bytes = 0;
}

// Writes the lines on the calling thread through the writer's fds, after what is queued if the lock is held.
void WriteDirect(std::unique_lock<std::mutex>& lock, const Lines& lines)
{
    if (lock.owns_lock())
        Drain(lock, true);
    thread_local Batch t_batch;
    lines.AppendTo(t_batch);
    WriteBatch(t_batch);
    t_batch.Clear();
}

void LockSinks(std::unique_lock<std::mutex>& lock, bool bounded)
{
    // A crashing thread may already hold the lock: FATAL records and the crash handler wait for it only so long.
    if (!bounded)
        return lock.lock();
    for (int i = 0; i < 1000 && !lock.try_lock(); i++)
        std::this_thread::yield();
}

// Hands a record's finished lines to the sinks: text without its newline (cut at an embedded NUL, as it always
// has been), json with it.
void Deliver(Channel::Enum channel, uint8_t sinks, std::string_view text, std::string_view json)
{
    if (sinks & StdoutText)
    {
        INSTR_MESSAGE(text.data(), text.size());
    }
    else if (sinks & StdoutJson)
    {
        INSTR_MESSAGE(json.data(), json.size() - 1);
    }

    AsyncWriter& w = Writer();
    const Lines lines = { sinks, text, json, GetTextColors(channel) };
    if (t_IsWriter)
    {
        // The writer's own records (dropped counts): it owns the fds, and must not wait on its own queue.
        Batch batch;
        lines.AppendTo(batch);
        return WriteBatch(batch);
    }

    std::unique_lock<std::mutex> lock(s_SinkMutex, std::defer_lock);
    LockSinks(lock, channel == Channel::SEV_FATAL);
    if (!w.running)
        return WriteSync(channel, lines);

    // FATAL drains the queue first, so it comes out after everything logged before it.
    if (channel == Channel::SEV_FATAL || w.direct || !lock.owns_lock())
        return WriteDirect(lock, lines);

    const bool important = channel <= Channel::SEV_WARNING;
    const size_t outSize = lines.OutSize();
    const size_t fileSize = lines.FileSize();
    const size_t size = outSize + fileSize;
    if (w.bytes && w.bytes + size > w.options.maxBytes)
    {
        if (!important && w.options.dropWhenFull)
        {
            w.dropped++;
            return;
        }
        w.urgent = true;
        w.wake.notify_one();
        w.progress.wait(lock, [&w, size]
        {
            return !w.running || w.direct || !w.bytes || w.bytes + size <= w.options.maxBytes;
        });
        if (!w.running)
            return WriteSync(channel, lines);
        if (w.direct)
            return WriteDirect(lock, lines);
    }

    if (outSize)
        lines.AppendOut(Tail(w.out, outSize));
    if (fileSize)
        lines.AppendFile(Tail(w.file, fileSize));
    w.bytes += size;
    const bool wake = !w.urgent && (important || w.bytes > s_BlockBytes);
    w.urgent |= wake;
    lock.unlock();
    if (wake)
        w.wake.notify_one();
}

void ReportDropped(uint64_t dropped);

void WriterThread()
{
    t_IsWriter = true;
    pthread_setname_np(pthread_self(), "nwnx-log");
    AsyncWriter& w = Writer();
    const auto interval = std::chrono::milliseconds(w.options.flushMs);

    std::unique_lock<std::mutex> lock(s_SinkMutex);
    bool fileFirst = false;
    for (;;)
    {
        w.wake.wait_for(lock, interval, [&w] { return w.urgent || w.stop; });
        w.urgent = false;
        while ((!w.out.empty() || !w.file.empty() || w.dropped) && !w.draining)
        {
            if (!w.out.empty() || !w.file.empty())
            {
                // Take turns, so a stalled stdout does not hold the file back for good.
                fileFirst = !fileFirst;
                const bool toFile = w.out.empty() || (fileFirst && !w.file.empty());
                auto& blocks = toFile ? w.file : w.out;
                std::string block = std::move(blocks.front());
                blocks.pop_front();
                w.busy = true;
                w.busyFile = toFile;
                lock.unlock();

                if (toFile)
                    WriteFile(block);
                else
                    WriteLines(w.outFd, w.outChunked, block);

                lock.lock();
                w.busy = w.busyFile = false;
                w.bytes -= block.size();
                Recycle(std::move(block));
                w.progress.notify_all();
            }
            if (const uint64_t dropped = std::exchange(w.dropped, 0))
            {
                // The report is written on this thread, to both sinks.
                w.busy = w.busyFile = true;
                lock.unlock();
                ReportDropped(dropped);
                lock.lock();
                w.busy = w.busyFile = false;
                w.progress.notify_all();
            }
        }
        if (w.stop)
            break;
    }
    w.running = false;
    w.progress.notify_all();
}

// Writes a message that has no prefix in text: InternalTrace and WriteToLogFile.
void WriteUnprefixed(Channel::Enum channel, uint8_t sinks, const char* message)
{
    thread_local std::string t_json;
    if (sinks & JsonSinks)
    {
        EncodeJson(t_json, { channel, nullptr, nullptr, 0, message, {}, nullptr, {}, false });
        t_json += '\n';
    }
    Deliver(channel, sinks, message, t_json);
    if (t_json.capacity() > 64 * 1024)
        std::string().swap(t_json);
}

}

void StartAsync(const AsyncOptions& options)
{
    std::lock_guard<std::mutex> lock(s_SinkMutex);
    AsyncWriter& w = Writer();
    if (w.running || w.thread.joinable())
        return;
    w.outFd = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (w.outFd < 0)
        return;
    w.outChunked = IsPipe(w.outFd);
    w.options = options;
    w.options.flushMs = std::max(options.flushMs, 1u);
    w.running = true;
    w.stop = w.urgent = w.direct = false;

    // Process signals (SIGINT, SIGTERM, ...) stay with the game's threads.
    sigset_t all, previous;
    sigfillset(&all);
    for (int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT })
        sigdelset(&all, sig);
    pthread_sigmask(SIG_SETMASK, &all, &previous);
    w.thread = std::thread(WriterThread);
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);

    // exit() without NWNXCore::Shutdown must not lose the queue either.
    static const bool s_atExit = std::atexit(Shutdown) == 0;
    (void)s_atExit;
}

void Flush()
{
    AsyncWriter& w = Writer();
    std::unique_lock<std::mutex> lock(s_SinkMutex);
    if (!w.running || t_IsWriter)
        return;
    Drain(lock, false);
}

void Shutdown()
{
    AsyncWriter& w = Writer();
    std::unique_lock<std::mutex> lock(s_SinkMutex);
    if (!w.thread.joinable() || t_IsWriter)
        return;
    std::thread thread = std::move(w.thread);
    w.stop = true;
    w.wake.notify_one();
    lock.unlock();
    thread.join();

    lock.lock();
    ::close(w.outFd);
    w.outFd = -1;
}

void EmergencyFlush()
{
    AsyncWriter& w = Writer();
    std::unique_lock<std::mutex> lock(s_SinkMutex, std::defer_lock);
    LockSinks(lock, true);
    if (!w.running || w.direct)
        return;
    w.direct = true;
    Drain(lock, true);
}

namespace {
// Makes the caller the owner of the file's fd: waits while the writer writes a block to it.
void TakeFile(std::unique_lock<std::mutex>& lock)
{
    AsyncWriter& w = Writer();
    if (!w.busyFile)
        return;
    w.draining = true;
    w.progress.wait(lock, [&w] { return !w.busyFile; });
    w.draining = false;
    // The writer paused for us: let it go on with what is queued.
    if (w.bytes)
    {
        w.urgent = true;
        w.wake.notify_one();
    }
}

// Writes what is queued for the file to its current fd, before it is replaced: it was logged before. The caller owns the fd.
void WriteQueuedFile()
{
    AsyncWriter& w = Writer();
    for (auto& block : w.file)
    {
        WriteFile(block);
        w.bytes -= block.size();
        Recycle(std::move(block));
    }
    w.file.clear();
    w.progress.notify_all();
}
}

void SetLogFile(const std::string& logfilepath)
{
    // A new path starts a new file at the next record.
    std::unique_lock<std::mutex> lock(s_SinkMutex);
    TakeFile(lock);
    WriteQueuedFile();
    if (s_FileFd >= 0)
        ::close(s_FileFd);
    s_FileFd = -1;
    LogFilePath() = logfilepath;
}

void SetFileRotation(const FileRotation& rotation)
{
    std::lock_guard<std::mutex> lock(s_SinkMutex);
    s_FileRotation = rotation;
}

std::optional<std::string> RotateFile()
{
    if (t_IsWriter)
        return std::nullopt;
    // One rotation or reopen at a time, so a computed archive name stays free.
    std::lock_guard<std::mutex> rotating(s_RotateMutex);
    const std::string path = LogFilePath();
    struct stat st;
    if (path.empty() || ::stat(path.c_str(), &st) || !S_ISREG(st.st_mode))
    {
        LOG_ERROR("Cannot rotate the NWNX log file: there is none, or it is not a regular file");
        return std::nullopt;
    }

    // Logged first, so it is the archive's last line and the new file is empty of records until others log.
    const std::string archive = Rotation::ArchiveName(path, std::time(nullptr));
    LOG_NOTICE("Rotating '%s' to '%s'", path, archive);

    std::unique_lock<std::mutex> lock(s_SinkMutex);
    TakeFile(lock);
    WriteQueuedFile();

    const char* failed = nullptr;
    if (::rename(path.c_str(), archive.c_str()))
        failed = "rename";
    else if (!OpenLogFile(archive.c_str()))
    {
        failed = "open";
        const int error = errno;
        ::rename(archive.c_str(), path.c_str());
        errno = error;
    }
    const int error = errno;
    // A failure retries only after another ROTATE_SIZE bytes, not on every tick.
    s_RotateDue.store(false, std::memory_order_relaxed);
    if (failed)
        s_FileWritten.store(0, std::memory_order_relaxed);
    lock.unlock();

    if (failed)
    {
        LOG_ERROR("Cannot rotate '%s': %s failed: %s", path, failed, std::strerror(error));
        return std::nullopt;
    }
    Rotation::Prune(path, s_FileRotation.keep);
    return archive;
}

bool ReopenFile()
{
    if (t_IsWriter)
        return false;
    std::lock_guard<std::mutex> rotating(s_RotateMutex);
    std::unique_lock<std::mutex> lock(s_SinkMutex);
    if (LogFilePath().empty())
        return false;
    TakeFile(lock);
    const std::string path = LogFilePath();

    // Only a file moved away (external logrotate) is reopened; one not open yet opens at its first record.
    struct stat atPath, atFd;
    if (s_FileFd < 0 || (!::stat(path.c_str(), &atPath) && !::fstat(s_FileFd, &atFd) &&
        atPath.st_dev == atFd.st_dev && atPath.st_ino == atFd.st_ino))
        return false;
    WriteQueuedFile();
    const bool ok = OpenLogFile();
    const int error = errno;
    lock.unlock();

    if (!ok)
        LOG_ERROR("Cannot reopen '%s': %s", path, std::strerror(error));
    else
        LOG_NOTICE("Reopened '%s'", path);
    return ok;
}

void RequestReopen()
{
    s_ReopenDue.store(true, std::memory_order_relaxed);
}

bool Tick()
{
    if (const std::time_t period = s_FileRotation.period)
    {
        // Archives end on the boundary; a file with no records since the last rotation stays.
        static std::time_t s_nextBoundary;
        const std::time_t now = std::time(nullptr);
        if (now >= s_nextBoundary)
        {
            if (s_nextBoundary && s_FileWritten.load(std::memory_order_relaxed) > 0)
                s_RotateDue.store(true, std::memory_order_relaxed);
            s_nextBoundary = (now / period + 1) * period;
        }
    }
    if (s_RotateDue.load(std::memory_order_relaxed))
        RotateFile();
    if (!s_ReopenDue.load(std::memory_order_relaxed) || !s_ReopenDue.exchange(false))
        return false;
    ReopenFile();
    return true;
}

namespace {
void ReportDropped(uint64_t dropped)
{
    Record record(Channel::SEV_WARNING, PLUGIN_NAME, __FILE__, __LINE__);
    record.Stream() << "Dropped " << dropped << " log records: the log queue was full";
    record.Msg("log records dropped").With("dropped", static_cast<int64_t>(dropped)).Write();
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
    std::string keys;    // final field keys, for duplicates and static overrides
    std::string jsonMsg; // Msg()
    std::string origin;
    std::string originText; // the origin keys in logfmt
    std::string raw;     // string values as the text line shows them (game-locale bytes)
    std::vector<FieldIndex> index;
    StringSink sink{message};
    std::ostream stream{&sink};
    bool fieldsTruncated = false;
    bool hasJsonMsg = false;
    bool jsonMsgUtf8 = false;
    bool textFields = false;
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
    m_buffers->origin.clear();
    m_buffers->originText.clear();
    m_buffers->raw.clear();
    m_buffers->index.clear();
    m_buffers->fieldsTruncated = false;
    m_buffers->hasJsonMsg = false;
    m_buffers->jsonMsgUtf8 = false;
    m_buffers->textFields = false;

    // Same state as the fresh std::ostringstream each line used to get.
    m_stream = &m_buffers->stream;
    m_stream->clear();
    m_stream->flags(std::ios_base::skipws | std::ios_base::dec);
    m_stream->precision(6);
    m_stream->width(0);
    m_stream->fill(' ');

    const uint8_t sinks = ActiveSinks();
    m_json = sinks & JsonSinks;
    m_textRaw = s_TextFields && (sinks & TextSinks);
    m_fields = m_json || m_textRaw;
    if (m_fields)
        AddContext();
}

void Record::AddContext()
{
    if (ContextSource source = s_ContextSource.load(std::memory_order_relaxed))
        source(*this);
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
    for (std::string* buffer : { &m_buffers->message, &m_buffers->text, &m_buffers->json, &m_buffers->fields,
                                 &m_buffers->keys, &m_buffers->jsonMsg, &m_buffers->origin, &m_buffers->originText, &m_buffers->raw })
    {
        if (buffer->capacity() > maxKeptCapacity)
            std::string().swap(*buffer);
    }
    if (m_buffers->index.capacity() > 1024)
        std::vector<FieldIndex>().swap(m_buffers->index);
}

static Json::StringOptions ValueOptions(Json::Encoding encoding, bool stripColors = s_JsonOptions.stripColors)
{
    return { encoding, String::GetDefaultLocale(), stripColors, s_JsonOptions.maxValue };
}

std::string& Record::BeginField(std::string_view key, bool reservedAllowed)
{
    Buffers& b = *m_buffers;
    const int reserved = reservedAllowed ? -1 : Json::ReservedKeyIndex(key);
    if (reserved >= 0)
    {
        // Warn once per key, where it is first used.
        static std::atomic<uint32_t> s_warned;
        const uint32_t bit = 1u << reserved;
        if (!(s_warned.fetch_or(bit, std::memory_order_relaxed) & bit) && m_plugin && GetLogLevel(m_plugin) >= Channel::SEV_WARNING)
        {
            Record warning(Channel::SEV_WARNING, m_plugin, m_file, m_line);
            warning.Stream() << "Log field '" << key << "' is a reserved key; it is written as '_" << key << "'.";
            warning.Write();
        }
    }

    const auto keyOffset = static_cast<uint32_t>(b.keys.size());
    if (reserved >= 0)
        b.keys += '_';
    b.keys += key;
    const std::string_view name = std::string_view(b.keys).substr(keyOffset);

    // A later value for the same key replaces the earlier one, at the later position.
    for (size_t i = 0; i < b.index.size(); i++)
    {
        if (std::string_view(b.keys).substr(b.index[i].keyOffset, b.index[i].keyLength) != name)
            continue;
        const uint32_t start = b.index[i].start;
        const uint32_t end = i + 1 < b.index.size() ? b.index[i + 1].start : static_cast<uint32_t>(b.fields.size());
        b.fields.erase(start, end - start);
        b.index.erase(b.index.begin() + static_cast<ptrdiff_t>(i));
        for (size_t j = i; j < b.index.size(); j++)
        {
            b.index[j].start -= end - start;
            b.index[j].valueStart -= end - start;
        }
        break;
    }

    const auto start = static_cast<uint32_t>(b.fields.size());
    b.fields += ",\"";
    Json::AppendKeyContents(b.fields, name);
    b.fields += "\":";
    b.index.push_back({ keyOffset, static_cast<uint32_t>(name.size()), start, static_cast<uint32_t>(b.fields.size()) });
    return b.fields;
}

Record& Record::AddString(std::string_view key, std::string_view value, StringKind kind)
{
    const Json::Encoding mode = kind == StringKind::Game ? Json::Encoding::Locale
                              : kind == StringKind::Utf8 ? Json::Encoding::Utf8 : s_JsonOptions.encoding;
    std::string& out = BeginField(key);
    m_buffers->fieldsTruncated |= !Json::AppendString(out, value, ValueOptions(mode));
    if (m_textRaw)
    {
        // Text lines are in game-locale bytes, so the value is kept as given, colours and all.
        if (kind == StringKind::Utf8)
            SetRaw(String::FromUTF8(std::string(value), String::GetDefaultLocale()));
        else
            SetRaw(value);
    }
    return *this;
}

void Record::SetRaw(std::string_view value)
{
    FieldIndex& field = m_buffers->index.back();
    field.rawStart = static_cast<uint32_t>(m_buffers->raw.size());
    field.rawLength = static_cast<uint32_t>(value.size());
    m_buffers->raw += value;
}

Record& Record::With(std::string_view key, const CExoString& value)
{
    return With(key, std::string_view(value.CStr(), value.GetLength()));
}

Record& Record::With(std::string_view key, const CResRef& value)
{
    return With(key, std::string_view(value.GetResRefStr()));
}

Record& Record::AddOid(std::string_view key, uint32_t id)
{
    char buffer[16];
    const int length = std::snprintf(buffer, sizeof(buffer), "\"%08x\"", id);
    BeginField(key).append(buffer, static_cast<size_t>(length));
    if (m_textRaw)
        SetRaw(std::string_view(buffer + 1, static_cast<size_t>(length) - 2));
    return *this;
}

Record& Record::AddRaw(std::string_view key, std::string_view json)
{
    BeginField(key).append(json.empty() ? "null" : json);
    return *this;
}

Record& Record::AddInt(std::string_view key, int64_t value)
{
    Json::AppendInt(BeginField(key), value);
    return *this;
}

Record& Record::AddUInt(std::string_view key, uint64_t value)
{
    Json::AppendUInt(BeginField(key), value);
    return *this;
}

Record& Record::AddDouble(std::string_view key, double value)
{
    Json::AppendDouble(BeginField(key), value);
    return *this;
}

Record& Record::AddJson(std::string_view key, const nlohmann::json& value)
{
    std::string& out = BeginField(key);
    m_buffers->fieldsTruncated |= !Json::AppendValue(out, value, ValueOptions(s_JsonOptions.encoding));
    return *this;
}

Record& Record::AddErr(std::string_view message, std::optional<int64_t> code)
{
    std::string& out = BeginField("err", true);
    out += "{\"message\":";
    m_buffers->fieldsTruncated |= !Json::AppendString(out, message, ValueOptions(s_JsonOptions.encoding));
    if (code)
    {
        out += ",\"code\":";
        Json::AppendInt(out, *code);
    }
    out += '}';
    return *this;
}

Record& Record::AddStack(std::string_view key, std::string_view trace)
{
    // Frames are the lines indented by four spaces.
    const Json::StringOptions options = ValueOptions(s_JsonOptions.encoding, false);
    std::string& out = BeginField(key);
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
    return *this;
}

Record& Record::Msg(std::string_view msg)
{
    if (m_json)
    {
        m_buffers->jsonMsg.assign(msg);
        m_buffers->hasJsonMsg = true;
    }
    return *this;
}

Record& Record::Msg(Utf8 msg)
{
    Msg(msg.s);
    m_buffers->jsonMsgUtf8 = m_json;
    return *this;
}

// Appends a string value in logfmt: bare unless it is empty or has a space, '=', a quote, a backslash or a
// control character; then quoted with those escaped. Other bytes stay as they are.
static void AppendLogfmtString(std::string& out, std::string_view value)
{
    const auto needsQuotes = [](unsigned char c) { return c <= ' ' || c == '=' || c == '"' || c == '\\' || c == 0x7f; };
    if (!value.empty() && std::none_of(value.begin(), value.end(), needsQuotes))
    {
        out += value;
        return;
    }
    out += '"';
    for (const char c : value)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < ' ' || c == 0x7f)
                {
                    char buffer[8];
                    out.append(buffer, static_cast<size_t>(std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c))));
                }
                else
                    out += c;
        }
    }
    out += '"';
}

Record& Record::Origin(const char* key, std::string_view value)
{
    if (m_json)
    {
        m_buffers->origin.append(",\"").append(key).append("\":");
        m_buffers->fieldsTruncated |= !Json::AppendString(m_buffers->origin, value, ValueOptions(s_JsonOptions.encoding));
    }
    if (m_textRaw)
    {
        m_buffers->originText.append(" ").append(key).append("=");
        AppendLogfmtString(m_buffers->originText, value);
    }
    return *this;
}

Record& Record::TextOnly()
{
    m_textOnly = true;
    m_json = false;
    m_fields = m_fields && (s_TextFields || m_buffers->textFields);
    return *this;
}

Record& Record::TextFields()
{
    m_buffers->textFields = true;
    if (!m_textRaw && (ActiveSinks() & TextSinks))
    {
        m_textRaw = true;
        // (Re)adds the context so its strings keep their text form too; nothing else is set yet.
        m_fields = true;
        AddContext();
    }
    return *this;
}

// Appends the fields in logfmt: strings as their text-mode bytes, any other value as its JSON.
static void AppendLogfmt(std::string& out, const Record::Buffers& buffers)
{
    const std::string& fields = buffers.fields;
    for (size_t i = 0; i < buffers.index.size(); i++)
    {
        const FieldIndex& field = buffers.index[i];
        out += ' ';
        out.append(buffers.keys, field.keyOffset, field.keyLength);
        out += '=';
        if (field.rawStart != UINT32_MAX)
        {
            AppendLogfmtString(out, std::string_view(buffers.raw).substr(field.rawStart, field.rawLength));
            continue;
        }
        const size_t end = i + 1 < buffers.index.size() ? buffers.index[i + 1].start : fields.size();
        out.append(fields, field.valueStart, end - field.valueStart);
    }
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
    if (s_PrintSource && file)
    {
        out += '[';
        out += Basename(file);
        out += ':';
        out.append(buffer, std::to_chars(buffer, buffer + sizeof(buffer), line).ptr);
        out += "] ";
    }
}

static void WriteRecord(Record::Buffers& buffers, Channel::Enum channel, const char* plugin, const char* file, int line,
                        const char* src, uint8_t sinks)
{
    if (sinks & TextSinks)
    {
        buffers.text.clear();
        AppendPrefix(buffers.text, channel, plugin, file, line);
        buffers.text += buffers.message;
        if (s_TextFields || buffers.textFields)
        {
            buffers.text += buffers.originText;
            AppendLogfmt(buffers.text, buffers);
        }
    }
    if (sinks & JsonSinks)
    {
        const std::string_view msg = buffers.hasJsonMsg ? buffers.jsonMsg : buffers.message;
        EncodeJson(buffers.json, { channel, plugin, file, line, msg, buffers.fields, &buffers.index, buffers.keys,
                                   buffers.fieldsTruncated, src, buffers.origin, buffers.hasJsonMsg && buffers.jsonMsgUtf8 });
        buffers.json += '\n';
    }
    Deliver(channel, sinks, buffers.text.c_str(), buffers.json);
}

void Record::Format()
{
    if (m_format)
        tfm::vformat(*m_stream, m_format, *m_args);
    m_format = nullptr;
}

void Record::Write()
{
    const uint8_t sinks = ActiveSinks() & (m_textOnly ? TextSinks : 0xff);
    static const std::atomic<bool>* s_hasSubscribers = &MessageBus::GetSubscribedFlag("NWNX_CORE_LOG_MESSAGE");
    const bool subscribed = s_hasSubscribers->load(std::memory_order_relaxed);
    // JSON with Msg() is the one case that never shows the formatted message.
    if (subscribed || (sinks & TextSinks) || !m_buffers->hasJsonMsg)
        Format();

    if (subscribed)
    {
        MessageBus::Broadcast("NWNX_CORE_LOG_MESSAGE", {
            std::to_string((int) m_channel),
            m_plugin ? m_plugin : "",
            m_file ? m_file : "",
            std::to_string(m_line),
            m_buffers->message
        });
    }

    WriteRecord(*m_buffers, m_channel, m_plugin, m_file, m_line, m_src, sinks);

    if (m_channel == Channel::SEV_FATAL)
    {
        ASSERT_FAIL();
        std::abort();
    }
}

void Record::WriteJson()
{
    if (const uint8_t sinks = ActiveSinks() & JsonSinks)
    {
        if (!m_buffers->hasJsonMsg)
            Format();
        WriteRecord(*m_buffers, m_channel, m_plugin, m_file, m_line, m_src, sinks);
    }
}

}

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
