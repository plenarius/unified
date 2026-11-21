#include "Rotation.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace NWNXLib::Log::Rotation {

namespace {

bool IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)); }

// Archive suffix: YYYYmmddTHHMMSSZ, plus -N when that second is already taken.
bool IsArchiveSuffix(const char* s)
{
    for (int i = 0; i < 15; ++i)
    {
        if (i == 8 ? s[i] != 'T' : !IsDigit(s[i]))
            return false;
    }
    if (s[15] != 'Z')
        return false;
    s += 16;
    if (!*s)
        return true;
    if (*s++ != '-' || !*s)
        return false;
    for (; *s; ++s)
    {
        if (!IsDigit(*s))
            return false;
    }
    return true;
}

struct Archives
{
    std::string dir;
    size_t stampEnd = 0;            // where the time stamp ends in each name
    std::vector<std::string> names; // oldest first
};

// The -N of a same-second archive; 0 for the first.
unsigned long long Seq(const std::string& name, size_t stampEnd)
{
    return name.size() > stampEnd ? std::strtoull(name.c_str() + stampEnd + 1, nullptr, 10) : 0;
}

Archives List(const std::string& path)
{
    const auto slash = path.rfind('/');
    const std::string prefix = (slash == std::string::npos ? path : path.substr(slash + 1)) + ".";
    Archives archives;
    archives.dir = slash == std::string::npos ? "." : path.substr(0, slash);
    archives.stampEnd = prefix.size() + 16;

    if (DIR* d = opendir(archives.dir.c_str()))
    {
        while (auto* entry = readdir(d))
        {
            if (!std::strncmp(entry->d_name, prefix.c_str(), prefix.size()) && IsArchiveSuffix(entry->d_name + prefix.size()))
                archives.names.emplace_back(entry->d_name);
        }
        closedir(d);
    }

    // By time stamp, then by -N numerically (-2 before -10).
    const size_t stampEnd = archives.stampEnd;
    std::sort(archives.names.begin(), archives.names.end(), [stampEnd](const std::string& a, const std::string& b)
    {
        const int cmp = a.compare(0, stampEnd, b, 0, stampEnd);
        return cmp ? cmp < 0 : Seq(a, stampEnd) < Seq(b, stampEnd);
    });
    return archives;
}

}

std::string ArchiveName(const std::string& path, std::time_t when)
{
    std::tm tm;
    char stamp[32];
    gmtime_r(&when, &tm);
    std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm);

    // A second archive in the same second gets the next -N above all existing ones, so the newest
    // always sorts last, even after pruning freed a lower name.
    const auto archives = List(path);
    bool taken = false;
    unsigned long long next = 1;
    for (const auto& name : archives.names)
    {
        if (!name.compare(archives.stampEnd - 16, 16, stamp))
        {
            taken = true;
            next = std::max(next, Seq(name, archives.stampEnd) + 1);
        }
    }
    return path + "." + stamp + (taken ? "-" + std::to_string(next) : "");
}

void Prune(const std::string& path, int keep)
{
    if (keep <= 0)
        return;

    const auto archives = List(path);
    for (size_t i = 0; i + static_cast<size_t>(keep) < archives.names.size(); ++i)
        unlink((archives.dir + "/" + archives.names[i]).c_str());
}

std::string KeepPrevious(const std::string& path, int keep)
{
    struct stat st;
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size == 0)
        return {};

    auto archive = ArchiveName(path, st.st_mtime);
    if (rename(path.c_str(), archive.c_str()))
        return {};
    Prune(path, keep);
    return archive;
}

std::optional<off_t> ParseSize(const std::string& value)
{
    char* end;
    const unsigned long long n = std::strtoull(value.c_str(), &end, 10);
    if (IsDigit(value[0]) && (!*end || !end[1]))
    {
        switch (std::toupper(static_cast<unsigned char>(*end)))
        {
            case 0:   return n;
            case 'K': return n << 10;
            case 'M': return n << 20;
            case 'G': return n << 30;
        }
    }
    return std::nullopt;
}

std::optional<std::time_t> ParsePeriod(const std::string& value)
{
    if (!strcasecmp(value.c_str(), "hourly"))
        return 3600;
    if (!strcasecmp(value.c_str(), "daily"))
        return 86400;
    if (!strcasecmp(value.c_str(), "off"))
        return 0;
    return std::nullopt;
}

}
