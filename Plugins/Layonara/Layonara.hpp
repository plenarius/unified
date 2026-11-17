#pragma once

#include "nwnx.hpp"
#include <fstream>
#include <map>
#include <unordered_set>

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
    static void SetPositionHook(CNWSObject*, Vector, int32_t);
    static void SendMessageToCombatLog(CNWSCreature* target, const std::string& msg);

    enum SurfaceMaterials
    {
        UNDEFINED = 0,
        DIRT = 1,
        OBSCURING = 2,
        GRASS = 3,
        STONE = 4,
        WOOD = 5,
        WATER = 6,
        NONWALK = 7,
        TRANSPARENT = 8,
        CARPET = 9,
        METAL = 10,
        PUDDLES = 11,
        SWAMP = 12,
        MUD = 13,
        LEAVES = 14,
        LAVA = 15,
        BOTTOMLESSPIT = 16,
        DEEPWATER = 17,
        DOOR = 18,
        SNOW = 19,
        SAND = 20,
        BAREBONES = 21,
        STONEBRIDGE = 22,
        TEMP23 = 23,
        TEMP24 = 24,
        TEMP25 = 25,
        TEMP26 = 26,
        TEMP27 = 27,
        DIRTPATH = 28,
        STONEPATH = 29,
        TRIGGER = 30
    };

    unordered_map<uint8_t, int8_t> m_SurfaceMaterialSpeeds;
    unordered_map<ObjectID, int32_t> m_objectCurrentMaterial;
    unordered_set<string> m_CrosserExclusions;
    void LoadCrosserExclusions();
    bool IsTileOnRoad(CNWSArea* pArea, Vector vPos, uint8_t iMat);

    ArgumentStack ClearSurfaceMaterial            (ArgumentStack&& args);
    ArgumentStack GetNWSyncAdvertHash             (ArgumentStack&& args);
    ArgumentStack GetNWSyncAdvertUrl              (ArgumentStack&& args);
    ArgumentStack SetNWSyncAdvert                 (ArgumentStack&& args);
};

}
