#include "Barter.hpp"
#include "API/CAppManager.hpp"
#include "API/CNWSPlayer.hpp"
#include "API/CNWSCreature.hpp"
#include "API/CNWSBarter.hpp"
#include "API/CNWSItem.hpp"
#include "API/CServerExoApp.hpp"
#include "API/Constants.hpp"
#include "API/Globals.hpp"
#include "API/Functions.hpp"
#include "Services/Hooks/Hooks.hpp"
#include "Services/PerObjectStorage/PerObjectStorage.hpp"
#include "ViewPtr.hpp"


using namespace NWNXLib;
using namespace NWNXLib::API;
using namespace NWNXLib::Services;

static ViewPtr<Barter::Barter> g_plugin;

NWNX_PLUGIN_ENTRY Plugin::Info* PluginInfo()
{
    return new Plugin::Info
    {
        "Barter",
        "Barter related functions",
        "orth",
        "plenarius@gmail.com",
        1,
        true
    };
}

NWNX_PLUGIN_ENTRY Plugin* PluginLoad(Plugin::CreateParams params)
{
    g_plugin = new Barter::Barter(params);
    return g_plugin;
}


namespace Barter {

static NWNXLib::Hooking::FunctionHook* m_HandlePlayerToServerBarter_AcceptTradeHook = nullptr;
static NWNXLib::Hooking::FunctionHook* m_HandlePlayerToServerBarter_AddItemHook = nullptr;
static NWNXLib::Hooking::FunctionHook* m_HandlePlayerToServerBarter_CloseBarterHook = nullptr;
static NWNXLib::Hooking::FunctionHook* m_HandlePlayerToServerBarter_LockListHook = nullptr;
static NWNXLib::Hooking::FunctionHook* m_HandlePlayerToServerBarter_MoveItemHook = nullptr;
static NWNXLib::Hooking::FunctionHook* m_HandlePlayerToServerBarter_RemoveItemHook = nullptr;
static NWNXLib::Hooking::FunctionHook* m_HandlePlayerToServerBarter_StartBarterHook = nullptr;
static NWNXLib::Hooking::FunctionHook* m_HandlePlayerToServerBarter_WindowHook = nullptr;

const int32_t NWNX_BARTER_ACTION_INITIATE     = 0;
const int32_t NWNX_BARTER_ACTION_ADD_ITEM     = 1;
const int32_t NWNX_BARTER_ACTION_REMOVE_ITEM  = 2;
const int32_t NWNX_BARTER_ACTION_EXAMINE_ITEM = 3;
const int32_t NWNX_BARTER_ACTION_LOCK_LIST    = 4;
const int32_t NWNX_BARTER_ACTION_ACCEPT       = 5;
const int32_t NWNX_BARTER_ACTION_CANCEL       = 6;

//key names for Per Object Storage
const std::string barterOwnerKey = "BARTER OWNER";
const std::string barterTargetKey = "BARTER TARGET";
const std::string barterLastActionKey = "BARTER LAST ACTION";
const std::string barterLastActorKey = "BARTER LAST ACTOR";
const std::string barterLastActionItemKey = "BARTER LAST ACTION ITEM";

Barter::Barter(const Plugin::CreateParams& params)
    : Plugin(params)
{
#define REGISTER(func) \
    GetServices()->m_events->RegisterEvent(#func, std::bind(&Barter::func, this, std::placeholders::_1))

    REGISTER(GetInfo);
    REGISTER(GetBarteringWith);

#undef REGISTER

    GetServices()->m_hooks->RequestExclusiveHook<API::Functions::CNWSMessage__HandlePlayerToServerBarter_AcceptTrade>(&HandlePlayerToServerBarter_AcceptTradeHook);
    m_HandlePlayerToServerBarter_AcceptTradeHook = GetServices()->m_hooks->FindHookByAddress(API::Functions::CNWSMessage__HandlePlayerToServerBarter_AcceptTrade);

    GetServices()->m_hooks->RequestExclusiveHook<API::Functions::CNWSMessage__HandlePlayerToServerBarter_AddItem>(&HandlePlayerToServerBarter_AddItemHook);
    m_HandlePlayerToServerBarter_AddItemHook = GetServices()->m_hooks->FindHookByAddress(API::Functions::CNWSMessage__HandlePlayerToServerBarter_AddItem);

    GetServices()->m_hooks->RequestExclusiveHook<API::Functions::CNWSMessage__HandlePlayerToServerBarter_CloseBarter>(&HandlePlayerToServerBarter_CloseBarterHook);
    m_HandlePlayerToServerBarter_CloseBarterHook = GetServices()->m_hooks->FindHookByAddress(API::Functions::CNWSMessage__HandlePlayerToServerBarter_CloseBarter);

    GetServices()->m_hooks->RequestExclusiveHook<API::Functions::CNWSMessage__HandlePlayerToServerBarter_LockList>(&HandlePlayerToServerBarter_LockListHook);
    m_HandlePlayerToServerBarter_LockListHook = GetServices()->m_hooks->FindHookByAddress(API::Functions::CNWSMessage__HandlePlayerToServerBarter_LockList);

    GetServices()->m_hooks->RequestExclusiveHook<API::Functions::CNWSMessage__HandlePlayerToServerBarter_MoveItem>(&HandlePlayerToServerBarter_MoveItemHook);
    m_HandlePlayerToServerBarter_MoveItemHook = GetServices()->m_hooks->FindHookByAddress(API::Functions::CNWSMessage__HandlePlayerToServerBarter_MoveItem);

    GetServices()->m_hooks->RequestExclusiveHook<API::Functions::CNWSMessage__HandlePlayerToServerBarter_RemoveItem>(&HandlePlayerToServerBarter_RemoveItemHook);
    m_HandlePlayerToServerBarter_RemoveItemHook = GetServices()->m_hooks->FindHookByAddress(API::Functions::CNWSMessage__HandlePlayerToServerBarter_RemoveItem);

    GetServices()->m_hooks->RequestExclusiveHook<API::Functions::CNWSMessage__HandlePlayerToServerBarter_StartBarter>(&HandlePlayerToServerBarter_StartBarterHook);
    m_HandlePlayerToServerBarter_StartBarterHook = GetServices()->m_hooks->FindHookByAddress(API::Functions::CNWSMessage__HandlePlayerToServerBarter_StartBarter);

    GetServices()->m_hooks->RequestExclusiveHook<API::Functions::CNWSMessage__HandlePlayerToServerBarter_Window>(&HandlePlayerToServerBarter_WindowHook);
    m_HandlePlayerToServerBarter_WindowHook = GetServices()->m_hooks->FindHookByAddress(API::Functions::CNWSMessage__HandlePlayerToServerBarter_Window);

}

Barter::~Barter()
{
}

int32_t Barter::HandlePlayerToServerBarter_AcceptTradeHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer)
{
    Types::ObjectID oidPlayer = pPlayer ? pPlayer->m_oidNWSObject : Constants::OBJECT_INVALID;
    Services::PerObjectStorageProxy* pPOS = g_plugin->GetServices()->m_perObjectStorage.get();
    pPOS->Set(oidPlayer, barterLastActorKey, static_cast<int>(oidPlayer));
    pPOS->Set(oidPlayer, barterLastActionKey, NWNX_BARTER_ACTION_ACCEPT);
    return m_HandlePlayerToServerBarter_AcceptTradeHook->CallOriginal<uint32_t>(thisPtr, pPlayer);
}

int32_t Barter::HandlePlayerToServerBarter_AddItemHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer)
{
    Types::ObjectID oidPlayer = pPlayer ? pPlayer->m_oidNWSObject : Constants::OBJECT_INVALID;

    int offset = 0;
    Types::ObjectID itemId = Utils::PeekMessage<Types::ObjectID>(thisPtr, offset) & 0x7FFFFFFF;
    Services::PerObjectStorageProxy* pPOS = g_plugin->GetServices()->m_perObjectStorage.get();
    pPOS->Set(oidPlayer, barterLastActorKey, static_cast<int>(oidPlayer));
    pPOS->Set(oidPlayer, barterLastActionItemKey, static_cast<int>(itemId));
    pPOS->Set(oidPlayer, barterLastActionKey, NWNX_BARTER_ACTION_ADD_ITEM);

    return m_HandlePlayerToServerBarter_AddItemHook->CallOriginal<uint32_t>(thisPtr, pPlayer);
}

