#include "Bindings.hpp"

#include "API/CVirtualMachine.hpp"
#include "API/CVirtualMachineStack.hpp"
#include "API/Globals.hpp"
#include "Log/Json.hpp"

#include <algorithm>
#include <array>
#include <vector>

using namespace NWNXLib;
using namespace NWNXLib::API;

namespace ScriptLog::Bindings {

namespace {

struct Binding
{
    std::string key;
    json value;
    std::string encoded; // numbers, booleans and null, encoded once and copied into each record
};
using Frame = std::vector<Binding>;

// Slot 0 holds the global bindings, slot L+1 the run at VM recursion level L.
constexpr size_t MaxRecursion = sizeof(CVirtualMachine::m_oidObjectRunScript) / sizeof(OBJECT_ID);
std::array<Frame, MaxRecursion + 1> s_frames;
// Bit N is set while slot N may hold bindings, so the hooks cost one test while nothing is bound.
uint32_t s_boundFrames;
static_assert(MaxRecursion + 2 < 32);

int32_t CurrentFrame()
{
    return Globals::VirtualMachine()->m_nRecursionLevel + 1;
}

// The slots of the current run and every run that encloses it, outermost first.
int32_t LastVisibleFrame()
{
    return std::min<int32_t>(CurrentFrame(), static_cast<int32_t>(s_frames.size()) - 1);
}

// frame is at most MaxRecursion + 1, past the last slot, whose bit is never set.
void ClearFrame(int32_t frame)
{
    if (s_boundFrames & (1u << frame))
    {
        s_frames[frame].clear();
        s_boundFrames &= ~(1u << frame);
    }
}

void Assign(Frame& frame, const json& fields)
{
    for (auto it = fields.begin(); it != fields.end(); ++it)
    {
        std::string encoded;
        if (it.value().is_primitive() && !it.value().is_string())
            Log::Json::AppendValue(encoded, it.value(), {});
        auto existing = std::find_if(frame.begin(), frame.end(), [&](const Binding& b) { return b.key == it.key(); });
        if (existing != frame.end())
        {
            existing->value = it.value();
            existing->encoded = std::move(encoded);
        }
        else
            frame.push_back({it.key(), it.value(), std::move(encoded)});
    }
}

// A run's slot is cleared when the run starts, so bindings left by a run that was aborted
// (AbortRunningScript, the instruction limit) never reach the next run at that depth. Slots above
// the running level are never read, so nothing is needed when the run ends and the hook stays a
// tail call. Earliest, so it still runs when Core or DotNET replace the original call.
Hooks::Hook s_runScriptHook = Hooks::HookFunction(&CVirtualMachine::RunScript,
    +[](CVirtualMachine* pVM, CExoString* psFileName, OBJECT_ID oid, BOOL bOidValid, int32_t nScriptEventID) -> BOOL
    {
        ClearFrame(pVM->m_nRecursionLevel + 2);
        return s_runScriptHook->CallOriginal<BOOL>(pVM, psFileName, oid, bOidValid, nScriptEventID);
    }, Hooks::Order::Earliest);

Hooks::Hook s_runScriptSituationHook = Hooks::HookFunction(&CVirtualMachine::RunScriptSituation,
    +[](CVirtualMachine* pVM, void* pScriptSituation, OBJECT_ID oid, BOOL bOidValid) -> BOOL
    {
        ClearFrame(pVM->m_nRecursionLevel + 2);
        return s_runScriptSituationHook->CallOriginal<BOOL>(pVM, pScriptSituation, oid, bOidValid);
    }, Hooks::Order::Earliest);

Hooks::Hook s_runScriptChunkHook = Hooks::HookFunction(&CVirtualMachine::RunScriptChunk,
    +[](CVirtualMachine* pVM, const CExoString& sScriptChunk, OBJECT_ID oid, BOOL bOidValid, BOOL bWrapIntoMain) -> int32_t
    {
        ClearFrame(pVM->m_nRecursionLevel + 2);
        // Explicit types: deduction would pass the chunk by value and copy it on every run.
        return s_runScriptChunkHook->CallOriginal<int32_t, CVirtualMachine*, const CExoString&, OBJECT_ID, BOOL, BOOL>(
            pVM, sScriptChunk, oid, bOidValid, bWrapIntoMain);
    }, Hooks::Order::Earliest);

// Frees what ended runs left, and is the safety net for an entry point the hooks above miss.
Hooks::Hook s_clearStackHook = Hooks::HookFunction(&CVirtualMachineStack::ClearStack,
    +[](CVirtualMachineStack* pStack) -> void
    {
        s_clearStackHook->CallOriginal<void>(pStack);
        if (s_boundFrames && pStack->m_pVMachine && pStack->m_pVMachine->m_nRecursionLevel == -1)
        {
            for (int32_t frame = 1; frame < static_cast<int32_t>(s_frames.size()); frame++)
                ClearFrame(frame);
        }
    }, Hooks::Order::Earliest);

}

void Bind(const json& fields)
{
    const auto frame = CurrentFrame();
    if (frame < 1 || frame >= static_cast<int32_t>(s_frames.size()))
        return;
    Assign(s_frames[frame], fields);
    s_boundFrames |= 1u << frame;
}

void Unbind(std::string_view key)
{
    const auto frame = CurrentFrame();
    if (frame < 1 || frame >= static_cast<int32_t>(s_frames.size()))
        return;
    auto& bindings = s_frames[frame];
    if (key.empty())
        bindings.clear();
    else
        bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&](const Binding& b) { return b.key == key; }), bindings.end());
}

void SetGlobal(const json& fields)
{
    s_frames[0].clear();
    if (fields.is_object())
        Assign(s_frames[0], fields);
}

json Get()
{
    json merged = json::object();
    for (int32_t frame = 0, last = LastVisibleFrame(); frame <= last; frame++)
    {
        for (const auto& binding : s_frames[frame])
            merged[binding.key] = binding.value;
    }
    return merged;
}

const std::string* FindLogger()
{
    for (int32_t frame = LastVisibleFrame(); frame >= 0; frame--)
    {
        for (const auto& binding : s_frames[frame])
        {
            if (binding.key == "logger" && binding.value.is_string())
                return &binding.value.get_ref<const std::string&>();
        }
    }
    return nullptr;
}

void AddTo(Log::Record& record)
{
    if (!record.HasFields())
        return;
    for (int32_t frame = 0, last = LastVisibleFrame(); frame <= last; frame++)
    {
        for (const auto& binding : s_frames[frame])
        {
            if (!binding.encoded.empty())
                record.With(binding.key, Log::RawJson{binding.encoded});
            else if (binding.key != "logger" || !binding.value.is_string())
                AddField(record, binding.key, binding.value);
        }
    }
}

void AddField(Log::Record& record, std::string_view key, const json& value)
{
    if (value.is_string())
        record.With(key, std::string_view(value.get_ref<const std::string&>()));
    else
        record.With(key, value);
}

}
