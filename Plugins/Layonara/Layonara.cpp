#include "Layonara.hpp"

#include "API/CAppManager.hpp"
#include "API/CScriptEvent.hpp"
#include "API/CServerAIMaster.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CServerInfo.hpp"
#include "API/CWorldTimer.hpp"
#include "API/CNWSArea.hpp"
#include "API/CNWSItem.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSCreatureStats.hpp"
#include "API/CNWSInventory.hpp"
#include "API/CNWSTile.hpp"
#include "API/CNWTileData.hpp"
#include "API/CNWSPlayer.hpp"
#include "API/CNWSJournal.hpp"
#include "API/CExoArrayList.hpp"
#include "API/Constants.hpp"
#include "API/Globals.hpp"
#include "API/Functions.hpp"
#include <cmath>

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
static Hooks::Hook s_setPositionHook;

Layonara::Layonara(Services::ProxyServiceList* services)
  : Plugin(services)
{
#define REGISTER(func) \
    ScriptAPI::RegisterEvent(PLUGIN_NAME, #func, \
        [this](ArgumentStack&& args){ return func(std::move(args)); })
    REGISTER(ClearSurfaceMaterial);
    REGISTER(GetNWSyncAdvertHash);
    REGISTER(GetNWSyncAdvertUrl);
    REGISTER(SetNWSyncAdvert);

#undef REGISTER

    s_getItemInSlotHook = Hooks::HookFunction(&CNWSInventory::GetItemInSlot,&GetItemInSlotHook, Hooks::Order::Early);
    s_setPositionHook = Hooks::HookFunction(&CNWSObject::SetPosition,&SetPositionHook, Hooks::Order::Early);

    m_SurfaceMaterialSpeeds[SurfaceMaterials::DIRTPATH] = 33;
    m_SurfaceMaterialSpeeds[SurfaceMaterials::STONEPATH] = 33;
    m_SurfaceMaterialSpeeds[SurfaceMaterials::WATER] = -25;
    m_SurfaceMaterialSpeeds[SurfaceMaterials::MUD] = -20;
    m_SurfaceMaterialSpeeds[SurfaceMaterials::SWAMP] = -20;
    m_SurfaceMaterialSpeeds[SurfaceMaterials::SNOW] = -5;
    m_SurfaceMaterialSpeeds[SurfaceMaterials::SAND] = -5;

    LoadCrosserExclusions();
}

Layonara::~Layonara()
{
}

void Layonara::LoadCrosserExclusions()
{
    auto path = Config::Get<std::string>("CROSSER_EXCLUSIONS_FILE",
        "/data/surface_speed_exclusions.txt");

    std::ifstream file(path);
    if (!file.is_open())
    {
        LOG_WARNING("No crosser exclusions file at %s, all crossers treated as roads", path);
        return;
    }

    std::string line;
    while (std::getline(file, line))
    {
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#')
            continue;
        auto end = line.find_last_not_of(" \t\r\n");
        m_CrosserExclusions.insert(line.substr(start, end - start + 1));
    }

    LOG_INFO("Loaded %zu crosser exclusions", m_CrosserExclusions.size());
}

