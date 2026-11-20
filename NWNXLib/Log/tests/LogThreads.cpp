// Several threads log at once to stdout and the log file, in JSON and in text: every line must come out
// whole. Run it under TSan to check the sinks are race free.
#include "nwnx.hpp"
#include "Log/Json.hpp"
#include "External/json/json.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using namespace NWNXLib;

constexpr int s_threads = 8;
constexpr int s_lines = 2000;

static void LogFromThreads()
{
    std::vector<std::thread> threads;
    for (int t = 0; t < s_threads; t++)
    {
        threads.emplace_back([t]
        {
            for (int i = 0; i < s_lines; i++)
            {
                if (i % 2)
                    LOG_NOTICE("thread %d line %d %s", t, i, std::string(static_cast<size_t>(i % 300), 'x'));
                else
                {
                    Log::Record record(Log::Channel::SEV_NOTICE, PLUGIN_NAME, __FILE__, __LINE__);
                    record.Stream() << "thread " << t << " line " << i;
                    record.With("thread", t).With("text", "<c\xff\x01\x01>caf\xe9</c>").Write();
                }
            }
        });
    }
    for (auto& thread : threads)
        thread.join();
}

static std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Counts whole lines: JSON records that parse, or text lines with the expected prefix and ending.
static int CountLines(const std::string& output, bool json, int& bad)
{
    int count = 0;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.find("NWNX secondary log file") != std::string::npos || line.find("====") == 0 ||
            line.find("Please attach stdout") != std::string::npos)
            continue;
        bool ok;
        if (json)
        {
            try
            {
                auto record = nlohmann::json::parse(line);
                ok = record.at("msg").get<std::string>().find("thread ") == 0;
            }
            catch (const std::exception&)
            {
                ok = false;
            }
        }
        else
            ok = line.find("N [NWNX_LogThreads] [LogThreads.cpp:") == 0 && line.find("thread ") != std::string::npos;
        if (!ok)
        {
            if (bad++ < 5)
                std::fprintf(stderr, "torn line: %.200s\n", line.c_str());
            continue;
        }
        count++;
    }
    return count;
}

int main()
{
    const char* tmp = std::getenv("TMPDIR");
    std::string tmpDir = std::string(tmp && *tmp ? tmp : "/tmp") + "/nwnx-log-threads-XXXXXX";
    if (!mkdtemp(tmpDir.data()))
        return 2;
    const std::string stdoutPath = tmpDir + "/stdout.txt";
    const std::string filePath = tmpDir + "/file.log";

    Log::SetPrintTimestamp(false);
    Log::SetPrintPlugin(true);
    Log::SetPrintSource(true);
    Log::SetColorOutput(false);
    Log::SetLogFile(filePath);

    int failures = 0;
    for (auto format : {Log::Format::Json, Log::Format::Text})
    {
        Log::SetFormat(format);
        Log::SetFileFormat(format == Log::Format::Json ? Log::Format::Text : Log::Format::Json);

        std::fflush(stdout);
        const int saved = dup(1);
        const int fd = open(stdoutPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 1);
        close(fd);
        LogFromThreads();
        std::fflush(stdout);
        dup2(saved, 1);
        close(saved);

        int bad = 0;
        const bool json = format == Log::Format::Json;
        const int out = CountLines(ReadFile(stdoutPath), json, bad);
        std::printf("stdout %s: %d lines, %d torn\n", json ? "json" : "text", out, bad);
        failures += bad + (out != s_threads * s_lines);
        std::remove(stdoutPath.c_str());
    }

    // The file was opened as text (first round's file format) and then received JSON lines.
    int bad = 0;
    const std::string file = ReadFile(filePath);
    const size_t split = file.find("\n{");
    const int textLines = CountLines(file.substr(0, split + 1), false, bad);
    const int jsonLines = CountLines(file.substr(split + 1), true, bad);
    std::printf("file: %d text lines, %d json lines, %d torn\n", textLines, jsonLines, bad);
    failures += bad + (textLines != s_threads * s_lines) + (jsonLines != s_threads * s_lines);
    std::remove(filePath.c_str());
    rmdir(tmpDir.c_str());
    return failures ? 1 : 0;
}
