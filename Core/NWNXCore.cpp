#include "nwnx.hpp"
#include "NWNXCore.hpp"
#include "EngineLogFiles.hpp"

#include "API/CAppManager.hpp"
#include "API/CExoString.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CNWSModule.hpp"
#include "API/CExoLinkedListInternal.hpp"
#include "API/CExoLinkedListNode.hpp"
#include "API/CExoResMan.hpp"
#include "API/CExoBase.hpp"
#include "API/CExoAliasList.hpp"
#include "API/CVirtualMachine.hpp"
#include "API/CExoStringList.hpp"
#include "API/CScriptCompiler.hpp"
#include "Log/Json.hpp"
#include "Log/Rotation.hpp"
#include "Log/ScriptContext.hpp"

#include <csignal>
#include <regex>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <sstream>
#include <dlfcn.h>
#include <atomic>
#include <pthread.h>
#include <execinfo.h>
#include <strings.h>

using namespace NWNXLib;
using namespace NWNXLib::API;

static uint32_t s_PostFixVersion;

static std::string NWNXVersion()
{
    return std::to_string(NWNX_TARGET_NWN_BUILD) + "." + std::to_string(NWNX_TARGET_NWN_BUILD_REVISION) + "-" +
           std::to_string(NWNX_TARGET_NWN_BUILD_POSTFIX);
}

static void (*nwn_crash_handler)(int);

// The thread currently reporting a crash, 0 when none is. A glibc pthread_t is the address of the
// thread's descriptor, so 0 never names a live thread.
static std::atomic<pthread_t> s_crashReportingThread{0};
static_assert(std::atomic<pthread_t>::is_always_lock_free, "the crash handler needs a lock-free atomic");

extern "C" void nwnx_signal_handler(int sig)
{
    const pthread_t self = pthread_self();
    pthread_t owner = 0;
    if (!s_crashReportingThread.compare_exchange_strong(owner, self))
    {
        // The reporting thread faulted again: its report is what faulted, and on the same fault it
        // would never stop re-entering, so terminate.
        if (pthread_equal(owner, self))
        {
            std::signal(SIGABRT, SIG_DFL);
            std::abort();
        }

        // Another thread is already reporting: let it finish, it ends the process.
        for (;;)
            pause();
    }

    const char *err;
    switch (sig)
    {
        case SIGABRT:  err = "Program aborted";          break;
        case SIGFPE:   err = "Floating point exception"; break;
        case SIGSEGV:  err = "Segmentation fault";       break;
        case SIGILL:   err = "Illegal instruction";      break;
        default:       err = "Unknown error";            break;
    }

    // Queued lines first; the crash output below is then written directly.
    Log::EmergencyFlush();

    const bool text = Log::GetFormat() == Log::Format::Text;
    if (text)
    {
        std::fprintf(stdout, " NWNX Signal Handler:\n"
            "==============================================================\n"
            " NWNX %d.%d-%d (%s) has crashed. Fatal error: %s (%d).\n"
            " Please file a bug at https://github.com/nwnxee/unified/issues\n"
            "==============================================================\n",
            NWNX_TARGET_NWN_BUILD, NWNX_TARGET_NWN_BUILD_REVISION, NWNX_TARGET_NWN_BUILD_POSTFIX, NWNX_BUILD_SHA, err, sig);

        if (s_PostFixVersion != NWNX_TARGET_NWN_BUILD_POSTFIX)
        {
            std::fprintf(stdout, " Postfix Version Mismatch: Expected: %d, Got: %d\n"
                    " This may have been the cause of this crash.\n"
                    "==============================================================\n",
                    NWNX_TARGET_NWN_BUILD_POSTFIX, s_PostFixVersion);
        }
    }

    // stdout is fully buffered when it is not a terminal (under Docker, for one): flush the header now,
    // so it survives if building the stack trace faults and the re-entry guard aborts.
    std::fflush(stdout);

    const std::string stackTrace = NWNXLib::Platform::GetStackTrace(20);
    if (text)
    {
        std::fputs(stackTrace.c_str(), stdout);
        std::fflush(stdout);
    }

    // The sinks that use JSON get one record instead of the text.
    Log::Record record(Log::Channel::SEV_FATAL, PLUGIN_NAME, __FILE__, __LINE__);
    record.Stream() << "NWNX has crashed";
    record.With("signal", sig).With("reason", err).With("version", NWNXVersion()).With("sha", NWNX_BUILD_SHA);
    if (s_PostFixVersion != NWNX_TARGET_NWN_BUILD_POSTFIX)
        record.With("postfix_expected", NWNX_TARGET_NWN_BUILD_POSTFIX).With("postfix", s_PostFixVersion);
    record.WithStack("stack", stackTrace).WriteJson();

    if (Config::Get<bool>("BASE_GAME_CRASH_HANDLER", false))
    {
        nwn_crash_handler(sig);
    }
    else
    {
        std::signal(SIGABRT, NULL);
        std::abort();
    }
}

