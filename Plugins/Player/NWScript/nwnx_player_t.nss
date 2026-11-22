#include "nwnx_player"
#include "nwnx_tests"

void main()
{
    WriteTimestampedLogEntry("NWNX_Player unit test begin..");

    object oPC = GetFirstPC();
    if (!GetIsObjectValid(oPC))
    {
        WriteTimestampedLogEntry("NWNX_Player test: No PC found");
        return;
    }

    object oGameObject = NWNX_Player_GetGameObject(oPC);
    NWNX_Tests_Report("NWNX_Player", "GetControllingPlayer", NWNX_Player_GetControllingPlayer(oGameObject) == oPC);

    object oCreature = CreateObject(OBJECT_TYPE_CREATURE, "nw_chicken", GetStartingLocation());
    NWNX_Tests_Report("NWNX_Player", "GetControllingPlayer (not controlled)", NWNX_Player_GetControllingPlayer(oCreature) == OBJECT_INVALID);
    DestroyObject(oCreature);

    string sBic = NWNX_Player_GetBicFileName(oPC);
    NWNX_Tests_Report("NWNX_Player", "SetBicFileName (invalid player)", !NWNX_Player_SetBicFileName(OBJECT_INVALID, "nwnxtest"));
    NWNX_Tests_Report("NWNX_Player", "SetBicFileName (empty)", !NWNX_Player_SetBicFileName(oPC, ""));
    NWNX_Tests_Report("NWNX_Player", "SetBicFileName (too long)", !NWNX_Player_SetBicFileName(oPC, "abcdefghijklmnopq"));
    NWNX_Tests_Report("NWNX_Player", "SetBicFileName (invalid characters)", !NWNX_Player_SetBicFileName(oPC, "Bad Name"));
    NWNX_Tests_Report("NWNX_Player", "SetBicFileName (same name)", !NWNX_Player_SetBicFileName(oPC, sBic));
    NWNX_Tests_Report("NWNX_Player", "SetBicFileName (unchanged)", NWNX_Player_GetBicFileName(oPC) == sBic);

    WriteTimestampedLogEntry("NWNX_Player unit test end.");
}