int32_t Barter::HandlePlayerToServerBarter_CloseBarterHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer)
{
    //bool closeEvent = (bool)(Utils::PeekMessage<uint8_t>(thisPtr, 0) & 1);
    Types::ObjectID oidPlayer = pPlayer ? pPlayer->m_oidNWSObject : Constants::OBJECT_INVALID;
    Services::PerObjectStorageProxy* pPOS = g_plugin->GetServices()->m_perObjectStorage.get();
    pPOS->Set(oidPlayer, barterLastActorKey, static_cast<int>(oidPlayer));
    pPOS->Set(oidPlayer, barterLastActionKey, NWNX_BARTER_ACTION_CANCEL);

    return m_HandlePlayerToServerBarter_CloseBarterHook->CallOriginal<uint32_t>(thisPtr, pPlayer);
}

int32_t Barter::HandlePlayerToServerBarter_LockListHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer)
{
    Types::ObjectID oidPlayer = pPlayer ? pPlayer->m_oidNWSObject : Constants::OBJECT_INVALID;
    Services::PerObjectStorageProxy* pPOS = g_plugin->GetServices()->m_perObjectStorage.get();
    pPOS->Set(oidPlayer, barterLastActorKey, static_cast<int>(oidPlayer));
    pPOS->Set(oidPlayer, barterLastActionKey, NWNX_BARTER_ACTION_LOCK_LIST);

    return m_HandlePlayerToServerBarter_LockListHook->CallOriginal<uint32_t>(thisPtr, pPlayer);
}