// Don't allow the -quite flag to close stdout/stderr, we print important info there.
extern "C" FILE *freopen64(const char *filename, const char *mode, FILE *stream)
{
    if ((stream == stdout || stream == stderr) && !strcmp(filename, "/dev/null"))
    {
        if (stream == stdout)
        {
            std::puts("NWNX overriding -quiet flag. Always keep an eye on stdout.\n"
                      "Server will continue in non-interactive mode, but with full output.\n");
        }
        return stream;
    }

    using Type = FILE*(*)(const char*,const char*,FILE*);
    static Type real;
    if (!real)
        real = (Type)dlsym(RTLD_NEXT, "freopen64");
    return real(filename, mode, stream);
}


namespace {

void InitCrashHandlers()
{
    // The first backtrace() loads libgcc_s, which needs far more stack than a warm call. Make that
    // call here, so the handler does not have to make it on a small signal stack.
    void* warmUpFrame[1];
    backtrace(warmUpFrame, 1);

    nwn_crash_handler = std::signal(SIGABRT, nwnx_signal_handler);
    std::signal(SIGFPE,  nwnx_signal_handler);
    std::signal(SIGSEGV, nwnx_signal_handler);
    std::signal(SIGILL, nwnx_signal_handler);
}
void RestoreCrashHandlers()
{
    std::signal(SIGABRT, nwn_crash_handler);
    std::signal(SIGFPE,  nwn_crash_handler);
    std::signal(SIGSEGV, nwn_crash_handler);
    std::signal(SIGILL, nwn_crash_handler);
}

// Both engine files, for logreopen and the reopen signal. Returns whether either was reopened.
bool ReopenEngineLogs()
{
    const bool engine = Core::EngineLogFiles::Reopen(Core::EngineLogFiles::File::Engine);
    return Core::EngineLogFiles::Reopen(Core::EngineLogFiles::File::Server) || engine;
}

}

// TODO: Remove and allow auto-init post-load
namespace NWNXLib::POS { void InitializeHooks(); }
namespace NWNXLib::Tasks {
    void StartAsyncWorkers();
    void StopAsyncWorkers();
}


