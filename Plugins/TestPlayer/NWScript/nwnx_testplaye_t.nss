#include "nwnx_testplayer"
#include "nwnx_area"
#include "nwnx_tests"

// These tests need NWNX_TESTPLAYER_ENABLE, the Area plugin and the default NWNX_TESTPLAYER_MESSAGE_STORE_SIZE.

void Report(string sTestName, int bSuccess)
{
    NWNX_Tests_Report("NWNX_TestPlayer", sTestName, bSuccess);
}

void TestDetach(object oCreature, object oArea, int nCount)
{
    Report("BootPC", !NWNX_TestPlayer_GetIsTestPlayer(oCreature) && !GetIsPC(oCreature));
    Report("BootPC (GetCount)", NWNX_TestPlayer_GetCount() == nCount);
    Report("BootPC (Players In Area)", NWNX_Area_GetNumberOfPlayersInArea(oArea) == 0);

    Report("Attach (Again)", NWNX_TestPlayer_Attach(oCreature, "TestPlayer", "TESTKEY1") > 0);
    Report("Attach (Again, Players In Area)", NWNX_Area_GetNumberOfPlayersInArea(oArea) == 1);
    Report("DestroyArea (Players In Area)", DestroyArea(oArea) == -2);

    Report("Detach", NWNX_TestPlayer_Detach(oCreature));
    Report("Detach (GetIsPC)", !GetIsPC(oCreature));
    Report("Detach (GetIsTestPlayer)", !NWNX_TestPlayer_GetIsTestPlayer(oCreature));
    Report("Detach (GetCount)", NWNX_TestPlayer_GetCount() == nCount);
    Report("Detach (Players In Area)", NWNX_Area_GetNumberOfPlayersInArea(oArea) == 0);
    Report("Detach (Twice)", !NWNX_TestPlayer_Detach(oCreature));
    Report("DestroyArea", DestroyArea(oArea) == 1);

    WriteTimestampedLogEntry("NWNX_TestPlayer unit test end.");
}

void TestAreaTransition(object oCreature, object oFrom, object oTo, int nPlayers, int nCount)
{
    Report("JumpToLocation (Between Areas)", !GetIsObjectValid(GetArea(oCreature)));
    Report("JumpToLocation (Players In Area)", NWNX_Area_GetNumberOfPlayersInArea(oFrom) == nPlayers && NWNX_Area_GetNumberOfPlayersInArea(oTo) == 0);
    Report("CompleteAreaTransition", NWNX_TestPlayer_CompleteAreaTransition(oCreature) && GetArea(oCreature) == oTo);
    Report("CompleteAreaTransition (Players In Area)", NWNX_Area_GetNumberOfPlayersInArea(oFrom) == nPlayers && NWNX_Area_GetNumberOfPlayersInArea(oTo) == 1);
    Report("CompleteAreaTransition (Twice)", !NWNX_TestPlayer_CompleteAreaTransition(oCreature));

    BootPC(oCreature);
    DelayCommand(2.0, TestDetach(oCreature, oTo, nCount));
}

