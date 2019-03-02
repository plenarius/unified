#include "nwnx"

const int NWNX_BARTER_ACTION_INITIATE     = 0;
const int NWNX_BARTER_ACTION_ADD_ITEM     = 1;
const int NWNX_BARTER_ACTION_REMOVE_ITEM  = 2;
const int NWNX_BARTER_ACTION_EXAMINE_ITEM = 3;
const int NWNX_BARTER_ACTION_LOCK_LIST    = 4;
const int NWNX_BARTER_ACTION_ACCEPT       = 5;
const int NWNX_BARTER_ACTION_CANCEL       = 6;

struct NWNX_Barter_Info
{
    object oInitiator;
    object oTarget;
    int nLastAction;
    object oLastActor; // either oInitiator or oTarget
    object oLastActionItem;
};

struct NWNX_Barter_Info NWNX_Barter_GetInfo(object player);

// Returns if the creature is currently bartering
object NWNX_Barter_GetBarteringWith(object oObject);


const string NWNX_Barter = "NWNX_Barter";

struct NWNX_Barter_Info NWNX_Barter_GetInfo(object player)
{
    string sFunc = "GetInfo";
    struct NWNX_Barter_Info barter;

    NWNX_PushArgumentObject(NWNX_Barter, sFunc, player);
    NWNX_CallFunction(NWNX_Barter, sFunc);

    barter.oInitiator          = NWNX_GetReturnValueObject(NWNX_Barter, sFunc);
    barter.oTarget             = NWNX_GetReturnValueObject(NWNX_Barter, sFunc);
    barter.nLastAction         = NWNX_GetReturnValueInt(NWNX_Barter,    sFunc);
    barter.oLastActor          = NWNX_GetReturnValueObject(NWNX_Barter, sFunc);
    barter.oLastActionItem     = NWNX_GetReturnValueObject(NWNX_Barter, sFunc);

    return barter;
}

object NWNX_Barter_GetBarteringWith(object oObject)
{
    string sFunc = "GetBarteringWith";

    NWNX_PushArgumentObject(NWNX_Barter, sFunc, oObject);

    NWNX_CallFunction(NWNX_Barter, sFunc);
    return NWNX_GetReturnValueObject(NWNX_Barter, sFunc);
}