namespace Core {

static NWNXCore s_core;
NWNXCore* g_core = nullptr; // Used to access the core class in hook or event handlers.
bool g_CoreShuttingDown = false;

NWNXCore::NWNXCore()
    : m_ScriptChunkRecursion(0)
{
    g_core = this;
    CleanupPreload();
    InitialSetupLogging();

    // NOTE: We should do the version check here, but the global in the binary hasn't been initialised yet at this point.
    // This will be fixed in a future release of NWNX:EE. For now, the version check will happen *too late* - we may
    // crash before the version check happens.
    if (Log::GetFormat() == Log::Format::Text)
        std::printf("Starting NWNX %d.%d-%d [%s]\n", NWNX_TARGET_NWN_BUILD, NWNX_TARGET_NWN_BUILD_REVISION, NWNX_TARGET_NWN_BUILD_POSTFIX, NWNX_BUILD_SHA);
    Log::Record banner(Log::Channel::SEV_NOTICE, PLUGIN_NAME, __FILE__, __LINE__);
    banner.Stream() << "Starting NWNX";
    banner.With("version", NWNXVersion()).With("sha", NWNX_BUILD_SHA).WriteJson();

    // Initialise export table. New plugin code should endeavour to use direct linking
    // for hook naming, but these might help if you want to target a overloaded function.
    NWNXLib::API::Functions::Initialize();

    m_createServerHook = Hooks::HookFunction(&CAppManager::CreateServer, &CreateServerHandler);
}

NWNXCore::~NWNXCore()
{
    Shutdown();
}

void NWNXCore::CleanupPreload()
{
    const auto* preload = std::getenv("LD_PRELOAD");
    const std::regex regex("(([^: ]+)?NWNX_[^: ]+)");

    std::string newPreload = preload;
    while (std::regex_search(newPreload, regex))
    {
        newPreload = std::regex_replace(newPreload, regex, "");
    }

    setenv("LD_PRELOAD", newPreload.c_str(), true);
}

std::unique_ptr<Services::ServiceList> NWNXCore::ConstructCoreServices()
{
    using namespace NWNXLib::Services;
    std::unique_ptr<ServiceList> services = std::make_unique<ServiceList>();

    services->m_metrics = std::make_unique<Services::Metrics>();

    return services;
}

std::unique_ptr<Services::ProxyServiceList> NWNXCore::ConstructProxyServices(const std::string& plugin)
{
    std::unique_ptr<Services::ProxyServiceList> proxyServices = std::make_unique<Services::ProxyServiceList>();

    proxyServices->m_metrics = std::make_unique<Services::MetricsProxy>(*m_services->m_metrics, plugin);

    ConfigureLogLevel(plugin);

    return proxyServices;
}

void NWNXCore::ConfigureLogLevel(const std::string& plugin)
{
    // Setup the log level. We do this first by checking if NWNX_<PLUGIN>_LOG_LEVEL is set.
    auto logLevel = Config::Get<uint32_t>("LOG_LEVEL", plugin);

    if (!logLevel)
    {
        // If it is not, then we check if NWNX_CORE_LOG_LEVEL is set.
        logLevel = Config::Get<uint32_t>("LOG_LEVEL");
    }

    if (logLevel)
    {
        Log::SetLogLevel(plugin.c_str(), static_cast<Log::Channel::Enum>(*logLevel));
    }
}


void NWNXCore::InitialSetupLogging()
{
    ConfigureLogLevel(NWNX_CORE_PLUGIN_NAME);
    // We need to set the NWNXLib log level (separate from Core now) to match the core log level.
    Log::SetLogLevel("NWNXLib", Log::GetLogLevel(NWNX_CORE_PLUGIN_NAME));
    Log::SetPrintTimestamp(Config::Get<bool>("LOG_TIMESTAMP", true));
    Log::SetPrintDate(Config::Get<bool>("LOG_DATE", false));
    Log::SetPrintPlugin(Config::Get<bool>("LOG_PLUGIN", true));
    Log::SetPrintSource(Config::Get<bool>("LOG_SOURCE", true));
    Log::SetColorOutput(Config::Get<bool>("LOG_COLOR", true));
    Log::SetForceColor(Config::Get<bool>("LOG_FORCE_COLOR", false));
    const auto logFile = Config::Get<std::string>("LOG_FILE_PATH", "");
    Log::SetLogFile(logFile);
    Log::FileRotation rotation;
    rotation.keep = Config::Get<int>("LOG_FILE_KEEP", int(rotation.keep));
    // Before the file is first opened, so the new run starts a new file.
    std::string keptFile;
    if (!logFile.empty() && Config::Get<bool>("LOG_FILE_KEEP_PREVIOUS", false))
        keptFile = Log::Rotation::KeepPrevious(logFile, rotation.keep);

    // Problems are logged once the format is set, so they come out in it.
    std::vector<std::string> problems;
    auto getFormat = [&problems](const char* key, Log::Format def)
    {
        const auto value = Config::Get<std::string>(key);
        if (!value)
            return def;
        if (*value == "json")
            return Log::Format::Json;
        if (*value != "text")
            problems.push_back(std::string("NWNX_CORE_") + key + "='" + *value + "' is not text or json");
        return *value == "text" ? Log::Format::Text : def;
    };
    const auto format = getFormat("LOG_FORMAT", Log::Format::Text);
    Log::SetFormat(format);
    const auto fileFormat = getFormat("LOG_FILE_FORMAT", format);
    Log::SetFileFormat(fileFormat);
    Log::SetTextFields(Config::Get<bool>("LOG_TEXT_FIELDS", false));

    Log::Json::Options options;
    const auto level = Config::Get<std::string>("LOG_JSON_LEVEL", "number");
    options.levelLabel = level == "label";
    if (level != "number" && level != "label")
        problems.push_back("NWNX_CORE_LOG_JSON_LEVEL='" + level + "' is not number or label");
    const auto encoding = Config::Get<std::string>("LOG_JSON_ENCODING", "auto");
    if (encoding == "locale")
        options.encoding = Log::Json::Encoding::Locale;
    else if (encoding == "utf8")
        options.encoding = Log::Json::Encoding::Utf8;
    else if (encoding != "auto")
        problems.push_back("NWNX_CORE_LOG_JSON_ENCODING='" + encoding + "' is not auto, locale or utf8");
    options.stripColors = Config::Get<bool>("LOG_JSON_STRIP_COLORS", true);
    // Floors keep room for the fixed keys of a record.
    options.maxValue = std::max(Config::Get<uint32_t>("LOG_JSON_MAX_VALUE", 8 * 1024), 64u);
    options.maxLine = std::max(Config::Get<uint32_t>("LOG_JSON_MAX_LINE", 64 * 1024), 1024u);
    Log::SetJsonOptions(options);

    if (auto json = Config::Get<std::string>("LOG_JSON_STATIC"))
    {
        std::vector<std::string> renamed;
        const std::string error = Log::SetJsonStatic(*json, renamed);
        if (!error.empty())
            problems.push_back("NWNX_CORE_LOG_JSON_STATIC is not a JSON object: " + error);
        for (const auto& key : renamed)
            problems.push_back("NWNX_CORE_LOG_JSON_STATIC: '" + key + "' is a reserved key, renamed to '_" + key + "'");
    }

    if (auto locale = Config::Get<std::string>("LOCALE"))
    {
        String::SetDefaultLocale(*locale);
    }
    String::SetBestFit(Config::Get<bool>("ENCODING_BEST_FIT", false));

    // The running script's context on records made during a script; "none" turns it off.
    uint32_t contextKeys = Log::ContextKey::Default;
    if (auto context = Config::Get<std::string>("LOG_CONTEXT"))
    {
        static constexpr std::pair<std::string_view, uint32_t> keys[] = {
            {"script", Log::ContextKey::Script}, {"oid", Log::ContextKey::Oid}, {"tag", Log::ContextKey::Tag},
            {"area", Log::ContextKey::Area}, {"pc", Log::ContextKey::Pc}, {"player", Log::ContextKey::Player}};
        contextKeys = 0;
        for (const auto& name : String::Split(*context == "none" ? std::string() : *context, ','))
        {
            auto key = std::find_if(std::begin(keys), std::end(keys), [&](const auto& k) { return name == k.first; });
            if (key != std::end(keys))
                contextKeys |= key->second;
            else
                problems.push_back("NWNX_CORE_LOG_CONTEXT: unknown key '" + name + "'");
        }
    }
    Log::SetScriptContextKeys(contextKeys);
    Log::SetContextSource(contextKeys ? &Log::AddScriptContext : nullptr);

    const auto rotateSize = Config::Get<std::string>("LOG_FILE_ROTATE_SIZE", "0");
    if (auto size = Log::Rotation::ParseSize(rotateSize))
        rotation.size = *size;
    else
        problems.push_back("NWNX_CORE_LOG_FILE_ROTATE_SIZE='" + rotateSize + "' is not bytes with an optional K, M or G");
    const auto rotateTime = Config::Get<std::string>("LOG_FILE_ROTATE_TIME", "off");
    if (auto period = Log::Rotation::ParsePeriod(rotateTime))
        rotation.period = *period;
    else
        problems.push_back("NWNX_CORE_LOG_FILE_ROTATE_TIME='" + rotateTime + "' is not hourly, daily or off");
    Log::SetFileRotation(rotation);

    // Installed only when asked for: by default SIGHUP still ends the server, as it always has.
    const auto reopenSignal = Config::Get<std::string>("LOG_REOPEN_SIGNAL", "off");
    const int sig = !strcasecmp(reopenSignal.c_str(), "HUP") ? SIGHUP : !strcasecmp(reopenSignal.c_str(), "USR1") ? SIGUSR1
                  : !strcasecmp(reopenSignal.c_str(), "USR2") ? SIGUSR2 : 0;
    if (sig)
    {
        struct sigaction action = {};
        action.sa_handler = [](int) { Log::RequestReopen(); };
        action.sa_flags = SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(sig, &action, nullptr);
    }
    else if (strcasecmp(reopenSignal.c_str(), "off"))
        problems.push_back("NWNX_CORE_LOG_REOPEN_SIGNAL='" + reopenSignal + "' is not off, HUP, USR1 or USR2");

    const auto async = Config::Get<std::string>("LOG_ASYNC", "auto");
    if (async != "auto" && async != "true" && async != "false")
        problems.push_back("NWNX_CORE_LOG_ASYNC='" + async + "' is not auto, true or false");
    const bool anyJson = format == Log::Format::Json || (!logFile.empty() && fileFormat == Log::Format::Json);
    if (async == "true" || (async != "false" && anyJson))
    {
        Log::AsyncOptions asyncOptions;
        asyncOptions.flushMs = Config::Get<uint32_t>("LOG_ASYNC_FLUSH_MS", uint32_t(asyncOptions.flushMs));
        // A floor keeps room for a few long records.
        asyncOptions.maxBytes = std::max(Config::Get<uint32_t>("LOG_ASYNC_MAX_BYTES", uint32_t(asyncOptions.maxBytes)), 256u * 1024);
        const auto full = Config::Get<std::string>("LOG_ASYNC_FULL", "block");
        asyncOptions.dropWhenFull = full == "drop";
        if (full != "block" && full != "drop")
            problems.push_back("NWNX_CORE_LOG_ASYNC_FULL='" + full + "' is not block or drop");
        Log::StartAsync(asyncOptions);
    }

    for (const auto& problem : problems)
        LOG_WARNING("%s", problem);
    if (!keptFile.empty())
        LOG_NOTICE("Kept the previous run's log as '%s'", keptFile);
}

void NWNXCore::InitialSetupHooks()
{
    m_vmPlaySoundHook               = Hooks::HookFunction(&CNWSVirtualMachineCommands::ExecuteCommandPlaySound, &PlaySoundHandler, Hooks::Order::Final);
    m_nwnxFunctionManagementHook    = Hooks::HookFunction(&CNWSVirtualMachineCommands::ExecuteCommandNWNXFunctionManagement, &NWNXFunctionManagementHandler, Hooks::Order::Final);
    m_destroyServerHook             = Hooks::HookFunction(&CAppManager::DestroyServer, &DestroyServerHandler, Hooks::Order::Final);
    m_mainLoopInternalHook          = Hooks::HookFunction(&CServerExoAppInternal::MainLoop, &MainLoopInternalHandler, Hooks::Order::Final);

    POS::InitializeHooks();

    static Hooks::Hook loadModuleInProgressHook = Hooks::HookFunction(&CNWSModule::LoadModuleInProgress,
            +[](CNWSModule *pModule, int32_t nAreasLoaded, int32_t nAreasToLoad) -> uint32_t
            {
                int index = nAreasLoaded;
                auto *node = pModule->m_lstModuleArea.m_pcExoLinkedListInternal->pHead;
                while (node && index)
                {
                    node = node->pNext;
                    index--;
                }

                if (node)
                {
                    auto *resref = (CResRef*)node->pObject;
                    LOG_DEBUG("(%i/%i) Trying to load area with resref: %s", nAreasLoaded + 1,  nAreasToLoad, *resref);
                }

                return loadModuleInProgressHook->CallOriginal<uint32_t>(pModule, nAreasLoaded, nAreasToLoad);
            }, Hooks::Order::Earliest);

    static Hooks::Hook loadModuleFinishHook = Hooks::HookFunction(
            &CNWSModule::LoadModuleFinish,
            +[](CNWSModule *pModule) -> uint32_t
            {
                MessageBus::Broadcast("NWNX_CORE_SIGNAL", { "ON_MODULE_LOAD_FINISH" });
                return loadModuleFinishHook->CallOriginal<uint32_t>(pModule);
            }, Hooks::Order::Earliest);

    if (!Config::Get<bool>("ALLOW_NWNX_FUNCTIONS_IN_EXECUTE_SCRIPT_CHUNK", false))
    {
        static Hooks::Hook runScriptChunkHook = Hooks::HookFunction(
                &CVirtualMachine::RunScriptChunk,
                +[](CVirtualMachine *pVirtualMachine, const CExoString& sScriptChunk, ObjectID oid, int32_t bOidValid, int32_t bWrapIntoMain) -> int32_t
                {
                    g_core->m_ScriptChunkRecursion += 1;
                    auto retVal = runScriptChunkHook->CallOriginal<int32_t>(pVirtualMachine, sScriptChunk, oid, bOidValid, bWrapIntoMain);
                    g_core->m_ScriptChunkRecursion -= 1;
                    return retVal;
                }, Hooks::Order::VeryEarly);

        static Hooks::Hook runScriptSituationHook = Hooks::HookFunction(
                &CVirtualMachine::RunScriptSituation,
                +[](CVirtualMachine *pVirtualMachine, void * pScriptSituation, OBJECT_ID oid, BOOL bOidValid) -> BOOL
                {
                    auto *pVirtualMachineScript = (CVirtualMachineScript*)pScriptSituation;
                    bool isScriptChunk = !pVirtualMachineScript->m_sScriptChunk.IsEmpty();

                    if (isScriptChunk)
                        g_core->m_ScriptChunkRecursion += 1;

                    auto retVal = runScriptSituationHook->CallOriginal<BOOL>(pVirtualMachine, pScriptSituation, oid, bOidValid);

                    if (isScriptChunk)
                        g_core->m_ScriptChunkRecursion -= 1;

                    return retVal;
                }, Hooks::Order::VeryEarly);
    }
}

void NWNXCore::InitialVersionCheck()
{
    CExoString *pBuildNumber = Globals::BuildNumber();
    CExoString *pBuildRevision = Globals::BuildRevision();
    CExoString *pBuildPostfix = Globals::BuildPostfix();

    if (pBuildNumber && pBuildRevision && pBuildPostfix)
    {
        const uint32_t version = std::stoul(pBuildNumber->m_sString);
        const uint32_t revision = std::stoul(pBuildRevision->m_sString);
        const uint32_t postfix = std::stoul(pBuildPostfix->m_sString);

        s_PostFixVersion = postfix;

        const bool text = Log::GetFormat() == Log::Format::Text;
        if (version != NWNX_TARGET_NWN_BUILD || revision != NWNX_TARGET_NWN_BUILD_REVISION)
        {
            if (text)
            {
                std::fprintf(stdout, "NWNX: Expected build %u.%u, got build %u.%u.\n", NWNX_TARGET_NWN_BUILD, NWNX_TARGET_NWN_BUILD_REVISION, version, revision);
                std::fprintf(stdout, "NWNX: Will terminate. Please use the correct NWNX build for your game version.\n");
                std::fflush(stdout);
            }
            Log::Record record(Log::Channel::SEV_FATAL, PLUGIN_NAME, __FILE__, __LINE__);
            record.Stream() << "Wrong game build; use the NWNX build for your game version";
            record.With("expected", std::to_string(NWNX_TARGET_NWN_BUILD) + "." + std::to_string(NWNX_TARGET_NWN_BUILD_REVISION))
                  .With("got", std::to_string(version) + "." + std::to_string(revision)).WriteJson();
            std::exit(1);
        }

        if (postfix != NWNX_TARGET_NWN_BUILD_POSTFIX)
        {
            if (text)
            {
                std::fprintf(stdout, "NWNX: WARNING: POSTFIX VERSION MISMATCH: EXPECTED: %d, GOT: %d\n", NWNX_TARGET_NWN_BUILD_POSTFIX, postfix);
                std::fprintf(stdout, "NWNX:          THE NWNX API MAY NOT BE UP TO DATE AND CRASH YOUR SERVER.\n");
                std::fflush(stdout);
            }
            Log::Record record(Log::Channel::SEV_WARNING, PLUGIN_NAME, __FILE__, __LINE__);
            record.Stream() << "Postfix version mismatch; the NWNX API may not be up to date and crash your server";
            record.With("expected", NWNX_TARGET_NWN_BUILD_POSTFIX).With("got", postfix).WriteJson();
        }
    }
    else
    {
        if (Log::GetFormat() == Log::Format::Text)
        {
            std::fprintf(stdout, "NWNX: Could not determine build version.\n");
            std::fflush(stdout);
        }
        Log::Record record(Log::Channel::SEV_FATAL, PLUGIN_NAME, __FILE__, __LINE__);
        record.Stream() << "Could not determine the game build";
        record.WriteJson();
        std::abort();
    }
}

void NWNXCore::InitialSetupPlugins()
{
    constexpr static const char* pluginPrefix = NWNX_PLUGIN_PREFIX;
    const std::string prefix = pluginPrefix;

    char cwd[PATH_MAX];
    ASSERT(getcwd(cwd, sizeof(cwd)) != nullptr);

    const auto pluginDir = Config::Get<std::string>("LOAD_PATH", cwd);
    const bool skipAllPlugins = Config::Get<bool>("SKIP_ALL", false);

    LOG_INFO("Loading plugins from: %s", pluginDir);

    std::vector<std::string> files;
    if (auto dir = opendir(pluginDir.c_str()))
    {
        while (auto entry = readdir(dir))
        {
            if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_REG || entry->d_type == DT_LNK)
            {
                files.emplace_back(entry->d_name);
            }
        }
        closedir(dir);
    }
    // Sort by file name, so at least plugins are loaded in deterministic order.
    std::sort(std::begin(files), std::end(files));