void main()
{
    WriteTimestampedLogEntry("NWNX_TestPlayer unit test begin..");

    location locStart = GetStartingLocation();
    object oArea = GetAreaFromLocation(locStart);
    object oCreature = CreateObject(OBJECT_TYPE_CREATURE, "nw_chicken", locStart);
    object oOgre = CreateObject(OBJECT_TYPE_CREATURE, "nw_ogrechief01", locStart);
    if (!GetIsObjectValid(oCreature) || !GetIsObjectValid(oOgre))
    {
        WriteTimestampedLogEntry("NWNX_TestPlayer test: Failed to create creature");
        return;
    }

    int nCount = NWNX_TestPlayer_GetCount();
    int nPlayers = NWNX_Area_GetNumberOfPlayersInArea(oArea);
    Report("Attach (Empty Player Name)", NWNX_TestPlayer_Attach(oCreature, "", "TESTKEY1") == 0);
    Report("Attach (Empty CD Key)", NWNX_TestPlayer_Attach(oCreature, "TestPlayer", "") == 0);

    int nPlayerId = NWNX_TestPlayer_Attach(oCreature, "TestPlayer", "TESTKEY1");
    Report("Attach", nPlayerId > 0);
    if (!nPlayerId)
    {
        WriteTimestampedLogEntry("NWNX_TestPlayer test: Failed to attach, is NWNX_TESTPLAYER_ENABLE set and the server not public?");
        DestroyObject(oCreature);
        DestroyObject(oOgre);
        return;
    }

    Report("Attach (GetIsPC)", GetIsPC(oCreature));
    Report("Attach (GetPCPlayerName)", GetPCPlayerName(oCreature) == "TestPlayer");
    Report("Attach (GetPCPublicCDKey)", GetPCPublicCDKey(oCreature) == "TESTKEY1");
    Report("Attach (Twice)", NWNX_TestPlayer_Attach(oCreature, "TestPlayer2", "TESTKEY2") == 0);
    Report("Attach (Player Name In Use)", NWNX_TestPlayer_Attach(oOgre, "TestPlayer", "TESTKEY2") == 0);
    Report("Attach (CD Key In Use)", NWNX_TestPlayer_Attach(oOgre, "TestPlayer2", "TESTKEY1") == 0);
    Report("Attach (Players In Area)", NWNX_Area_GetNumberOfPlayersInArea(oArea) == nPlayers + 1);
    Report("GetIsTestPlayer", NWNX_TestPlayer_GetIsTestPlayer(oCreature));
    Report("GetPlayerId", NWNX_TestPlayer_GetPlayerId(oCreature) == nPlayerId);
    Report("GetCount", NWNX_TestPlayer_GetCount() == nCount + 1);

    int nLevels = GetHitDice(oOgre);
    int nHitPoints = GetMaxHitPoints(oOgre);
    Report("Attach (Level 8)", nLevels == 8 && NWNX_TestPlayer_Attach(oOgre, "TestPlayer2", "TESTKEY2") > 0);
    json jLevels = JsonObjectGet(JsonObjectGet(ObjectToJson(oOgre), "LvlStatList"), "value");
    Report("Attach (ObjectToJson)", JsonGetLength(jLevels) == nLevels);
    int nLevel, nHitDice;
    for (nLevel = 0; nLevel < JsonGetLength(jLevels); nLevel++)
        nHitDice += JsonGetInt(JsonObjectGet(JsonObjectGet(JsonArrayGet(jLevels, nLevel), "LvlStatHitDie"), "value"));
    Report("Attach (Hit Dice)", nHitDice > 0 && nHitDice + nLevels * GetAbilityModifier(ABILITY_CONSTITUTION, oOgre) == nHitPoints);
    Report("Attach (GetMaxHitPoints)", GetMaxHitPoints(oOgre) == nHitPoints);
    object oCopy = CopyObject(oOgre, locStart);
    Report("Attach (CopyObject)", GetIsObjectValid(oCopy) && !GetIsPC(oCopy) && GetHitDice(oCopy) == nLevels);
    DestroyObject(oCopy);
    Report("Detach (Level 8)", NWNX_TestPlayer_Detach(oOgre) && GetMaxHitPoints(oOgre) == nHitPoints);
    DestroyObject(oOgre);

    NWNX_TestPlayer_ClearMessages(oCreature);
    SendMessageToPC(oCreature, "NWNX_TestPlayer test");
    Report("GetMessageCount", NWNX_TestPlayer_GetMessageCount(oCreature, 18) == 1);
    Report("GetStoredMessageCount", NWNX_TestPlayer_GetStoredMessageCount(oCreature) == 1);
    Report("FindStoredMessage", NWNX_TestPlayer_FindStoredMessage(oCreature, "NWNX_TestPlayer test") == 0);
    Report("FindStoredMessage (Type)", NWNX_TestPlayer_FindStoredMessage(oCreature, "", 18) == 0 && NWNX_TestPlayer_FindStoredMessage(oCreature, "", 9) == -1);
    Report("FindStoredMessage (No Match)", NWNX_TestPlayer_FindStoredMessage(oCreature, "no such text") == -1);
    struct NWNX_TestPlayer_Message msg = NWNX_TestPlayer_GetStoredMessage(oCreature, 0);
    Report("GetStoredMessage", msg.nMajor == 18 && msg.nSize * 2 == GetStringLength(msg.sData) && FindSubString(msg.sData, "4e574e585f54657374506c617965722074657374") > 0);
    msg = NWNX_TestPlayer_GetStoredMessage(oCreature, 1);
    Report("GetStoredMessage (No Message)", msg.nMajor == -1);

    int nMessage;
    for (nMessage = 1; nMessage <= 300; nMessage++)
        SendMessageToPC(oCreature, "message " + IntToString(nMessage) + ".");
    Report("GetMessageCount (Many)", NWNX_TestPlayer_GetMessageCount(oCreature, 18) == 301);
    Report("GetStoredMessageCount (Many)", NWNX_TestPlayer_GetStoredMessageCount(oCreature) == 256);
    Report("FindStoredMessage (Many)", NWNX_TestPlayer_FindStoredMessage(oCreature, "message 44.") == -1 && NWNX_TestPlayer_FindStoredMessage(oCreature, "message 300.") == 255);
    Report("ClearMessages", NWNX_TestPlayer_ClearMessages(oCreature) && NWNX_TestPlayer_GetMessageCount(oCreature, 18) == 0 && NWNX_TestPlayer_GetStoredMessageCount(oCreature) == 0);

    Report("SetInventoryOpen", NWNX_TestPlayer_SetInventoryOpen(oCreature, TRUE));

    object oTo = CreateArea(GetResRef(oArea));
    Report("CompleteAreaTransition (Not Between Areas)", GetIsObjectValid(oTo) && !NWNX_TestPlayer_CompleteAreaTransition(oCreature));
    AssignCommand(oCreature, JumpToLocation(Location(oTo, GetPositionFromLocation(locStart), 0.0)));
    DelayCommand(2.0, TestAreaTransition(oCreature, oArea, oTo, nPlayers, nCount));
}
