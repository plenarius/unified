/// @addtogroup testplayer TestPlayer
/// @brief Fake players for headless test servers. Never enable on a live server.
/// @{
/// @file nwnx_testplayer.nss

const string NWNX_TestPlayer = "NWNX_TestPlayer"; ///< @private

/// @brief Make oCreature a player character with no client behind it.
/// @param oCreature An existing creature (not already controlled by a player).
/// @param sPlayerName What GetPCPlayerName() returns.
/// @param sCDKey What GetPCPublicCDKey() returns.
/// @return The fake player's id, or 0 on failure.
int NWNX_TestPlayer_Attach(object oCreature, string sPlayerName, string sCDKey);

/// @brief Turn a fake player's creature back into an ordinary creature.
/// @return TRUE if oCreature had a fake player.
int NWNX_TestPlayer_Detach(object oCreature);

/// @return TRUE if oCreature is controlled by a fake player.
int NWNX_TestPlayer_GetIsFake(object oCreature);

/// @}

int NWNX_TestPlayer_Attach(object oCreature, string sPlayerName, string sCDKey)
{
    NWNXPushString(sCDKey);
    NWNXPushString(sPlayerName);
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "Attach");
    return NWNXPopInt();
}

int NWNX_TestPlayer_Detach(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "Detach");
    return NWNXPopInt();
}

int NWNX_TestPlayer_GetIsFake(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "GetIsFake");
    return NWNXPopInt();
}