    for (auto& dynamicLibrary : files)
    {
        const std::string& pluginName = dynamicLibrary;
        const std::string pluginNameWithoutExtension = String::Basename(pluginName);

        if (pluginNameWithoutExtension.compare(0, prefix.size(), prefix) != 0)
        {
            continue; // Not a plugin.
        }

        if (pluginNameWithoutExtension == "NWNX_Experimental" && !Config::Get<bool>("LOAD_EXPERIMENTAL_PLUGIN", false))
        {
            continue;
        }

        auto services = ConstructProxyServices(pluginNameWithoutExtension);

        // Always load core.
        if (pluginNameWithoutExtension != NWNX_CORE_PLUGIN_NAME && Config::Get<bool>("SKIP", (bool)skipAllPlugins, pluginNameWithoutExtension))
        {
            LOG_INFO("Skipping plugin %s due to configuration.", pluginNameWithoutExtension);
            continue;
        }
        Plugin::Load(pluginDir + "/" + pluginName, std::move(services));
    }
}

void NWNXCore::InitialSetupResourceDirectories()
{
    static Hooks::Hook s_SetupDefaultSearchPathHook = Hooks::HookFunction(&CExoResMan::SetupDefaultSearchPath,
    +[](CExoResMan *pThis) -> void
    {
        s_SetupDefaultSearchPathHook->CallOriginal<void>(pThis);

        if (g_CoreShuttingDown)
            return;

        auto nwnxResDirPath = Config::Get<std::string>("NWNX_RESOURCE_DIRECTORY_PATH", Globals::ExoBase()->m_sUserDirectory.CStr() + std::string("/nwnx"));
        auto nwnxResDirPriority = Config::Get<int32_t>("NWNX_RESOURCE_DIRECTORY_PRIORITY", 70000000);

        std::unordered_map<std::string, std::pair<std::string, int32_t>> resourceDirectories;
        resourceDirectories.emplace("NWNX", std::make_pair(nwnxResDirPath, nwnxResDirPriority));

        if (auto customResmanDefinition = Config::Get<std::string>("CUSTOM_RESMAN_DEFINITION"))
        {
            std::string crdPath = *customResmanDefinition;
            FILE* file = std::fopen(crdPath.c_str(), "r");

            if (file)
            {
                LOG_INFO("Custom Resman Definition File: %s", crdPath);

                char line[640];
                char alias[64];
                char path[512];
                int32_t priority;

                while (std::fgets(line, 640, file))
                {
                    if (sscanf(line, "%s %s %i", alias, path, &priority) == 3)
                    {
                        resourceDirectories.try_emplace(alias, std::make_pair(path, priority));
                    }
                    else
                    {
                        std::string errorLine = std::string(line);
                        LOG_WARNING("Invalid Custom Resman Definition Line: %s", String::Trim(errorLine));
                    }
                }

                std::fclose(file);
            }
            else
                LOG_ERROR("Failed to open Custom Resman Definition File: %s", crdPath);
        }

        for (const auto& resDir : resourceDirectories)
        {
            CExoString alias = CExoString(resDir.first + ":");
            CExoString path = CExoString(resDir.second.first);

            if (Globals::ExoBase()->m_pcExoAliasList->GetAliasPath(alias).IsEmpty())
            {
                LOG_INFO("Setting up Resource Directory: %s%s (Priority: %i)", alias, path, resDir.second.second);

                g_core->m_CustomResourceDirectoryAliases.emplace_back(resDir.first);

                Globals::ExoBase()->m_pcExoAliasList->Add(resDir.first, path);
                Globals::ExoResMan()->CreateDirectory(alias);
                Globals::ExoResMan()->AddResourceDirectory(alias, resDir.second.second, true);
            }
            else
                LOG_WARNING("Resource Directory with alias '%s' already exists. Please use nwn.ini to redefine base game resource directories.", alias);
        }

    }, Hooks::Order::Early);
}

