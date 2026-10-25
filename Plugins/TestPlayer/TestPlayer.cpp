// TestPlayer: turn an existing creature into a player character with no client behind it.
//
// For headless test servers (Layonara's tests/run.sh): scripts that only work on PCs
// (GetIsPC, GetFirstPC, GetPCPlayerName, object databases via SqlPrepareQueryObject...)
// can then be tested without anyone logging in.
//
// A fake player is a CNWSPlayer in a free player slot (counted down from the top), added to
// the server's player list and controlling the creature. Nothing is connected to the slot,
// so everything the server sends to it is dropped at CNetLayer::SendMessageToPlayer.
//
// Everything the server sends a fake player is counted by message type (see GetMessageCount),
// so tests can check what a client would have been sent.
//
// NEVER enable this on a live server. Attach refuses to work unless NWNX_TESTPLAYER_ENABLE=y.

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

#include <unordered_map>

using namespace NWNXLib;
using namespace NWNXLib::API;

static constexpr uint32_t MAX_PLAYER_ID = 0xfe;   // NWN_MAX_PLAYERS - 1
static constexpr uint32_t MIN_FAKE_ID   = 0xc0;   // real clients get the low slots

static std::unordered_map<uint32_t, CNWSPlayer*> s_fakePlayers;   // player id -> player
// player id -> (major << 8 | minor) -> number of messages sent to that fake player
static std::unordered_map<uint32_t, std::unordered_map<uint16_t, uint32_t>> s_messages;
static Hooks::Hook s_SendServerToPlayerMessageHook;
static Hooks::Hook s_SendMessageToPlayerHook;
static Hooks::Hook s_DisconnectPlayerHook;
static Hooks::Hook s_ExportReplyHook;
static Hooks::Hook s_SaveServerCharacterHook;

static CServerExoApp* Server() { return Globals::AppManager()->m_pServerExoApp; }

static bool IsFake(uint32_t nPlayerId) { return s_fakePlayers.count(nPlayerId) != 0; }

static void Init() __attribute__((constructor));
static void Init()
{
    // The funnel for everything the server writes for a player; it still reaches the (dropped)
    // net layer send below.
    s_SendServerToPlayerMessageHook = Hooks::HookFunction(&CNWSMessage::SendServerToPlayerMessage,
        +[](CNWSMessage* pThis, uint32_t nPlayerId, uint8_t nMajor, uint8_t nMinor, uint8_t* pBuffer, uint32_t nSize) -> BOOL
        {
            if (IsFake(nPlayerId))
                s_messages[nPlayerId][(uint16_t)(nMajor << 8 | nMinor)]++;
            return s_SendServerToPlayerMessageHook->CallOriginal<BOOL>(pThis, nPlayerId, nMajor, nMinor, pBuffer, nSize);
        }, Hooks::Order::Earliest);

    s_SendMessageToPlayerHook = Hooks::HookFunction(&CNetLayer::SendMessageToPlayer,
        +[](CNetLayer* pThis, uint32_t nPlayerId, uint8_t* pData, uint32_t nSize, uint32_t nFlags) -> BOOL
        {
            if (IsFake(nPlayerId))
                return true;
            return s_SendMessageToPlayerHook->CallOriginal<BOOL>(pThis, nPlayerId, pData, nSize, nFlags);
        }, Hooks::Order::Earliest);

    // The slot is marked in use (GetPCPlayerName and friends need that) but has no
    // connection; never let the net layer try to drop it.
    s_DisconnectPlayerHook = Hooks::HookFunction(&CNetLayer::DisconnectPlayer,
        +[](CNetLayer* pThis, uint32_t nPlayerId, uint32_t nStrRef, BOOL bCDAuthFail, const CExoString& sReason) -> BOOL
        {
            if (IsFake(nPlayerId))
                return true;
            return s_DisconnectPlayerHook->CallOriginal<BOOL>(pThis, nPlayerId, nStrRef, bCDAuthFail, sReason);
        }, Hooks::Order::Earliest);

    // Never save a fake player's character (ExportSingleCharacter, autosave, shutdown): there is
    // no vault to save to, and a creature made from an NPC blueprint lacks the per-level data a
    // real character has, which crashes the engine in CNWSCreatureStats::SaveClassInfo.
    // (CServerExoAppInternal::ExportPlayer can't be hooked; this is what it calls to pack
    // the character.)
    s_ExportReplyHook = Hooks::HookFunction(&CNWSMessage::SendServerToPlayerModule_ExportReply,
        +[](CNWSMessage* pThis, CNWSPlayer* pPlayer) -> BOOL
        {
            if (pPlayer && IsFake(pPlayer->m_nPlayerID))
                return true;
            return s_ExportReplyHook->CallOriginal<BOOL>(pThis, pPlayer);
        }, Hooks::Order::Earliest);

    s_SaveServerCharacterHook = Hooks::HookFunction(&CNWSPlayer::SaveServerCharacter,
        +[](CNWSPlayer* pThis, BOOL bBackupPlayer) -> BOOL
        {
            if (IsFake(pThis->m_nPlayerID))
                return true;
            return s_SaveServerCharacterHook->CallOriginal<BOOL>(pThis, bBackupPlayer);
        }, Hooks::Order::Earliest);
}

