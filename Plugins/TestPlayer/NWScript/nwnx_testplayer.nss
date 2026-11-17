/// @addtogroup testplayer TestPlayer
/// @brief Fake players for headless test servers. Never enable on a live server.
/// @{
/// @file nwnx_testplayer.nss

const string NWNX_TestPlayer = "NWNX_TestPlayer"; ///< @private

/// @brief Make a creature a player character with no client behind it.
/// @note Fails unless the environment variable `NWNX_TESTPLAYER_ENABLE` is true.
/// @param oCreature An existing creature that is not controlled by a player.
/// @param sPlayerName The player name, as returned by GetPCPlayerName().
/// @param sCDKey The public cd key, as returned by GetPCPublicCDKey().
/// @return The fake player's id, or 0 on failure.
int NWNX_TestPlayer_Attach(object oCreature, string sPlayerName, string sCDKey);

/// @brief Turn a fake player's creature back into an ordinary creature.
/// @param oCreature The fake player's creature.
/// @return TRUE if oCreature had a fake player.
int NWNX_TestPlayer_Detach(object oCreature);

/// @brief Get if a creature is controlled by a fake player.
/// @param oCreature The creature.
/// @return TRUE if oCreature is controlled by a fake player.
int NWNX_TestPlayer_GetIsTestPlayer(object oCreature);

/// @brief Get the player id of a fake player.
/// @param oCreature The fake player's creature.
/// @return The fake player's id, or 0 if oCreature has none.
int NWNX_TestPlayer_GetPlayerId(object oCreature);

/// @brief Get the number of fake players.
/// @return The number of fake players.
int NWNX_TestPlayer_GetCount();

/// @brief Count the messages the server has sent to a fake player, by message type.
/// @note Counts start when the player is attached or NWNX_TestPlayer_ClearMessages() is called.
/// The types are the MessageMajor and Message*Minor constants in NWNXLib/API/Constants/Messages.hpp.
/// @param oCreature The fake player's creature.
/// @param nMajor The major type of the messages.
/// @param nMinor The minor type of the messages, or -1 for all of them.
/// @return The number of messages, or 0 if oCreature has no fake player.
int NWNX_TestPlayer_GetMessageCount(object oCreature, int nMajor, int nMinor = -1);

/// @brief Reset the message counts of a fake player.
/// @param oCreature The fake player's creature.
/// @return TRUE if oCreature had a fake player.
int NWNX_TestPlayer_ClearMessages(object oCreature);

/// @brief Set whether the server believes a fake player has its inventory window open.
/// @note A real client reports this itself.
/// @param oCreature The fake player's creature.
/// @param bOpen TRUE for open, FALSE for closed.
/// @return TRUE if oCreature had a fake player.
int NWNX_TestPlayer_SetInventoryOpen(object oCreature, int bOpen = TRUE);

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

int NWNX_TestPlayer_GetIsTestPlayer(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "GetIsTestPlayer");
    return NWNXPopInt();
}

int NWNX_TestPlayer_GetPlayerId(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "GetPlayerId");
    return NWNXPopInt();
}

int NWNX_TestPlayer_GetCount()
{
    NWNXCall(NWNX_TestPlayer, "GetCount");
    return NWNXPopInt();
}

int NWNX_TestPlayer_GetMessageCount(object oCreature, int nMajor, int nMinor = -1)
{
    NWNXPushInt(nMinor);
    NWNXPushInt(nMajor);
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "GetMessageCount");
    return NWNXPopInt();
}

int NWNX_TestPlayer_ClearMessages(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "ClearMessages");
    return NWNXPopInt();
}

int NWNX_TestPlayer_SetInventoryOpen(object oCreature, int bOpen = TRUE)
{
    NWNXPushInt(bOpen);
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "SetInventoryOpen");
    return NWNXPopInt();
}