void NWNXCore::InitialSetupCommands()
{
    Commands::Register("runscript", [](std::string&, std::string& args)
    {
        if (Globals::AppManager()->m_pServerExoApp->GetServerMode() != 2)
            return;

        if (!args.empty())
        {
            LOG_INFO("Executing console command: 'runscript' with args: %s", args);
            Utils::ExecuteScript(args, 0);
        }
    });

    Commands::Register("eval", [](std::string&, std::string& args)
    {
        if (Globals::AppManager()->m_pServerExoApp->GetServerMode() != 2)
            return;

        if (!args.empty())
        {
            LOG_INFO("Executing console command: 'eval' with args: %s", args);
            bool bWrapIntoMain = args.find("void main()") == std::string::npos;
            if (Globals::VirtualMachine()->RunScriptChunk(args, 0, true, bWrapIntoMain))
            {
                LOG_ERROR("Failed to run console command 'eval' with error: %s", Globals::VirtualMachine()->m_pJitCompiler->m_sCapturedError.CStr());
            }
        }
    });

    Commands::Register("evalx", [](std::string&, std::string& args)
    {
        if (Globals::AppManager()->m_pServerExoApp->GetServerMode() != 2)
            return;

        static std::string nwnxHeaders;
        if (nwnxHeaders.empty())
        {
            if (auto *pList = Globals::ExoResMan()->GetResOfType(Constants::ResRefType::NSS, false))
            {
                std::regex rgx("nwnx_[a-z]*");
                for (int i = 0; i < pList->m_nCount; i++)
                {
                    if (std::regex_match(pList->m_pStrings[i]->CStr(), rgx))
                        nwnxHeaders += "#include \"" + std::string(pList->m_pStrings[i]->CStr()) + "\" ";
                }
            }
        }

        if (!args.empty())
        {
            LOG_INFO("Executing console command: 'evalx' with args: %s", args);
            std::string script = nwnxHeaders + (args.find("void main()") == std::string::npos ? "void main() { " + args + " }" : args);
            if (Globals::VirtualMachine()->RunScriptChunk(script, 0, true, false))
            {
                LOG_ERROR("Failed to run console command 'evalx' with error: %s", Globals::VirtualMachine()->m_pJitCompiler->m_sCapturedError.CStr());
            }
        }
    });

    Commands::Register("loglevel", [](std::string&, std::string& args)
    {
        if (!args.empty())
        {
            size_t space = args.find_first_of(' ');
            std::string pluginName = args.substr(0, space);
            std::string level = args.substr(space + 1);

            auto* plugin = Plugin::Find("NWNX_" + pluginName);

            if (plugin)
            {
                if (auto logLevel = String::FromString<uint32_t>(level))
                {
                    LOG_INFO("Setting log level of plugin '%s' to '%u'", plugin->GetName(), *logLevel);
                    Log::SetLogLevel(plugin->GetName().c_str(), static_cast<Log::Channel::Enum>(*logLevel));
                }
                else if (level == pluginName) // no level given.
                {
                    LOG_INFO("Log level for %s is %u", plugin->GetName(), Log::GetLogLevel(plugin->GetName().c_str()));
                }
                else
                {
                    LOG_INFO("'%s' is not a valid log level", level);
                }
            }
            else
            {
                LOG_INFO("Plugin '%s' is not loaded", pluginName);
            }
        }
    });

    Commands::Register("logformat", [](std::string&, std::string& args)
    {
        if (args.find("timestamp") != std::string::npos)
            Log::SetPrintTimestamp(args.find("notimestamp") == std::string::npos);
        if (args.find("date") != std::string::npos)
            Log::SetPrintDate(args.find("nodate") == std::string::npos);
        if (args.find("plugin") != std::string::npos)
            Log::SetPrintPlugin(args.find("noplugin") == std::string::npos);
        if (args.find("source") != std::string::npos)
            Log::SetPrintSource(args.find("nosource") == std::string::npos);
        if (args.find("color") != std::string::npos)
            Log::SetColorOutput(args.find("nocolor") == std::string::npos);
        if (args.find("force") != std::string::npos)
            Log::SetForceColor(args.find("noforce") == std::string::npos);
        if (args.find("json") != std::string::npos)
            Log::SetFormat(Log::Format::Json);
        else if (args.find("text") != std::string::npos)
            Log::SetFormat(Log::Format::Text);
        LOG_INFO("Log format updated: Timestamp:%s Date:%s Plugin:%s Source:%s Color:%s Force:%s.",
                 Log::GetPrintTimestamp(), Log::GetPrintDate(), Log::GetPrintPlugin(),
                 Log::GetPrintSource(), Log::GetColorOutput(), Log::GetForceColor());
    });

    Commands::Register("logrotate", [](std::string&, std::string& args)
    {
        const bool all = args == "all";
        if (args == "nwnx" || (all && !Log::GetLogFile().empty()))
            Log::RotateFile();
        if (args == "engine" || all)
            EngineLogFiles::Rotate(EngineLogFiles::File::Engine);
        if (args == "server" || all)
            EngineLogFiles::Rotate(EngineLogFiles::File::Server);
        if (args != "nwnx" && args != "engine" && args != "server" && !all)
            LOG_INFO("Usage: logrotate nwnx|engine|server|all");
    });

    Commands::Register("logreopen", [](std::string&, std::string&)
    {
        const bool nwnx = Log::ReopenFile();
        if (!ReopenEngineLogs() && !nwnx)
            LOG_INFO("No log file was moved: nothing to reopen");
    });

}


