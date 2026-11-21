#include "nwnx.hpp"
#include "EngineLogFiles.hpp"

#include "API/CExoBase.hpp"
#include "API/CExoDebug.hpp"
#include "API/CExoDebugInternal.hpp"
#include "API/CExoFile.hpp"
#include "Log/Rotation.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

using namespace NWNXLib;
using namespace NWNXLib::API;

namespace Core::EngineLogFiles {

namespace {

struct Tracked
{
    std::atomic<FILE*> fp{nullptr};
    std::string path;
    off_t start = 0; // bytes NWNX wrote at the start of the file (the rotation banner)
};

// Guards the paths, and orders an engine open, fclose and Rotate against each other.
std::mutex s_lock;
// Set while this thread holds s_lock in Open or Rotate, so a crash handler's fclose there does not wait on it.
thread_local bool t_holding;
Tracked s_files[2];

const char* Name(File file)
{
    return file == File::Engine ? "nwengineLog.txt" : "nwserverLog<N>.txt";
}

bool IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)); }

// One archive count for keep-previous and live rotation; 0 keeps all.
int Keep()
{
    static const int s_keep = Config::Get<int>("LOG_ENGINE_KEEP", 7);
    return s_keep;
}

// Matches the basenames the engine opens: nwengineLog.txt and nwserverLog<N>.txt.
std::optional<File> Classify(const char* path)
{
    const char* slash = std::strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    if (!std::strcmp(base, "nwengineLog.txt"))
        return File::Engine;
    if (std::strncmp(base, "nwserverLog", 11) || !IsDigit(base[11]))
        return std::nullopt;
    const char* p = base + 12;
    while (IsDigit(*p))
        ++p;
    return std::strcmp(p, ".txt") ? std::nullopt : std::make_optional(File::Server);
}

// Moves last run's file aside before the engine truncates it. Called with s_lock held.
void KeepPrevious(const char* path)
{
    auto archive = Log::Rotation::KeepPrevious(path, Keep());
    if (archive.empty())
        return;

    // The engine may open its log on a worker thread inside its own log lock: report from the main thread.
    Tasks::QueueOnMainThread([archive = std::move(archive)]
    {
        LOG_NOTICE("Kept the previous run's log as '%s'", archive);
    });
}

using FOpenType = FILE*(*)(const char*, const char*);

FILE* Open(File file, const char* path, const char* mode, FOpenType realOpen)
{
    static const bool s_keepPrevious = Config::Get<bool>("LOG_ENGINE_KEEP_PREVIOUS", false);

    std::lock_guard<std::mutex> lock(s_lock);
    t_holding = true;
    if (s_keepPrevious)
        KeepPrevious(path);

    FILE* fp = realOpen(path, mode);
    if (fp)
    {
        auto& tracked = s_files[static_cast<int>(file)];
        // The engine joins its folder and the name with a second slash ("logs.0//nwengineLog.txt").
        tracked.path = path;
        tracked.path.erase(std::unique(tracked.path.begin(), tracked.path.end(), [](char a, char b) { return a == '/' && b == '/'; }), tracked.path.end());
        tracked.start = 0;
        tracked.fp.store(fp);
    }
    t_holding = false;
    return fp;
}

void Forget(FILE* fp)
{
    for (auto& tracked : s_files)
    {
        if (tracked.fp.load(std::memory_order_relaxed) == fp)
        {
            std::unique_lock<std::mutex> lock(s_lock, std::defer_lock);
            if (!t_holding)
                lock.lock();
            if (tracked.fp.load() == fp)
                tracked.fp.store(nullptr);
        }
    }
}

