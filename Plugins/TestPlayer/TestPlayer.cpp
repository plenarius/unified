#include "nwnx.hpp"
#include "API/CAppManager.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CServerExoAppInternal.hpp"
#include "API/CNetLayer.hpp"
#include "API/CNetLayerPlayerInfo.hpp"
#include "API/CNWSPlayer.hpp"
#include "API/CNWSPlayerInventoryGUI.hpp"
#include "API/CNWSMessage.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSCreatureStats.hpp"
#include "API/CNWLevelStats.hpp"
#include "API/CNWSModule.hpp"

#include <algorithm>
#include <unordered_map>

using namespace NWNXLib;
using namespace NWNXLib::API;

static constexpr uint32_t MIN_FAKE_ID = 0xc0;   // real clients get the low slots

static std::unordered_map<uint32_t, CNWSPlayer*> s_fakePlayers;   // player id -> player
// player id -> (major << 8 | minor) -> messages sent
static std::unordered_map<uint32_t, std::unordered_map<uint16_t, uint32_t>> s_messages;
static Hooks::Hook s_SendServerToPlayerMessageHook;
static Hooks::Hook s_SendMessageToPlayerHook;
static Hooks::Hook s_DisconnectPlayerHook;
static Hooks::Hook s_ExportReplyHook;
static Hooks::Hook s_SaveServerCharacterHook;
static Hooks::Hook s_CreatureDestructorHook;
static bool s_enabled;
static bool s_allowOnPublicServer;

static CServerExoApp* Server() { return Globals::AppManager()->m_pServerExoApp; }

static bool IsTestPlayer(uint32_t nPlayerId) { return s_fakePlayers.count(nPlayerId) != 0; }

static void DetachPlayer(CNWSPlayer* pPlayer);

static CNWSPlayer* FindFakeByCreature(ObjectID oid)
{
    for (auto& [id, pPlayer] : s_fakePlayers)
    {
        if (pPlayer->m_oidNWSObject == oid)
            return pPlayer;
    }
    return nullptr;
}

static void Init() __attribute__((constructor));
static void Init()
{
    s_enabled = Config::Get<bool>("ENABLE", false);
    if (!s_enabled)
        return;
    s_allowOnPublicServer = Config::Get<bool>("ALLOW_ON_PUBLIC_SERVER", false);

    s_SendServerToPlayerMessageHook = Hooks::HookFunction(&CNWSMessage::SendServerToPlayerMessage,
        +[](CNWSMessage* pThis, uint32_t nPlayerId, uint8_t nMajor, uint8_t nMinor, uint8_t* pBuffer, uint32_t nSize) -> BOOL
        {
            if (IsTestPlayer(nPlayerId))
                s_messages[nPlayerId][(uint16_t)(nMajor << 8 | nMinor)]++;
            return s_SendServerToPlayerMessageHook->CallOriginal<BOOL>(pThis, nPlayerId, nMajor, nMinor, pBuffer, nSize);
        }, Hooks::Order::Earliest);

    s_SendMessageToPlayerHook = Hooks::HookFunction(&CNetLayer::SendMessageToPlayer,
        +[](CNetLayer* pThis, uint32_t nPlayerId, uint8_t* pData, uint32_t nSize, uint32_t nFlags) -> BOOL
        {
            if (IsTestPlayer(nPlayerId))
                return true;
            return s_SendMessageToPlayerHook->CallOriginal<BOOL>(pThis, nPlayerId, pData, nSize, nFlags);
        }, Hooks::Order::Earliest);

    // There is no connection to drop, and the engine expects the player to be gone from the list afterwards.
    s_DisconnectPlayerHook = Hooks::HookFunction(&CNetLayer::DisconnectPlayer,
        +[](CNetLayer* pThis, uint32_t nPlayerId, uint32_t nStrRef, BOOL bCDAuthFail, const CExoString& sReason) -> BOOL
        {
            if (auto it = s_fakePlayers.find(nPlayerId); it != s_fakePlayers.end())
            {
                DetachPlayer(it->second);
                return true;
            }
            return s_DisconnectPlayerHook->CallOriginal<BOOL>(pThis, nPlayerId, nStrRef, bCDAuthFail, sReason);
        }, Hooks::Order::Earliest);

    // A fake player has no character in the vault to write to.
    s_ExportReplyHook = Hooks::HookFunction(&CNWSMessage::SendServerToPlayerModule_ExportReply,
        +[](CNWSMessage* pThis, CNWSPlayer* pPlayer) -> BOOL
        {
            if (pPlayer && IsTestPlayer(pPlayer->m_nPlayerID))
                return true;
            return s_ExportReplyHook->CallOriginal<BOOL>(pThis, pPlayer);
        }, Hooks::Order::Earliest);

    s_SaveServerCharacterHook = Hooks::HookFunction(&CNWSPlayer::SaveServerCharacter,
        +[](CNWSPlayer* pThis, BOOL bBackupPlayer) -> BOOL
        {
            if (IsTestPlayer(pThis->m_nPlayerID))
                return true;
            return s_SaveServerCharacterHook->CallOriginal<BOOL>(pThis, bBackupPlayer);
        }, Hooks::Order::Earliest);

    // Otherwise the player would stay in the list, controlling an object that is gone.
    s_CreatureDestructorHook = Hooks::HookFunction(Functions::_ZN12CNWSCreatureD1Ev,
        +[](CNWSCreature* pThis) -> void
        {
            if (auto* pPlayer = FindFakeByCreature(pThis->m_idSelf))
                DetachPlayer(pPlayer);
            s_CreatureDestructorHook->CallOriginal<void>(pThis);
        }, Hooks::Order::Earliest);
}