void NWNXCore::UnloadServices()
{
    m_coreServices.reset();
    m_services.reset();
}

void NWNXCore::Shutdown()
{
    if (g_core)
    {
        Plugin::UnloadAll();
        UnloadServices();
        // Drains the log queue; anything logged later is written directly.
        Log::Shutdown();
        Tasks::StopAsyncWorkers();
        g_core = nullptr;
        if (Config::Get<bool>("HARD_EXIT", false))
            exit(0);
    }
}

void NWNXCore::CreateServerHandler(CAppManager* app)
{
    InitCrashHandlers();
    g_core->InitialVersionCheck();

    g_core->m_services = g_core->ConstructCoreServices();
    g_core->m_coreServices = g_core->ConstructProxyServices(NWNX_CORE_PLUGIN_NAME);

    if (Config::Get<bool>("SKIP", false))
    {
        LOG_NOTICE("Not loading NWNX due to configuration.");
    }
    else
    {
        LOG_NOTICE("Loading NWNX.");

        try
        {
            Tasks::StartAsyncWorkers();
            g_core->InitialSetupHooks();
            g_core->InitialSetupPlugins();
            g_core->InitialSetupResourceDirectories();
            g_core->InitialSetupCommands();
            MessageBus::Broadcast("NWNX_CORE_SIGNAL", { "ON_NWNX_LOADED" });
        }
        catch (const std::runtime_error& ex)
        {
            LOG_FATAL("The server encountered a fatal error '%s' during setup and must now terminate.", ex.what());
        }
    }

    g_core->m_createServerHook.reset();
    app->CreateServer();
}

