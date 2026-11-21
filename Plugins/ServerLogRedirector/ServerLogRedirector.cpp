#include "nwnx.hpp"

#include "API/CExoDebugInternal.hpp"
#include "API/CNWSVirtualMachineCommands.hpp"
#include "API/CVirtualMachineCmdImplementer.hpp"
#include "API/Database.hpp"
#include "API/ExoLog.hpp"

#include <cstdarg>
#include <cstring>
#include <strings.h>
#include <utility>

using namespace NWNXLib;
using namespace NWNXLib::API;

static bool s_printString;

inline std::string TrimMessage(CExoString* message)
{
    std::string s = std::string(message->CStr());

    if (!s_printString)
    {
        // Eat the auto-added timestamp.
        auto idxOfBracket = s.find_first_of(']');
        if (idxOfBracket != std::string::npos)
            s.erase(0, idxOfBracket + 1);
    }

    return String::Trim(s);
}

static void HookServerLog()
{
    static Hooks::Hook s_WriteToLogFileHook = Hooks::HookFunction((void*)&CExoDebugInternal::WriteToLogFile,
        +[](CExoDebugInternal *pExoDebugInternal, CExoString* message) -> void
        {
            LOG_INFO("(Server) %s", TrimMessage(message));
            s_WriteToLogFileHook->CallOriginal<void>(pExoDebugInternal, message);
        }, Hooks::Order::VeryEarly);

    static Hooks::Hook s_ExecuteCommandPrintStringHook = Hooks::HookFunction((void*)&CNWSVirtualMachineCommands::ExecuteCommandPrintString,
        +[](CNWSVirtualMachineCommands *pVirtualMachineCommands, int32_t nCommandId, int32_t nParameters) -> int32_t
        {
            s_printString = true;
            auto retVal = s_ExecuteCommandPrintStringHook->CallOriginal<int32_t>(pVirtualMachineCommands, nCommandId, nParameters);
            s_printString = false;
            return retVal;
        }, Hooks::Order::VeryEarly);
}