int32_t Barter::HandlePlayerToServerBarter_MoveItemHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer)
{
    Types::ObjectID oidPlayer = pPlayer ? pPlayer->m_oidNWSObject : Constants::OBJECT_INVALID;

    int offset = 0;
    Types::ObjectID itemId = Utils::PeekMessage<Types::ObjectID>(thisPtr, offset) & 0x7FFFFFFF;
    Services::PerObjectStorageProxy* pPOS = g_plugin->GetServices()->m_perObjectStorage.get();
    pPOS->Set(oidPlayer, barterLastActorKey, static_cast<int>(oidPlayer));
    pPOS->Set(oidPlayer, barterLastActionItemKey, static_cast<int>(itemId));

    return m_HandlePlayerToServerBarter_MoveItemHook->CallOriginal<uint32_t>(thisPtr, pPlayer);
}

int32_t Barter::HandlePlayerToServerBarter_RemoveItemHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer)
{
    Types::ObjectID oidPlayer = pPlayer ? pPlayer->m_oidNWSObject : Constants::OBJECT_INVALID;

    int offset = 0;
    Types::ObjectID itemId = Utils::PeekMessage<Types::ObjectID>(thisPtr, offset) & 0x7FFFFFFF;
    Services::PerObjectStorageProxy* pPOS = g_plugin->GetServices()->m_perObjectStorage.get();
    pPOS->Set(oidPlayer, barterLastActorKey, static_cast<int>(oidPlayer));
    pPOS->Set(oidPlayer, barterLastActionItemKey, static_cast<int>(itemId));
    pPOS->Set(oidPlayer, barterLastActionKey, NWNX_BARTER_ACTION_REMOVE_ITEM);

    return m_HandlePlayerToServerBarter_RemoveItemHook->CallOriginal<uint32_t>(thisPtr, pPlayer);
}

