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

static constexpr uint32_t MAX_PLAYER_ID = 0xfe;
static constexpr uint32_t MIN_FAKE_ID   = 0xc0;   // real clients get the low slots

static std::unordered_map<uint32_t, CNWSPlayer*> s_fakePlayers;   // player id -> player
// player id -> (major << 8 | minor) -> messages sent
static std::unordered_map<uint32_t, std::unordered_map<uint16_t, uint32_t>> s_messages;
static Hooks::Hook s_SendServerToPlayerMessageHook;
static Hooks::Hook s_SendMessageToPlayerHook;
static Hooks::Hook s_DisconnectPlayerHook;
static Hooks::Hook s_ExportReplyHook;
static Hooks::Hook s_SaveServerCharacterHook;
static bool s_enabled;

static CServerExoApp* Server() { return Globals::AppManager()->m_pServerExoApp; }

static bool IsTestPlayer(uint32_t nPlayerId) { return s_fakePlayers.count(nPlayerId) != 0; }

static void Init() __attribute__((constructor));
static void Init()
{
    s_enabled = Config::Get<bool>("ENABLE", false);
    if (!s_enabled)
        return;

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

    // The slot is in use but has no connection for the net layer to drop.
    s_DisconnectPlayerHook = Hooks::HookFunction(&CNetLayer::DisconnectPlayer,
        +[](CNetLayer* pThis, uint32_t nPlayerId, uint32_t nStrRef, BOOL bCDAuthFail, const CExoString& sReason) -> BOOL
        {
            if (IsTestPlayer(nPlayerId))
                return true;
            return s_DisconnectPlayerHook->CallOriginal<BOOL>(pThis, nPlayerId, nStrRef, bCDAuthFail, sReason);
        }, Hooks::Order::Earliest);

    // Saving a creature that has no per-level data as a character crashes in SaveClassInfo.
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
}

static uint32_t FindFreePlayerId()
{
    auto* pNetLayer = Server()->GetNetLayer();
    for (uint32_t id = MAX_PLAYER_ID; id >= MIN_FAKE_ID; id--)
    {
        if (IsTestPlayer(id) || Server()->GetClientObjectByPlayerId(id))
            continue;
        auto* pInfo = pNetLayer->GetPlayerInfo(id);
        if (pInfo && !pInfo->m_bPlayerInUse)
            return id;
    }
    return 0;
}

static void DetachPlayer(CNWSPlayer* pPlayer);

// The engine's shutdown waits on every player in the list, and a fake one never answers.
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

    // Not deleted: the destructor is untested on a player that never had a client.
    pPlayer->m_oidNWSObject = Constants::OBJECT_INVALID;
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
