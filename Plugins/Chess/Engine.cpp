#include "Engine.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace Chess {

static constexpr int32_t UCI_INIT_TIMEOUT_MS = 10000; // loading the NNUE nets takes a moment
static constexpr int32_t SEARCH_GRACE_MS = 5000;      // allowed past the move time before "stop"
static constexpr int32_t STOP_TIMEOUT_MS = 2000;      // after "stop", before the engine is killed

static bool IsExecutableFile(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

std::string Engine::FindBinary(const std::string& configured)
{
    if (configured.empty())
        return "";
    if (configured.find('/') != std::string::npos)
        return IsExecutableFile(configured) ? configured : "";

    std::string path;
    if (const char* env = std::getenv("PATH"))
        path = env;
    // Debian and Ubuntu install the stockfish package in /usr/games, which isn't always on PATH.
    path += ":/usr/games:/usr/local/games";

    std::istringstream dirs(path);
    std::string dir;
    while (std::getline(dirs, dir, ':'))
    {
        if (dir.empty())
            continue;
        auto candidate = dir + "/" + configured;
        if (IsExecutableFile(candidate))
            return candidate;
    }
    return "";
}

Engine::Engine(EngineConfig config, ResultCallback onResult, LogCallback onLog)
  : m_config(std::move(config)), m_onResult(std::move(onResult)), m_onLog(std::move(onLog))
{
    m_binary = FindBinary(m_config.path);
    m_thread = std::thread([this]() { Run(); });
}

Engine::~Engine()
{
    Stop();
}

void Engine::Log(LogLevel level, const std::string& message)
{
    if (m_onLog)
        m_onLog(level, message);
}

bool Engine::Submit(EngineRequest request, std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stop)
    {
        error = "the engine is shutting down";
        return false;
    }
    if (m_binary.empty())
    {
        // Look again: the binary may have been installed since.
        m_binary = FindBinary(m_config.path);
        if (m_binary.empty())
        {
            error = "chess engine '" + m_config.path + "' not found (set NWNX_CHESS_STOCKFISH_PATH)";
            return false;
        }
    }

    const auto gameId = request.gameId;
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                 [gameId](const EngineRequest& r) { return r.gameId == gameId; }),
                  m_queue.end());
    if (m_queue.size() >= m_config.maxQueue)
    {
        error = "too many engine requests queued";
        return false;
    }
    m_queue.push_back(std::move(request));
    m_cv.notify_one();
    return true;
}

void Engine::Cancel(int32_t gameId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                 [gameId](const EngineRequest& r) { return r.gameId == gameId; }),
                  m_queue.end());
}

void Engine::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop && !m_thread.joinable())
            return;
        m_stop = true;
        m_queue.clear();
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void Engine::Run()
{
    // A write to an engine that has died raises SIGPIPE, which would kill the server. Block it in
    // this thread: the write then fails with EPIPE, and the pending signal is cleared in Send().
    sigset_t pipeSet;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSet, nullptr);

    while (true)
    {
        EngineRequest request;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return m_stop || !m_queue.empty(); });
            if (m_stop)
                break;
            request = std::move(m_queue.front());
            m_queue.pop_front();
        }

        auto result = Search(request);
        if (m_onResult)
            m_onResult(std::move(result));
    }

    if (m_pid > 0)
    {
        Send("quit");
        // Give it a moment to exit by itself before it is killed.
        for (int i = 0; i < 20; i++)
        {
            if (waitpid(m_pid, nullptr, WNOHANG) != 0)
            {
                m_pid = -1;
                break;
            }
            usleep(10000);
        }
    }
    KillProcess();
}

EngineResult Engine::Search(const EngineRequest& request)
{
    EngineResult result;
    result.requestId = request.requestId;
    result.gameId = request.gameId;

    std::string error;
    if (!EnsureStarted(error))
    {
        result.error = error;
        return result;
    }

    const auto skill = std::clamp(request.skill, 0, 20);
    const auto moveTime = std::max(request.moveTimeMs, 1);

    bool ok = true;
    if (skill != m_currentSkill)
    {
        ok = Send("setoption name Skill Level value " + std::to_string(skill));
        m_currentSkill = skill;
    }
    if (ok && request.gameId != m_lastGameId)
    {
        // Another game: clear the hash so one game's search doesn't leak into the next.
        ok = Send("ucinewgame");
        m_lastGameId = request.gameId;
    }
    ok = ok && Send("isready") && WaitFor("readyok", UCI_INIT_TIMEOUT_MS);
    ok = ok && Send(request.position) && Send("go movetime " + std::to_string(moveTime));
    if (!ok)
    {
        result.error = "lost the connection to the chess engine";
        KillProcess();
        return result;
    }

    std::string line;
    if (!WaitFor("bestmove", moveTime + SEARCH_GRACE_MS, &line))
    {
        if (m_stop)
        {
            result.error = "the engine is shutting down";
            return result;
        }
        // Ask it to stop and answer; kill it if it doesn't.
        if (!(Send("stop") && WaitFor("bestmove", STOP_TIMEOUT_MS, &line)))
        {
            result.error = "the chess engine did not answer in time";
            Log(LogLevel::Warning, "Chess engine timed out, restarting it");
            KillProcess();
            return result;
        }
    }

    std::istringstream words(line);
    std::string keyword, move;
    words >> keyword >> move;
    if (move.empty() || move == "(none)" || move == "0000")
    {
        result.error = "the chess engine has no move in this position";
        return result;
    }

    result.success = true;
    result.move = move;
    return result;
}

