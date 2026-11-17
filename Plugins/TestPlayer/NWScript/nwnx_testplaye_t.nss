#include "nwnx_testplayer"
#include "nwnx_tests"

void main()
{
    WriteTimestampedLogEntry("NWNX_TestPlayer unit test begin..");

    object oCreature = CreateObject(OBJECT_TYPE_CREATURE, "nw_chicken", GetStartingLocation());
    if (!GetIsObjectValid(oCreature))
    {
        WriteTimestampedLogEntry("NWNX_TestPlayer test: Failed to create creature");
        return;
    }

    int nCount = NWNX_TestPlayer_GetCount();
    int nPlayerId = NWNX_TestPlayer_Attach(oCreature, "TestPlayer", "TESTKEY1");
    NWNX_Tests_Report("NWNX_TestPlayer", "Attach", nPlayerId > 0);
    if (!nPlayerId)
    {
        WriteTimestampedLogEntry("NWNX_TestPlayer test: Failed to attach, is NWNX_TESTPLAYER_ENABLE set?");
        DestroyObject(oCreature);
        return;
    }

    NWNX_Tests_Report("NWNX_TestPlayer", "Attach (GetIsPC)", GetIsPC(oCreature));
    NWNX_Tests_Report("NWNX_TestPlayer", "Attach (GetPCPlayerName)", GetPCPlayerName(oCreature) == "TestPlayer");
    NWNX_Tests_Report("NWNX_TestPlayer", "Attach (GetPCPublicCDKey)", GetPCPublicCDKey(oCreature) == "TESTKEY1");
    NWNX_Tests_Report("NWNX_TestPlayer", "Attach (Twice)", NWNX_TestPlayer_Attach(oCreature, "TestPlayer2", "TESTKEY2") == 0);
    NWNX_Tests_Report("NWNX_TestPlayer", "GetIsTestPlayer", NWNX_TestPlayer_GetIsTestPlayer(oCreature));
    NWNX_Tests_Report("NWNX_TestPlayer", "GetPlayerId", NWNX_TestPlayer_GetPlayerId(oCreature) == nPlayerId);
    NWNX_Tests_Report("NWNX_TestPlayer", "GetCount", NWNX_TestPlayer_GetCount() == nCount + 1);

    NWNX_TestPlayer_ClearMessages(oCreature);
    SendMessageToPC(oCreature, "NWNX_TestPlayer test");
    NWNX_Tests_Report("NWNX_TestPlayer", "GetMessageCount", NWNX_TestPlayer_GetMessageCount(oCreature, 18) == 1);
    NWNX_Tests_Report("NWNX_TestPlayer", "ClearMessages", NWNX_TestPlayer_ClearMessages(oCreature) && NWNX_TestPlayer_GetMessageCount(oCreature, 18) == 0);

    NWNX_Tests_Report("NWNX_TestPlayer", "SetInventoryOpen", NWNX_TestPlayer_SetInventoryOpen(oCreature, TRUE));

    NWNX_Tests_Report("NWNX_TestPlayer", "Detach", NWNX_TestPlayer_Detach(oCreature));
    NWNX_Tests_Report("NWNX_TestPlayer", "Detach (GetIsPC)", !GetIsPC(oCreature));
    NWNX_Tests_Report("NWNX_TestPlayer", "Detach (GetIsTestPlayer)", !NWNX_TestPlayer_GetIsTestPlayer(oCreature));
    NWNX_Tests_Report("NWNX_TestPlayer", "Detach (GetCount)", NWNX_TestPlayer_GetCount() == nCount);
    NWNX_Tests_Report("NWNX_TestPlayer", "Detach (Twice)", !NWNX_TestPlayer_Detach(oCreature));

    DestroyObject(oCreature);

    WriteTimestampedLogEntry("NWNX_TestPlayer unit test end.");
}