static uint32_t FindFreePlayerId()
{
    auto* pNetLayer = Server()->GetNetLayer();
    for (uint32_t id = CNetLayer::MAX_PLAYERS - 1; id >= MIN_FAKE_ID; id--)
    {
        if (IsTestPlayer(id) || Server()->GetClientObjectByPlayerId(id))
            continue;
        auto* pInfo = pNetLayer->GetPlayerInfo(id);
        if (pInfo && !pInfo->m_bPlayerInUse)
            return id;
    }
    return 0;
}

// Detach while the module is still loaded; the engine's own shutdown disconnects its players later.
static auto s_destroyServerSubscription = MessageBus::Subscribe("NWNX_CORE_SIGNAL",
    [](const std::vector<std::string>& message)
    {
        if (message[0] != "ON_DESTROY_SERVER" || s_fakePlayers.empty())
            return;
        LOG_INFO("Server shutting down: detaching %zu fake player(s)", s_fakePlayers.size());
        while (!s_fakePlayers.empty())
            DetachPlayer(s_fakePlayers.begin()->second);
    });

// The engine reads one entry per level of a player character: for its hit points, to save it, to level it down.
static void BuildLevelStats(CNWSCreature* pCreature)
{
    auto* pStats = pCreature->m_pStats;
    auto* pLevels = &pStats->m_lstLevelStats;

    // Every creature has one entry from its constructor, which nothing fills in for an NPC.
    auto* pUnused = pLevels->num == 1 && !pLevels->element[0]->m_nHitDie ? pLevels->element[0] : nullptr;

    int32_t nHitPoints = pCreature->m_nBaseHitPoints;
    int32_t nMissing = pStats->GetLevel(false) - (pUnused ? 0 : pLevels->num);
    uint8_t nClassLevels[8] = {};
    for (auto* pLevel : *pLevels)
    {
        if (pLevel == pUnused)
            break;
        nHitPoints -= pLevel->m_nHitDie;
        for (uint8_t nMultiClass = 0; nMultiClass < pStats->m_nNumMultiClasses; nMultiClass++)
        {
            if (pStats->m_ClassInfo[nMultiClass].m_nClass == pLevel->m_nClass)
            {
                nClassLevels[nMultiClass]++;
                break;
            }
        }
    }

    for (uint8_t nMultiClass = 0; nMultiClass < pStats->m_nNumMultiClasses; nMultiClass++)
    {
        auto* pClassInfo = &pStats->m_ClassInfo[nMultiClass];
        for (; nClassLevels[nMultiClass] < pClassInfo->m_nLevel && nMissing > 0; nClassLevels[nMultiClass]++, nMissing--)
        {
            auto* pLevel = pUnused ? pUnused : new CNWLevelStats();
            pLevel->m_nClass = pClassInfo->m_nClass;
            pLevel->m_nHitDie = std::clamp(nHitPoints / nMissing, 0, 255);
            pLevel->m_nSkillPointsRemaining = 0;
            nHitPoints -= pLevel->m_nHitDie;

            if (pUnused)
                pUnused = nullptr;
            else
                pLevels->Add(pLevel);
            pLevel->m_bEpic = pLevels->num > 20;
        }
    }
}