// A rotated file starts by naming the part before it: a server log with the engine's own banner
// (ctime, local time), the engine log with one dated line, as its lines carry only the time of day.
// Returns the bytes written.
off_t WriteBanner(File file, FILE* fp, std::time_t now, const std::string& archive)
{
    static constexpr const char* s_dashes = "--------------------------------------";
    const auto slash = archive.rfind('/');
    const char* from = archive.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    std::tm tm;
    char when[32];
    int written;
    if (file == File::Server)
    {
        localtime_r(&now, &tm);
        std::strftime(when, sizeof(when), "%a %b %e %H:%M:%S %Y", &tm);
        written = std::fprintf(fp, "\n%s\nMessages for: %s\nRotated from: %s\n%s\n", s_dashes, when, from, s_dashes);
    }
    else
    {
        gmtime_r(&now, &tm);
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        written = std::fprintf(fp, "----- Rotated at %s UTC from %s\n", when, from);
    }
    std::fflush(fp);
    return std::max(written, 0);
}

// Bytes written since the file was opened or rotated, or -1 if the engine does not have it open.
off_t Written(File file)
{
    std::lock_guard<std::mutex> lock(s_lock);
    auto& tracked = s_files[static_cast<int>(file)];
    FILE* fp = tracked.fp.load();
    return fp ? ftello(fp) - tracked.start : -1;
}

// Size and period, warning about malformed values.
off_t ParseSize(const std::string& value)
{
    if (auto size = Log::Rotation::ParseSize(value))
        return *size;
    LOG_WARNING("Ignoring NWNX_CORE_LOG_ENGINE_ROTATE_SIZE '%s': expected bytes with an optional K, M or G", value);
    return 0;
}
std::time_t ParsePeriod(const std::string& value)
{
    if (auto period = Log::Rotation::ParsePeriod(value))
        return *period;
    LOG_WARNING("Ignoring NWNX_CORE_LOG_ENGINE_ROTATE_TIME '%s': expected hourly, daily or off", value);
    return 0;
}

// The stream the engine writes file to now, or nullptr; debug gets the server log's state. Called with s_lock held.
FILE* Live(File file, CExoDebugInternal*& debug)
{
    FILE* fp = s_files[static_cast<int>(file)].fp.load();
    debug = nullptr;
    if (file == File::Server)
    {
        // The engine may have moved on to nwserverLog<N+1>.txt: only the file it writes now counts.
        if (auto* exoBase = Globals::ExoBase(); exoBase && exoBase->m_pcExoDebug)
            debug = exoBase->m_pcExoDebug->m_pcExoDebugInternal;
        if (!debug || !debug->m_bFilesOpen || !debug->m_pLogFile || debug->m_pLogFile->GetFileHandle() != fp)
            fp = nullptr;
    }
    return fp;
}

}

std::optional<std::string> Rotate(File file)
{
    std::unique_lock<std::mutex> lock(s_lock);
    t_holding = true;
    auto& tracked = s_files[static_cast<int>(file)];
    CExoDebugInternal* debug;
    FILE* fp = Live(file, debug);
    if (!fp)
    {
        t_holding = false;
        lock.unlock();
        LOG_ERROR("Cannot rotate %s: the engine does not have it open", Name(file));
        return std::nullopt;
    }

    const std::string path = tracked.path;
    const std::time_t now = std::time(nullptr);
    const std::string archive = Log::Rotation::ArchiveName(path, now);
    const char* failed = nullptr;
    int error = 0;

    // Under the stream lock no engine thread can write or flush: everything buffered goes to the
    // archive, everything after to the new file. The FILE and its descriptor number stay the same.
    flockfile(fp);
    std::fflush(fp);
    if (rename(path.c_str(), archive.c_str()))
    {
        failed = "rename";
        error = errno;
    }
    else
    {
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0 || dup2(fd, fileno(fp)) < 0)
        {
            failed = fd < 0 ? "open" : "dup2";
            error = errno;
            rename(archive.c_str(), path.c_str());
        }
        else
        {
            // Resync the stream's cached offset with the new file.
            fseeko(fp, 0, SEEK_SET);
            tracked.start = WriteBanner(file, fp, now, archive);
        }
        if (fd >= 0)
            close(fd);
    }
    funlockfile(fp);

    if (!failed && debug)
        debug->m_nCurrentLogSize = static_cast<int32_t>(tracked.start);
    t_holding = false;
    lock.unlock();

    if (failed)
    {
        LOG_ERROR("Cannot rotate '%s': %s failed: %s", path, failed, std::strerror(error));
        return std::nullopt;
    }

    Log::Rotation::Prune(path, Keep());
    LOG_NOTICE("Rotated '%s' to '%s'", path, archive);
    return archive;
}