// JSON mode: every ExoLog::Emit call becomes a record, enriched by markers the thin hooks below set.
namespace {

constexpr const char* s_categories[] = {
    "NWNX::Core", "NWNX::User", "User::NWScript", "User::Chat", "User::Info", "ASSERT", "Engine", "General",
    "Network", "ScriptVM", "NUI" };
constexpr uint32_t s_otherCategory = std::size(s_categories);

Hooks::Hook s_EmitHook;
uint32_t s_dropCategories;  // bit per category, s_otherCategory for unknown ones
bool s_engineFiles = true;

thread_local const char* t_vmCommand;
struct ScriptError { int32_t code; const char* message; };
thread_local const ScriptError* t_scriptError;
struct SqlError { const char* message; int code; const char* query; };
thread_local const SqlError* t_sqlError;
thread_local bool t_inEmit;

// Formats into a reused buffer; the engine gets it back through "%s", so its files keep their exact bytes.
void FormatMessage(std::string& out, const char* format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    out.resize(out.capacity());
    const int size = std::vsnprintf(out.data(), out.size() + 1, format, args);
    if (size > static_cast<int>(out.size()))
    {
        out.resize(static_cast<size_t>(size));
        std::vsnprintf(out.data(), out.size() + 1, format, copy);
    }
    va_end(copy);
    out.resize(size > 0 ? static_cast<size_t>(size) : 0);
}

Log::Channel::Enum ChannelOf(LogSeverity severity)
{
    switch (severity)
    {
        case LogSeverity_Debug:   return Log::Channel::SEV_DEBUG;
        case LogSeverity_Warning: return Log::Channel::SEV_WARNING;
        case LogSeverity_Error:   return Log::Channel::SEV_ERROR;
        default:                  return Log::Channel::SEV_INFO;
    }
}

void WriteRecord(LogSeverity severity, uint32_t category, const char* func, const char* file, int line, std::string_view message)
{
    if (t_scriptError)
        severity = LogSeverity_Error;
    const auto channel = ChannelOf(severity);
    if (static_cast<uint32_t>(channel) > Log::PluginLogLevel().load(std::memory_order_relaxed))
        return;

    // The VM passes "text\n"; a stack dump keeps its inner newlines in one record.
    while (!message.empty() && message.back() == '\n')
        message.remove_suffix(1);

    Log::Record record(channel, PLUGIN_NAME, file ? file : "", line);
    record.Stream() << message;
    record.Src("engine");
    if (category < s_otherCategory)
        record.Origin("cat", s_categories[category]);
    if (func)
        record.With("func", func);
    if (t_vmCommand)
        record.With("fn", t_vmCommand);
    if (t_scriptError)
    {
        std::string_view errorMessage = t_scriptError->message ? t_scriptError->message : "";
        if (errorMessage.empty())
        {
            // "Script error: <script> OID: ..., Tag: ..., ERROR: TOO MANY INSTRUCTIONS"
            const size_t error = message.rfind("ERROR: ");
            errorMessage = error != std::string_view::npos ? message.substr(error + 7) : message;
        }
        record.Err(errorMessage, t_scriptError->code);
    }
    if (t_sqlError)
    {
        record.Err(t_sqlError->message ? t_sqlError->message : "", t_sqlError->code);
        // The engine passes "(unknown)" when it has no query.
        if (t_sqlError->query && std::strcmp(t_sqlError->query, "(unknown)") != 0)
            record.With("query", t_sqlError->query);
    }
    record.Write();
}

void EmitHook(LogSeverity severity, LogCategory category, const char* func, const char* file, int line, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    // A nested call (from a log subscriber, say) only reaches the engine, as the engine itself does.
    if (t_inEmit)
    {
        std::string message;
        FormatMessage(message, format, args);
        va_end(args);
        if (s_engineFiles)
            s_EmitHook->CallOriginal<void>(severity, category, func, file, line, "%s", message.c_str());
        return;
    }

    thread_local std::string t_message;
    FormatMessage(t_message, format, args);
    va_end(args);

    t_inEmit = true;
    const uint32_t cat = std::min<uint32_t>(static_cast<uint32_t>(category), s_otherCategory);
    if (!(s_dropCategories & (1u << cat)))
        WriteRecord(severity, cat, func, file, line, t_message);
    if (s_engineFiles)
        s_EmitHook->CallOriginal<void>(severity, category, func, file, line, "%s", t_message.c_str());
    t_inEmit = false;

    if (t_message.capacity() > 64 * 1024)
        std::string().swap(t_message);
}

// The script commands that print: their name becomes the record's fn.
constexpr const char* s_printNames[] = {
    "PrintString", "PrintFloat", "PrintInteger", "PrintObject", "PrintVector", "WriteTimestampedLogEntry" };
Hooks::Hook s_printHooks[std::size(s_printNames)];

template <size_t N>
int32_t PrintCommandHook(CNWSVirtualMachineCommands* pThis, int32_t nCommandId, int32_t nParameters)
{
    t_vmCommand = s_printNames[N];
    const int32_t ret = s_printHooks[N]->CallOriginal<int32_t>(pThis, nCommandId, nParameters);
    t_vmCommand = nullptr;
    return ret;
}

template <size_t... N>
void HookPrintCommands(std::index_sequence<N...>)
{
    // Local: the hooks are installed from a constructor function, before this file's dynamic initialisers run.
    void* const functions[] = {
        (void*)&CVirtualMachineCmdImplementer::ExecuteCommandPrintString,
        (void*)&CVirtualMachineCmdImplementer::ExecuteCommandPrintFloat,
        (void*)&CVirtualMachineCmdImplementer::ExecuteCommandPrintInteger,
        (void*)&CVirtualMachineCmdImplementer::ExecuteCommandPrintObject,
        (void*)&CNWSVirtualMachineCommands::ExecuteCommandPrintVector,
        (void*)&CNWSVirtualMachineCommands::ExecuteCommandPrintLogEntry };
    static_assert(sizeof(functions) / sizeof(*functions) == sizeof...(N));
    ((s_printHooks[N] = Hooks::HookFunction(functions[N], &PrintCommandHook<N>, Hooks::Order::VeryEarly)), ...);
}

void HookEngineLog()
{
    if (auto drop = Config::Get<std::string>("DROP_CATEGORIES"))
    {
        for (auto name : String::Split(*drop, ','))
        {
            String::Trim(name);
            const auto* end = std::end(s_categories);
            const auto* found = std::find_if(std::begin(s_categories), end,
                [&](const char* category) { return strcasecmp(category, name.c_str()) == 0; });
            if (found != end)
                s_dropCategories |= 1u << (found - std::begin(s_categories));
            else
                LOG_WARNING("NWNX_SERVERLOGREDIRECTOR_DROP_CATEGORIES: unknown category '%s'", name);
        }
    }
    s_engineFiles = Config::Get<bool>("ENGINE_FILES", true);
    if (!s_engineFiles)
        LOG_NOTICE("The engine does not write nwserverLog1.txt and nwengineLog.txt.");

    s_EmitHook = Hooks::HookFunction(&ExoLog::Emit, &EmitHook, Hooks::Order::VeryEarly);
    // Stdio capture drops the engine's console lines only while this plugin is loaded.
    Log::SetEngineLinesHooked(true);
    static struct Unhooked { ~Unhooked() { Log::SetEngineLinesHooked(false); } } s_unhooked;
    HookPrintCommands(std::make_index_sequence<std::size(s_printNames)>());

    static Hooks::Hook s_ReportErrorHook = Hooks::HookFunction(API::Functions::_ZN26CNWSVirtualMachineCommands11ReportErrorERK10CExoStringiS2_,
        +[](CNWSVirtualMachineCommands* pThis, const CExoString& sFileName, int32_t nError, const CExoString& customMsg) -> void
        {
            const ScriptError error = { nError, customMsg.CStr() };
            t_scriptError = &error;
            s_ReportErrorHook->CallOriginal<void>(pThis, &sFileName, nError, &customMsg);
            t_scriptError = nullptr;
        }, Hooks::Order::VeryEarly);

    static Hooks::Hook s_LogErrorHook = Hooks::HookFunction(
        static_cast<void (*)(const char*, int, const char*)>(&NWSQLite::LogError),
        +[](const char* message, int code, const char* query) -> void
        {
            const SqlError error = { message, code, query };
            t_sqlError = &error;
            s_LogErrorHook->CallOriginal<void>(message, code, query);
            t_sqlError = nullptr;
        }, Hooks::Order::VeryEarly);
}

}

void ServerLogRedirector() __attribute__((constructor));
void ServerLogRedirector()
{
    // Text mode keeps the server log lines it always had; JSON takes every engine record.
    if (Log::GetFormat() == Log::Format::Json)
        HookEngineLog();
    else
        HookServerLog();
}