void NWNXCore::DestroyServerHandler(CAppManager* app)
{
    g_CoreShuttingDown = true;

    if (auto shutdownScript = Config::Get<std::string>("SHUTDOWN_SCRIPT"))
    {
        if (Globals::AppManager()->m_pServerExoApp->GetServerMode() == 2)
        {
            LOG_NOTICE("Running module shutdown script: %s", *shutdownScript);
            Utils::ExecuteScript(*shutdownScript, 0);
        }
    }

    MessageBus::Broadcast("NWNX_CORE_SIGNAL", { "ON_DESTROY_SERVER" });

    g_core->m_destroyServerHook.reset();
    app->DestroyServer();

    MessageBus::Broadcast("NWNX_CORE_SIGNAL", { "ON_DESTROY_SERVER_AFTER" });

    g_core->Shutdown();

    RestoreCrashHandlers();
}

int32_t NWNXCore::MainLoopInternalHandler(CServerExoAppInternal *pServerExoAppInternal)
{
    g_core->m_services->m_metrics->Update();
    Tasks::ProcessMainThreadWork();
    Commands::RunScheduled();
    EngineLogFiles::Tick();
    if (Log::Tick())
        ReopenEngineLogs();

    return g_core->m_mainLoopInternalHook->CallOriginal<int32_t>(pServerExoAppInternal);
}

NWNX_EXPORT ArgumentStack PluginExists(ArgumentStack&& args)
{
    return Plugin::Find(args.extract<std::string>()) ? 1 : 0;
}

}
