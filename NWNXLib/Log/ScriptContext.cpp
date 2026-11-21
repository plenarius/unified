#include "Log/ScriptContext.hpp"
#include "nwnx.hpp"

#include "API/CVirtualMachine.hpp"
#include "API/CAppManager.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CNWSObject.hpp"
#include "API/CNWSArea.hpp"
#include "API/CNWSModule.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSCreatureStats.hpp"
#include "API/CNWSPlayer.hpp"

#include <atomic>
#include <pthread.h>

namespace NWNXLib::Log {

using namespace NWNXLib::API;

// NWNXLib initialises on the main thread, whether in NWNX_Core or in a plugin's dlopen.
static const pthread_t s_mainThread = pthread_self();
static std::atomic<uint32_t> s_keys{ContextKey::Default};

static bool IsMainThread()
{
    return pthread_equal(pthread_self(), s_mainThread);
}

static std::string_view View(const CExoString& s)
{
    return s.m_sString ? std::string_view(s.m_sString, s.GetLength()) : std::string_view();
}

static std::string_view GetTag(CGameObject* pObject)
{
    if (auto* pNWSObject = Utils::AsNWSObject(pObject))
        return View(pNWSObject->m_sTag);
    if (auto* pArea = Utils::AsNWSArea(pObject))
        return View(pArea->m_sTag);
    if (auto* pModule = Utils::AsNWSModule(pObject))
        return View(pModule->m_sTag);
    return {};
}

void AddScriptContext(Record& record)
{
    if (!IsMainThread())
        return;
    auto* pVM = Globals::VirtualMachine();
    if (!pVM || pVM->m_nRecursionLevel < 0)
        return;

    const auto level = pVM->m_nRecursionLevel;
    const auto keys = s_keys.load(std::memory_order_relaxed);

    if (keys & ContextKey::Script)
    {
        const auto& name = pVM->m_pVirtualMachineScript[level].m_sScriptName;
        record.With("script", name.IsEmpty() ? std::string_view("<chunk>") : View(name));
    }

    if (!pVM->m_bValidObjectRunScript[level])
        return;

    const auto oid = pVM->m_oidObjectRunScript[level];
    if (keys & ContextKey::Oid)
        record.With("oid", Oid{oid});

    if (!(keys & (ContextKey::Tag | ContextKey::Area | ContextKey::Pc | ContextKey::Player)))
        return;

    auto* pServer = Globals::AppManager()->m_pServerExoApp;
    auto* pObject = pServer->GetGameObject(oid);
    if (!pObject)
        return;

    if (keys & ContextKey::Tag)
    {
        if (const auto tag = GetTag(pObject); !tag.empty())
            record.With("tag", tag);
    }

    auto* pNWSObject = Utils::AsNWSObject(pObject);
    if ((keys & ContextKey::Area) && pNWSObject)
    {
        if (auto* pArea = Utils::AsNWSArea(pServer->GetGameObject(pNWSObject->m_oidArea)))
            record.With("area", View(pArea->m_sTag));
    }

    if (keys & (ContextKey::Pc | ContextKey::Player))
    {
        // Privacy: names are added only when configured, and only for player-controlled creatures.
        if (auto* pPlayer = pServer->GetClientObjectByObjectId(oid))
        {
            auto* pCreature = Utils::AsNWSCreature(pObject);
            if ((keys & ContextKey::Pc) && pCreature && pCreature->m_pStats)
                record.With("pc", pCreature->m_pStats->GetFullName());
            if (keys & ContextKey::Player)
                record.With("player", pPlayer->GetPlayerName());
        }
    }
}

void SetScriptContextKeys(uint32_t keys)
{
    s_keys.store(keys, std::memory_order_relaxed);
}

uint32_t GetScriptContextKeys()
{
    return s_keys.load(std::memory_order_relaxed);
}

}