static uint32_t FindFreePlayerId()
{
    auto* pNetLayer = Server()->GetNetLayer();
    for (uint32_t id = MAX_PLAYER_ID; id >= MIN_FAKE_ID; id--)
    {
        if (IsFake(id) || Server()->GetClientObjectByPlayerId(id))
            continue;
        auto* pInfo = pNetLayer->GetPlayerInfo(id);
        if (pInfo && !pInfo->m_bPlayerInUse)
            return id;
    }
    return 0;
}

static void DetachPlayer(CNWSPlayer* pPlayer);

// The engine's shutdown waits on every player in the list (saving, disconnecting); a fake
// one never answers and the thread watchdog kills the server. Let them go first.
static auto s_destroyServerSubscription = MessageBus::Subscribe("NWNX_CORE_SIGNAL",
    [](const std::vector<std::string>& message)
    {
        if (message[0] != "ON_DESTROY_SERVER" || s_fakePlayers.empty())
            return;
        LOG_INFO("Server shutting down: detaching %zu fake player(s)", s_fakePlayers.size());
        while (!s_fakePlayers.empty())
            DetachPlayer(s_fakePlayers.begin()->second);
    });

static CNWSPlayer* FindFakeByCreature(ObjectID oid)
{
    for (auto& [id, pPlayer] : s_fakePlayers)
    {
        if (pPlayer->m_oidNWSObject == oid)
            return pPlayer;
    }
    return nullptr;
}

// Attach(object oCreature, string sPlayerName, string sCDKey) -> player id, or 0 on failure.
NWNX_EXPORT ArgumentStack Attach(ArgumentStack&& args)
{
    const auto oid = args.extract<ObjectID>();
    const auto sPlayerName = args.extract<std::string>();
    const auto sCDKey = args.extract<std::string>();

    if (!Config::Get<bool>("ENABLE", false))
    {
        LOG_ERROR("Attach: fake players are off; set NWNX_TESTPLAYER_ENABLE=y on a test server");
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
    pPlayer->m_oidNWSObject = oid;
    pPlayer->m_oidPCObject = oid;
    pPlayer->m_oidLastObjectControlled = oid;
    pPlayer->m_bIsPrimaryPlayer = false;

    pCreature->m_bPlayerCharacter = true;
    pCreature->m_pStats->m_bIsPC = true;

    Server()->m_pcExoAppInternal->m_lstPlayerList.Add(pPlayer);
    s_fakePlayers[nPlayerId] = pPlayer;
    s_messages.erase(nPlayerId);

    LOG_INFO("Attached fake player %u (%s) to %x", nPlayerId, sPlayerName.c_str(), oid);
    return (int32_t)nPlayerId;
}

static void DetachPlayer(CNWSPlayer* pPlayer)
{
    const uint32_t nPlayerId = pPlayer->m_nPlayerID;
    const ObjectID oid = pPlayer->m_oidNWSObject;
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

    // Not deleted: the destructor may tear down state a fake player never had, and a few
    // leaked players on a test server don't matter.
    pPlayer->m_oidNWSObject = Constants::OBJECT_INVALID;
    LOG_INFO("Detached fake player %u from %x", nPlayerId, oid);
}

// Detach(object oCreature) -> TRUE if it had a fake player.
NWNX_EXPORT ArgumentStack Detach(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    if (!pPlayer)
        return false;
    DetachPlayer(pPlayer);
    return true;
}

// GetIsFake(object oCreature) -> TRUE if a fake player controls it.
NWNX_EXPORT ArgumentStack GetIsFake(ArgumentStack&& args)
{
    return FindFakeByCreature(args.extract<ObjectID>()) != nullptr;
}

// GetPlayerId(object oCreature) -> the fake player's id, or 0 if oCreature has none.
NWNX_EXPORT ArgumentStack GetPlayerId(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    return pPlayer ? (int32_t)pPlayer->m_nPlayerID : 0;
}

// GetCount() -> how many fake players there are.
NWNX_EXPORT ArgumentStack GetCount(ArgumentStack&&)
{
    return (int32_t)s_fakePlayers.size();
}

// GetMessageCount(object oCreature, int nMajor, int nMinor) -> messages of that type sent to the
// fake player since it was attached or last cleared; nMinor -1 counts every minor type.
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

// ClearMessages(object oCreature) -> TRUE if it had a fake player.
NWNX_EXPORT ArgumentStack ClearMessages(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    if (!pPlayer)
        return false;
    s_messages.erase(pPlayer->m_nPlayerID);
    return true;
}

// SetInventoryOpen(object oCreature, int bOpen) -> TRUE if it had a fake player.
// What the server believes about the player's inventory window; a real client reports it itself.
NWNX_EXPORT ArgumentStack SetInventoryOpen(ArgumentStack&& args)
{
    auto* pPlayer = FindFakeByCreature(args.extract<ObjectID>());
    const auto bOpen = args.extract<int32_t>();
    if (!pPlayer || !pPlayer->m_pInventoryGUI)
        return false;
    pPlayer->m_pInventoryGUI->m_bGuiInventoryOpen = !!bOpen;
    return true;
}
