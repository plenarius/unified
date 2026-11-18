#include "Layonara.hpp"

#include "API/CAppManager.hpp"
#include "API/CScriptEvent.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CServerInfo.hpp"
#include "API/CNWSItem.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSInventory.hpp"
#include "API/CNWSPlayer.hpp"
#include "API/CNWSJournal.hpp"
#include "API/CExoArrayList.hpp"
#include "API/Constants.hpp"
#include "API/Globals.hpp"
#include "API/Functions.hpp"

using namespace NWNXLib;
using namespace NWNXLib::Services;
using namespace NWNXLib::API;
using namespace Constants;

static Layonara::Layonara* g_plugin;

NWNX_PLUGIN_ENTRY Plugin* PluginLoad(Services::ProxyServiceList* services)
{
    g_plugin = new Layonara::Layonara(services);
    return g_plugin;
}


namespace Layonara {


static Hooks::Hook s_getItemInSlotHook;

Layonara::Layonara(Services::ProxyServiceList* services)
  : Plugin(services)
{
#define REGISTER(func) \
    ScriptAPI::RegisterEvent(PLUGIN_NAME, #func, \
        [this](ArgumentStack&& args){ return func(std::move(args)); })
    REGISTER(GetNWSyncAdvertHash);
    REGISTER(GetNWSyncAdvertUrl);
    REGISTER(SetNWSyncAdvert);

#undef REGISTER

    s_getItemInSlotHook = Hooks::HookFunction(&CNWSInventory::GetItemInSlot,&GetItemInSlotHook, Hooks::Order::Early);
}

Layonara::~Layonara()
{
}

CNWSItem *Layonara::GetItemInSlotHook(CNWSInventory *pThis, uint32_t nSlot)
{
    // Arrow
    if (nSlot == 0x03800)
        nSlot = 0x00800;
    // Bolt
    else if (nSlot == 0x1f800)
        nSlot = 0x02000;
    // Bullet
    else if (nSlot == 0x23800)
        nSlot = 0x01000;
    return s_getItemInSlotHook->CallOriginal<CNWSItem*>(pThis, nSlot);
}

void Layonara::RemoveEffectByTag(CNWSCreature *pCreature, std::string sCustomTag)
{
    std::vector<uint64_t> remove;
    remove.reserve(128);
    for (int i = 0; i < pCreature->m_appliedEffects.num; i++)
    {
        auto eff = (CGameEffect*)pCreature->m_appliedEffects.element[i];
        if (eff->m_sCustomTag == sCustomTag)
        {
            remove.push_back(eff->m_nID);
        }
    }
    for (auto id: remove)
        pCreature->RemoveEffectById(id);
}

// The NWSync advert lives in CServerInfo::m_nwsyncAdvertisedData. nwserver fills it once at startup from
// -nwsyncurl/-nwsynchash, then reads it again whenever it answers a BNXI query (the reply a client gets
// before it connects, and which wins over the master server's copy) and on every master server heartbeat.
// Changing it at runtime therefore changes the manifest that every client connecting afterwards downloads.
static NWSync::Advertisement& GetNWSyncAdvert()
{
    return Globals::AppManager()->m_pServerExoApp->GetServerInfo()->m_nwsyncAdvertisedData;
}

ArgumentStack Layonara::GetNWSyncAdvertHash(ArgumentStack&&)
{
    const auto& advert = GetNWSyncAdvert();
    // An empty Hash::SHA1 keeps an all-zero m_hex, so check m_empty rather than returning it.
    return ScriptAPI::Arguments(std::string(advert.m_primary_manifest.m_empty ? "" : advert.m_primary_manifest.m_hex));
}

ArgumentStack Layonara::GetNWSyncAdvertUrl(ArgumentStack&&)
{
    return ScriptAPI::Arguments(std::string(GetNWSyncAdvert().m_url.CStr()));
}

ArgumentStack Layonara::SetNWSyncAdvert(ArgumentStack&& args)
{
    const auto hash = ScriptAPI::ExtractArgument<std::string>(args);
    const auto url = ScriptAPI::ExtractArgument<std::string>(args);

    // Both set, or both empty (clears the advert, as if the server had started without -nwsyncurl).
    if (hash.empty() != url.empty())
    {
        LOG_WARNING("SetNWSyncAdvert: the hash and the URL must both be set or both be empty (hash '%s', url '%s')",
                    hash, url);
        return ScriptAPI::Arguments(0);
    }
    if (!hash.empty() &&
        (hash.size() != SHA1_DIGEST_SIZE * 2 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos))
    {
        LOG_WARNING("SetNWSyncAdvert: '%s' is not a SHA1 (40 hex characters)", hash);
        return ScriptAPI::Arguments(0);
    }

    auto& advert = GetNWSyncAdvert();
    const std::string oldHash = advert.m_primary_manifest.m_empty ? "" : advert.m_primary_manifest.m_hex;
    const std::string oldUrl = advert.m_url.CStr();

    advert.m_primary_manifest = Hash::SHA1(CExoString(hash));
    advert.m_url = CExoString(url);
    // Any additional manifests were published for the old primary manifest; don't advertise them with the new one.
    advert.m_additional_manifests.clear();

    LOG_INFO("NWSync advert changed from '%s' @ '%s' to '%s' @ '%s'", oldHash, oldUrl,
             advert.m_primary_manifest.m_empty ? "" : advert.m_primary_manifest.m_hex, advert.m_url);
    return ScriptAPI::Arguments(1);
}

}
