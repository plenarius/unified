#pragma once

#include "nwnx.hpp"
#include <map>

using namespace std;

using ArgumentStack = NWNXLib::ArgumentStack;

namespace Layonara {

class Layonara : public NWNXLib::Plugin
{
public:
    Layonara(NWNXLib::Services::ProxyServiceList* services);
    virtual ~Layonara();

private:
    static void RemoveEffectByTag(CNWSCreature *pCreature, std::string sCustomTag);
    static CNWSItem *GetItemInSlotHook(CNWSInventory*, uint32_t);

    ArgumentStack GetNWSyncAdvertHash             (ArgumentStack&& args);
    ArgumentStack GetNWSyncAdvertUrl              (ArgumentStack&& args);
    ArgumentStack SetNWSyncAdvert                 (ArgumentStack&& args);
    ArgumentStack DropTileSet                     (ArgumentStack&& args);
};

}