bool Engine::EnsureStarted(std::string& error)
{
    if (m_pid > 0)
    {
        // Still alive? waitpid() fails with ECHILD if the server ignores SIGCHLD, ask kill() then.
        const auto rc = waitpid(m_pid, nullptr, WNOHANG);
        if (rc == 0 || (rc < 0 && errno == ECHILD && kill(m_pid, 0) == 0))
            return true;
        if (rc > 0)
            m_pid = -1;
        Log(LogLevel::Warning, "Chess engine exited, restarting it");
        KillProcess();
    }
    return StartProcess(error);
}

bool Engine::StartProcess(std::string& error)
{
    std::string binary;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        binary = m_binary;
    }
    if (binary.empty())
    {
        error = "chess engine '" + m_config.path + "' not found";
        return false;
    }

    int toEngine[2], fromEngine[2];
    if (pipe2(toEngine, O_CLOEXEC) != 0)
    {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    if (pipe2(fromEngine, O_CLOEXEC) != 0)
    {
        error = std::string("pipe: ") + std::strerror(errno);
        close(toEngine[0]);
        close(toEngine[1]);
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, toEngine[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fromEngine[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    // The child gets default signal handling and an empty mask, whatever the server has set.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    for (int sig : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGCHLD, SIGQUIT, SIGUSR1, SIGUSR2})
        sigaddset(&defaults, sig);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    char* argv[] = {const_cast<char*>(binary.c_str()), nullptr};
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, binary.c_str(), &actions, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(toEngine[0]);
    close(fromEngine[1]);
    if (rc != 0)
    {
        close(toEngine[1]);
        close(fromEngine[0]);
        error = "could not start '" + binary + "': " + std::strerror(rc);
        Log(LogLevel::Error, error);
        return false;
    }

    m_pid = pid;
    m_toEngine = toEngine[1];
    m_fromEngine = fromEngine[0];
    m_readBuffer.clear();
    m_currentSkill = -1;
    m_lastGameId = 0;

    if (m_config.nice > 0)
        setpriority(PRIO_PROCESS, pid, std::min(getpriority(PRIO_PROCESS, 0) + m_config.nice, 19));

    const bool ok = Send("uci") && WaitFor("uciok", UCI_INIT_TIMEOUT_MS) &&
                    Send("setoption name Threads value " + std::to_string(std::max(m_config.threads, 1))) &&
                    Send("setoption name Hash value " + std::to_string(std::max(m_config.hashMb, 1))) &&
                    Send("isready") && WaitFor("readyok", UCI_INIT_TIMEOUT_MS);
    if (!ok)
    {
        error = "'" + binary + "' did not start as a UCI chess engine";
        Log(LogLevel::Error, error);
        KillProcess();
        return false;
    }

    Log(LogLevel::Notice, "Started chess engine '" + binary + "' (pid " + std::to_string(pid) + ")");
    return true;
}

void Engine::KillProcess()
{
    if (m_toEngine >= 0)
    {
        close(m_toEngine);
        m_toEngine = -1;
    }
    if (m_fromEngine >= 0)
    {
        close(m_fromEngine);
        m_fromEngine = -1;
    }
    if (m_pid > 0)
    {
        kill(m_pid, SIGKILL);
        // Fails with ECHILD if the server ignores SIGCHLD (the child is reaped for us then).
        while (waitpid(m_pid, nullptr, 0) < 0 && errno == EINTR) {}
        m_pid = -1;
    }
    m_readBuffer.clear();
}

bool Engine::Send(const std::string& lineIn)
{
    if (m_toEngine < 0)
        return false;

    const std::string line = lineIn + "\n";
    size_t written = 0;
    while (written < line.size())
    {
        const auto n = write(m_toEngine, line.data() + written, line.size() - written);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EPIPE)
            {
                // Clear the SIGPIPE that is now pending on this thread.
                sigset_t pipeSet;
                sigemptyset(&pipeSet);
                sigaddset(&pipeSet, SIGPIPE);
                struct timespec zero = {0, 0};
                sigtimedwait(&pipeSet, nullptr, &zero);
            }
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

bool Engine::ReadLine(std::string& line, int32_t timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true)
    {
        const auto newline = m_readBuffer.find('\n');
        if (newline != std::string::npos)
        {
            line = m_readBuffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            m_readBuffer.erase(0, newline + 1);
            return true;
        }

        if (m_fromEngine < 0 || m_stop)
            return false;

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0)
            return false;

        // Wake up at least every 100ms to notice a shutdown.
        struct pollfd pfd = {m_fromEngine, POLLIN, 0};
        const int rc = poll(&pfd, 1, static_cast<int>(std::min<int64_t>(remaining, 100)));
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (rc == 0)
            continue;

        char buffer[4096];
        const auto n = read(m_fromEngine, buffer, sizeof(buffer));
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return false;
        }
        if (n == 0)
            return false; // EOF: the engine exited
        m_readBuffer.append(buffer, static_cast<size_t>(n));
        if (m_readBuffer.size() > 1024 * 1024)
            return false; // not a UCI engine
    }
}

bool Engine::WaitFor(const std::string& prefix, int32_t timeoutMs, std::string* lineOut)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string line;
    while (true)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || !ReadLine(line, static_cast<int32_t>(remaining)))
            return false;
        if (line.compare(0, prefix.size(), prefix) == 0 &&
            (line.size() == prefix.size() || line[prefix.size()] == ' '))
        {
            if (lineOut)
                *lineOut = line;
            return true;
        }
    }
}

}
