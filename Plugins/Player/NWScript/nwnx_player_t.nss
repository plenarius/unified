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

    json jChars = NWNX_Player_GetServerVaultCharacters(oPC);
    NWNX_Tests_Report("NWNX_Player", "GetServerVaultCharacters", JsonGetType(jChars) == JSON_TYPE_ARRAY);
    string sCurrent;
    int i, n = JsonGetLength(jChars);
    for (i = 0; i < n; i++)
    {
        json jChar = JsonArrayGet(jChars, i);
        if (JsonGetInt(JsonObjectGet(jChar, "current")))
            sCurrent = JsonGetString(JsonObjectGet(jChar, "resref"));
    }
    if (n > 0)
    {
        NWNX_Tests_Report("NWNX_Player", "GetServerVaultCharacters (current)", sCurrent != "");
        NWNX_Tests_Report("NWNX_Player", "SetNextCharacter (current)", !NWNX_Player_SetNextCharacter(oPC, sCurrent));
    }
    NWNX_Tests_Report("NWNX_Player", "SetNextCharacter (missing)", !NWNX_Player_SetNextCharacter(oPC, "nwnx_no_such_bic"));
    NWNX_Tests_Report("NWNX_Player", "SetNextCharacter (path)", !NWNX_Player_SetNextCharacter(oPC, "../x"));

    WriteTimestampedLogEntry("NWNX_Player unit test end.");
}
