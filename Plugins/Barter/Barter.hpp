#pragma once

#include "Plugin.hpp"
#include "Services/Events/Events.hpp"
#include "Services/Hooks/Hooks.hpp"
#include "API/Types.hpp"

using ArgumentStack = NWNXLib::Services::Events::ArgumentStack;

namespace Barter {

class Barter : public NWNXLib::Plugin
{
public:
    Barter(const Plugin::CreateParams& params);
    virtual ~Barter();

private:
    static int32_t HandlePlayerToServerBarter_AcceptTradeHook(NWNXLib::API::CNWSMessage*, NWNXLib::API::CNWSPlayer*);
    static int32_t HandlePlayerToServerBarter_AddItemHook(NWNXLib::API::CNWSMessage *pThis, NWNXLib::API::CNWSPlayer *pPlayer);
    static int32_t HandlePlayerToServerBarter_CloseBarterHook(NWNXLib::API::CNWSMessage *pThis, NWNXLib::API::CNWSPlayer *pPlayer);
    static int32_t HandlePlayerToServerBarter_LockListHook(NWNXLib::API::CNWSMessage *pThis, NWNXLib::API::CNWSPlayer *pPlayer);
    static int32_t HandlePlayerToServerBarter_MoveItemHook(NWNXLib::API::CNWSMessage *pThis, NWNXLib::API::CNWSPlayer *pPlayer);
    static int32_t HandlePlayerToServerBarter_RemoveItemHook(NWNXLib::API::CNWSMessage *thisPtr, NWNXLib::API::CNWSPlayer *pPlayer);
    static int32_t HandlePlayerToServerBarter_StartBarterHook(NWNXLib::API::CNWSMessage *thisPtr, NWNXLib::API::CNWSPlayer *pPlayer);
    static int32_t HandlePlayerToServerBarter_WindowHook(NWNXLib::API::CNWSMessage *pThis, NWNXLib::API::CNWSPlayer *pPlayer);

    ArgumentStack GetInfo(ArgumentStack&& args);
    ArgumentStack GetBarteringWith(ArgumentStack&& args);
};

}