NWNX_EXPORT ArgumentStack Attach(ArgumentStack&& args)
{
    const auto oid = args.extract<ObjectID>();
    const auto sPlayerName = args.extract<std::string>();
    const auto sCDKey = args.extract<std::string>();

    if (!s_enabled)
    {
        LOG_ERROR("Attach: fake players are off; set NWNX_TESTPLAYER_ENABLE=y on a test server");
        return 0;
    }
    if (Server()->GetGamePostedToInternet() && !s_allowOnPublicServer)
    {
        LOG_ERROR("Attach: this server is publicly listed; set NWNX_TESTPLAYER_ALLOW_ON_PUBLIC_SERVER=y to allow fake players on it");
        return 0;
    }

    auto* pCreature = Utils::AsNWSCreature(Utils::GetGameObject(oid));
    if (!pCreature)
    {
        LOG_ERROR("Attach: not a creature: %x", oid);
        return 0;
    }
    if (Server()->GetClientObjectByObjectId(oid))
    {
        LOG_ERROR("Attach: %x is already controlled by a player", oid);
        return 0;
    }

    if (sPlayerName.empty() || sCDKey.empty())
    {
        LOG_ERROR("Attach: the player name and the cd key can't be empty");
        return 0;
    }
    for (uint32_t id = 0; id < CNetLayer::MAX_PLAYERS; id++)
    {
        auto* pInfo = Server()->GetNetLayer()->GetPlayerInfo(id);
        if (pInfo->m_bPlayerInUse && (pInfo->m_sPlayerName == sPlayerName.c_str() || pInfo->m_cCDKey.sPublic == sCDKey.c_str()))
        {
            LOG_ERROR("Attach: player name '%s' or cd key '%s' is in use by player %u", sPlayerName.c_str(), sCDKey.c_str(), id);
            return 0;
        }
    }

    const uint32_t nPlayerId = FindFreePlayerId();
    if (!nPlayerId)
    {
        LOG_ERROR("Attach: no free player slot");
        return 0;
    }

    auto* pInfo = Server()->GetNetLayer()->GetPlayerInfo(nPlayerId);
    pInfo->m_sPlayerName = sPlayerName.c_str();
    pInfo->m_cCDKey.sPublic = sCDKey.c_str();
    pInfo->m_bPlayerPrivileges = true;
    pInfo->m_bPlayerInUse = true;

    auto* pPlayer = new CNWSPlayer(nPlayerId);
    pPlayer->m_oidPCObject = oid;
    pPlayer->m_oidLastObjectControlled = oid;
    pPlayer->m_bIsPrimaryPlayer = false;

    BuildLevelStats(pCreature);
    pCreature->m_bPlayerCharacter = true;
    pCreature->m_pStats->m_bIsPC = true;

    Server()->m_pcExoAppInternal->m_lstPlayerList.Add(pPlayer);
    pPlayer->SetGameObject(pCreature);
    s_fakePlayers[nPlayerId] = pPlayer;
    s_messages.erase(nPlayerId);

    LOG_INFO("Attached fake player %u (%s) to %x", nPlayerId, sPlayerName.c_str(), oid);
    return (int32_t)nPlayerId;
}