int32_t Barter::HandlePlayerToServerBarter_StartBarterHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer)
{
    Types::ObjectID oidPlayer = pPlayer->m_oidNWSObject;
    Types::ObjectID targetId = Utils::PeekMessage<Types::ObjectID>(thisPtr, 0) & 0x7FFFFFFF;
    Types::ObjectID itemId = Utils::PeekMessage<Types::ObjectID>(thisPtr, 4) & 0x7FFFFFFF;

    Services::PerObjectStorageProxy* pPOS = g_plugin->GetServices()->m_perObjectStorage.get();
    pPOS->Set(oidPlayer, barterOwnerKey, static_cast<int>(oidPlayer));
    pPOS->Set(oidPlayer, barterTargetKey, static_cast<int>(targetId));
    pPOS->Set(oidPlayer, barterLastActorKey, static_cast<int>(oidPlayer));
    pPOS->Set(oidPlayer, barterLastActionItemKey, static_cast<int>(itemId));
    pPOS->Set(oidPlayer, barterLastActionKey, NWNX_BARTER_ACTION_INITIATE);
    pPOS->Set(targetId, barterOwnerKey, static_cast<int>(oidPlayer));
    pPOS->Set(targetId, barterTargetKey, static_cast<int>(targetId));
    pPOS->Set(targetId, barterLastActorKey, static_cast<int>(oidPlayer));
    pPOS->Set(targetId, barterLastActionItemKey, static_cast<int>(itemId));
    pPOS->Set(targetId, barterLastActionKey, NWNX_BARTER_ACTION_INITIATE);

    return m_HandlePlayerToServerBarter_StartBarterHook->CallOriginal<int32_t>(thisPtr, pPlayer);
}

int32_t Barter::HandlePlayerToServerBarter_WindowHook(CNWSMessage *thisPtr, CNWSPlayer *pPlayer)
{
    //bool windowEvent = (bool)(Utils::PeekMessage<uint8_t>(thisPtr, 0) & 1);
    return m_HandlePlayerToServerBarter_WindowHook->CallOriginal<int32_t>(thisPtr, pPlayer);
}

ArgumentStack Barter::GetInfo(ArgumentStack&& args)
{
    ArgumentStack stack;
    const auto playerId = Services::Events::ExtractArgument<Types::ObjectID>(args);
    auto *pPlayer = Globals::AppManager()->m_pServerExoApp->GetClientObjectByObjectId(playerId);
    Types::ObjectID oidPlayer = pPlayer ? pPlayer->m_oidNWSObject : Constants::OBJECT_INVALID;
    Services::PerObjectStorageProxy* pPOS = g_plugin->GetServices()->m_perObjectStorage.get();
    Types::ObjectID nOwnerId = *pPOS->Get<Types::ObjectID>(oidPlayer,barterOwnerKey);
    Types::ObjectID nTargetId = *pPOS->Get<Types::ObjectID>(oidPlayer,barterTargetKey);
    Types::ObjectID nLastActorId = *pPOS->Get<Types::ObjectID>(oidPlayer,barterLastActorKey);
    Types::ObjectID nLastActorItemId = *pPOS->Get<Types::ObjectID>(oidPlayer,barterLastActionItemKey);
    int32_t nLastAction = *pPOS->Get<int>(oidPlayer,barterLastActionKey);

    Services::Events::InsertArgument(stack, nLastActorItemId);
    Services::Events::InsertArgument(stack, nLastActorId);
    Services::Events::InsertArgument(stack, nLastAction);
    Services::Events::InsertArgument(stack, nTargetId);
    Services::Events::InsertArgument(stack, nOwnerId);
    return stack;
}

ArgumentStack Barter::GetBarteringWith(ArgumentStack&& args)
{
    ArgumentStack stack;
    const auto playerId = Services::Events::ExtractArgument<Types::ObjectID>(args);
    auto *pPlayer = Globals::AppManager()->m_pServerExoApp->GetClientObjectByObjectId(playerId);
    Types::ObjectID oidPlayer = pPlayer ? pPlayer->m_oidNWSObject : Constants::OBJECT_INVALID;

    Services::PerObjectStorageProxy* pPOS = g_plugin->GetServices()->m_perObjectStorage.get();
    Types::ObjectID nOwnerId = *pPOS->Get<Types::ObjectID>(oidPlayer,barterOwnerKey);
    Types::ObjectID nTargetId = *pPOS->Get<Types::ObjectID>(oidPlayer,barterTargetKey);
    auto barterPartner = nOwnerId != oidPlayer ? nOwnerId : nTargetId;

    Services::Events::InsertArgument(stack, barterPartner);
    return stack;
}

}