bool Reopen(File file)
{
    std::unique_lock<std::mutex> lock(s_lock);
    t_holding = true;
    auto& tracked = s_files[static_cast<int>(file)];
    CExoDebugInternal* debug;
    FILE* fp = Live(file, debug);
    const std::string path = tracked.path;

    // Only a file moved away (external logrotate with create, not copytruncate) is reopened.
    struct stat atPath, atFd;
    if (!fp || (!stat(path.c_str(), &atPath) && !fstat(fileno(fp), &atFd) && atPath.st_dev == atFd.st_dev && atPath.st_ino == atFd.st_ino))
    {
        t_holding = false;
        return false;
    }

    // As in Rotate: what is buffered goes to the moved file, the rest to the new one.
    flockfile(fp);
    std::fflush(fp);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    const bool ok = fd >= 0 && dup2(fd, fileno(fp)) >= 0;
    const int error = errno;
    if (fd >= 0)
        close(fd);
    if (ok)
    {
        fseeko(fp, 0, SEEK_END);
        tracked.start = ftello(fp);
        if (debug)
            debug->m_nCurrentLogSize = static_cast<int32_t>(tracked.start);
    }
    funlockfile(fp);
    t_holding = false;
    lock.unlock();

    if (!ok)
        LOG_ERROR("Cannot reopen '%s': %s", path, std::strerror(error));
    else
        LOG_NOTICE("Reopened '%s'", path);
    return ok;
}

void Tick()
{
    static const off_t s_size = ParseSize(Config::Get<std::string>("LOG_ENGINE_ROTATE_SIZE", "0"));
    static const std::time_t s_period = ParsePeriod(Config::Get<std::string>("LOG_ENGINE_ROTATE_TIME", "off"));
    if (!s_size && !s_period)
        return;

    // The time boundary is checked every tick so archives end on it; the size only every 10 s.
    static std::time_t s_nextBoundary, s_nextSizeCheck;
    const std::time_t now = std::time(nullptr);
    if (s_period && !s_nextBoundary)
        s_nextBoundary = (now / s_period + 1) * s_period;
    const bool boundary = s_period && now >= s_nextBoundary;
    const bool sizeCheck = s_size && now >= s_nextSizeCheck;
    if (!boundary && !sizeCheck)
        return;
    if (boundary)
        s_nextBoundary = (now / s_period + 1) * s_period;
    if (sizeCheck)
        s_nextSizeCheck = now + 10;

    for (const File file : {File::Engine, File::Server})
    {
        const off_t written = Written(file);
        if ((boundary && written > 0) || (sizeCheck && written >= s_size))
            Rotate(file);
    }
}

}

using namespace Core::EngineLogFiles;

// The engine opens nwengineLog.txt and nwserverLog<N>.txt with fopen64(path, "w").
extern "C" FILE* fopen64(const char* path, const char* mode)
{
    static const auto realOpen = reinterpret_cast<FOpenType>(dlsym(RTLD_NEXT, "fopen64"));
    if (path && mode && mode[0] == 'w')
    {
        if (auto file = Classify(path))
            return Open(*file, path, mode, realOpen);
    }
    return realOpen(path, mode);
}

extern "C" int fclose(FILE* fp)
{
    using Type = int(*)(FILE*);
    static const auto realClose = reinterpret_cast<Type>(dlsym(RTLD_NEXT, "fclose"));
    Forget(fp);
    return realClose(fp);
}