static void DetachPlayer(CNWSPlayer* pPlayer)
{
    const uint32_t nPlayerId = pPlayer->m_nPlayerID;
    const ObjectID oid = pPlayer->m_oidNWSObject;
    pPlayer->SetGameObject(nullptr);
    Server()->m_pcExoAppInternal->m_lstPlayerList.Remove(pPlayer);
    s_fakePlayers.erase(nPlayerId);
    s_messages.erase(nPlayerId);

    if (auto* pCreature = Utils::AsNWSCreature(Utils::GetGameObject(oid)))
    {
        pCreature->m_bPlayerCharacter = false;
        pCreature->m_pStats->m_bIsPC = false;
    }

    auto* pInfo = Server()->GetNetLayer()->GetPlayerInfo(nPlayerId);
    pInfo->m_sPlayerName = "";
    pInfo->m_cCDKey.sPublic = "";
    pInfo->m_bPlayerPrivileges = false;
    pInfo->m_bPlayerInUse = false;

    // Deleted from the main loop: engine code up the stack may still hold the pointer.
    Tasks::QueueOnMainThread([pPlayer]() { delete pPlayer; });
    LOG_INFO("Detached fake player %u from %x", nPlayerId, oid);
}

NWNX_EXPORT ArgumentStack Detach(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    if (!pPlayer)
        return false;
    DetachPlayer(pPlayer);
    return true;
}

NWNX_EXPORT ArgumentStack CompleteAreaTransition(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    if (!pPlayer)
        return false;

    // Between areas a player's creature is in no area and has a desired one, see CNWSCreature::AddToArea.
    auto* pCreature = Utils::AsNWSCreature(Utils::GetGameObject(pPlayer->m_oidNWSObject));
    if (!pCreature || pCreature->GetArea() || !Server()->GetModule()->GetArea(pCreature->m_oidDesiredArea))
        return false;

    // What the server does when a client reports the area as loaded.
    Server()->MovePlayerToArea(pPlayer);
    return pCreature->GetArea() != nullptr;
}

NWNX_EXPORT ArgumentStack GetIsTestPlayer(ArgumentStack&& args)
{
    return FindFakeByCreature(args.extract<ObjectID>()) != nullptr;
}

NWNX_EXPORT ArgumentStack GetPlayerId(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    return pPlayer ? (int32_t)pPlayer->m_nPlayerID : 0;
}

NWNX_EXPORT ArgumentStack GetCount(ArgumentStack&&)
{
    return (int32_t)s_fakePlayers.size();
}

NWNX_EXPORT ArgumentStack GetMessageCount(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    const auto nMajor = args.extract<int32_t>();
    const auto nMinor = args.extract<int32_t>();
    if (!pPlayer)
        return 0;

    auto it = s_messages.find(pPlayer->m_nPlayerID);
    if (it == s_messages.end())
        return 0;

    uint32_t nCount = 0;
    for (const auto& [type, count] : it->second)
    {
        if ((type >> 8) == nMajor && (nMinor < 0 || (type & 0xff) == nMinor))
            nCount += count;
    }
    return (int32_t)nCount;
}

NWNX_EXPORT ArgumentStack ClearMessages(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    if (!pPlayer)
        return false;
    s_messages.erase(pPlayer->m_nPlayerID);
    return true;
}

NWNX_EXPORT ArgumentStack SetInventoryOpen(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    const auto bOpen = args.extract<int32_t>();
    if (!pPlayer || !pPlayer->m_pInventoryGUI)
        return false;
    pPlayer->m_pInventoryGUI->m_bGuiInventoryOpen = !!bOpen;
    return true;
}