bool Layonara::IsTileOnRoad(CNWSArea* pArea, Vector vPos, uint8_t iMat)
{
    if (iMat != SurfaceMaterials::DIRT && iMat != SurfaceMaterials::STONE)
        return false;

    int tileX = (int)(vPos.x / 10.0f);
    int tileY = (int)(vPos.y / 10.0f);

    if (tileX < 0 || tileX >= pArea->m_nWidth ||
        tileY < 0 || tileY >= pArea->m_nHeight)
        return false;

    int tileIdx = tileY * pArea->m_nWidth + tileX;
    auto* pTileData = pArea->m_pTile[tileIdx].m_pTileData;
    if (!pTileData)
        return false;

    for (uint8_t e = 0; e < 4; e++)
    {
        CExoString edge = pTileData->GetEdgeType(e);
        if (!edge.IsEmpty())
        {
            std::string edgeStr(edge.CStr());
            if (m_CrosserExclusions.find(edgeStr) == m_CrosserExclusions.end())
                return true;
        }
    }

    return false;
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

void Layonara::SetPositionHook(CNWSObject* thisPtr, Vector vPos, int32_t bDoingCharacterCopy)
{
    if (thisPtr->m_nObjectType == API::Constants::ObjectType::Creature)
    {
        auto pServer = Globals::AppManager()->m_pServerExoApp;
        auto pCreature = pServer->GetCreatureByGameObjectID(thisPtr->m_idSelf);
        auto pMaster = pServer->GetCreatureByGameObjectID(pCreature->m_oidMaster);
        if ((pMaster != nullptr && !pMaster->m_bPlayerCharacter) || pCreature->m_pStats->GetIsDM() || pCreature->m_nAssociateType == 7 || pCreature->m_nAssociateType == 8 ||
            !pCreature->m_bPlayerCharacter)
        {
            s_setPositionHook->CallOriginal<void>(thisPtr, vPos, bDoingCharacterCopy);
            return;
        }
        auto pArea = pServer->GetAreaByGameObjectID(thisPtr->m_oidArea);
        if (pArea == nullptr)
        {
            s_setPositionHook->CallOriginal<void>(thisPtr, vPos, bDoingCharacterCopy);
            return;
        }
        auto iMat = pArea->GetSurfaceMaterial(vPos);
        if (g_plugin->IsTileOnRoad(pArea, vPos, iMat))
        {
            iMat = (iMat == SurfaceMaterials::DIRT)
                ? SurfaceMaterials::DIRTPATH
                : SurfaceMaterials::STONEPATH;
        }
        if (!g_plugin->m_objectCurrentMaterial.count(thisPtr->m_idSelf) ||
             g_plugin->m_objectCurrentMaterial[thisPtr->m_idSelf] != iMat)
        {
            auto *pAIMaster = pServer->GetServerAIMaster();
            auto nDelayDays = pServer->GetWorldTimer()->GetCalendarDayFromSeconds(0.0f);
            auto nDelayTime = pServer->GetWorldTimer()->GetTimeOfDayFromSeconds(0.0f);

            RemoveEffectByTag(pCreature, "NWNX_Layonara_SurfMatMovement");

            auto nCurrentMovement = pCreature->GetMovementRateFactor();
            auto nSpeedIt = g_plugin->m_SurfaceMaterialSpeeds.find(iMat);
            int32_t nSpeed = (nSpeedIt != g_plugin->m_SurfaceMaterialSpeeds.end()) ? nSpeedIt->second : 0;
            int32_t effectChange = (2.0 - nCurrentMovement) * nSpeed;
            auto bHasStride = pCreature->m_pStats->HasFeat(Constants::Feat::WoodlandStride);
            auto bSlowImmune = pCreature->m_pStats->GetEffectImmunity(Constants::ImmunityType::Slow, nullptr, true);
            if (effectChange > 0 || (effectChange < 0 && !bHasStride && !bSlowImmune))
            {
                auto *eff = new CGameEffect(true);
                eff->m_oidCreator = 0;
                eff->m_nType = effectChange > 0
                    ? EffectTrueType::MovementSpeedIncrease
                    : EffectTrueType::MovementSpeedDecrease;
                eff->m_nSubType = EffectSubType::Supernatural | EffectDurationType::Permanent;
                eff->m_nParamInteger[0] = std::abs(effectChange);
                eff->m_bShowIcon = 0;
                eff->m_bExpose = true;

                auto *iconEff = new CGameEffect(true);
                iconEff->m_oidCreator = 0;
                iconEff->m_nType = EffectTrueType::Icon;
                iconEff->m_nSubType = EffectSubType::Supernatural | EffectDurationType::Permanent;
                iconEff->SetNumIntegers(1);
                iconEff->m_nParamInteger[0] = effectChange < 0 ? 38 : 37;
                iconEff->m_bExpose = true;

                auto *link = new CGameEffect(true);
                link->m_oidCreator = 0;
                link->m_nType = EffectTrueType::Link;
                link->m_nSubType = EffectSubType::Supernatural | EffectDurationType::Permanent;
                link->m_sCustomTag = "NWNX_Layonara_SurfMatMovement";
                link->SetLinked(eff, iconEff);
                link->UpdateLinked();
                pAIMaster->AddEventDeltaTime(nDelayDays, nDelayTime, 0, pCreature->m_idSelf, Constants::AIMasterEvent::ApplyEffect, link);
            }
            g_plugin->m_objectCurrentMaterial[thisPtr->m_idSelf] = iMat;
        }
    }
    s_setPositionHook->CallOriginal<void>(thisPtr, vPos, bDoingCharacterCopy);
}

ArgumentStack Layonara::ClearSurfaceMaterial(ArgumentStack&& args)
{
    ArgumentStack stack;
    const auto creatureId = ScriptAPI::ExtractArgument<ObjectID>(args);

    if (creatureId == Constants::OBJECT_INVALID)
    {
        LOG_NOTICE("NWNX_Layonara function called on OBJECT_INVALID");
        return stack;
    }

    g_plugin->m_objectCurrentMaterial.erase(creatureId);

    return stack;
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
