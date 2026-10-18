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
// NEVER enable this on a live server.

#include "nwnx.hpp"
#include "API/CAppManager.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CServerExoAppInternal.hpp"
#include "API/CNetLayer.hpp"
#include "API/CNetLayerPlayerInfo.hpp"
#include "API/CNWSPlayer.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSCreatureStats.hpp"

#include <unordered_map>

using namespace NWNXLib;
using namespace NWNXLib::API;

static constexpr uint32_t MAX_PLAYER_ID = 0xfe;   // NWN_MAX_PLAYERS - 1
static constexpr uint32_t MIN_FAKE_ID   = 0xc0;   // real clients get the low slots

static std::unordered_map<uint32_t, CNWSPlayer*> s_fakePlayers;   // player id -> player
static Hooks::Hook s_SendMessageToPlayerHook;
static Hooks::Hook s_DisconnectPlayerHook;

static CServerExoApp* Server() { return Globals::AppManager()->m_pServerExoApp; }

static bool IsFake(uint32_t nPlayerId) { return s_fakePlayers.count(nPlayerId) != 0; }

static void Init() __attribute__((constructor));
static void Init()
{
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

    LOG_INFO("Attached fake player %u (%s) to %x", nPlayerId, sPlayerName.c_str(), oid);
    return (int32_t)nPlayerId;
}

static void DetachPlayer(CNWSPlayer* pPlayer)
{
    const uint32_t nPlayerId = pPlayer->m_nPlayerID;
    const ObjectID oid = pPlayer->m_oidNWSObject;
    Server()->m_pcExoAppInternal->m_lstPlayerList.Remove(pPlayer);
    s_fakePlayers.erase(nPlayerId);

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
