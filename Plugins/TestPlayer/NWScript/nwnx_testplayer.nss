/// @addtogroup testplayer TestPlayer
/// @brief Fake players for headless test servers. Never enable on a live server.
/// @{
/// @file nwnx_testplayer.nss

const string NWNX_TestPlayer = "NWNX_TestPlayer"; ///< @private

/// @brief A message the server sent to a fake player.
struct NWNX_TestPlayer_Message
{
    int nMajor; ///< The major type of the message, or -1 if there is no such message.
    int nMinor; ///< The minor type of the message.
    int nSize; ///< The size of the message in bytes, without its type.
    string sData; ///< The first 1024 bytes of the message in hexadecimal, without its type.
};

/// @brief Make a creature a player character with no client behind it.
/// @note Fails unless the environment variable `NWNX_TESTPLAYER_ENABLE` is true, and on a public server unless
/// `NWNX_TESTPLAYER_ALLOW_ON_PUBLIC_SERVER` is true as well.
/// @param oCreature An existing creature that is not controlled by a player.
/// @param sPlayerName The player name, as returned by GetPCPlayerName(). Not empty or in use by another player.
/// @param sCDKey The public cd key, as returned by GetPCPublicCDKey(). Not empty or in use by another player.
/// @return The fake player's id, or 0 on failure.
int NWNX_TestPlayer_Attach(object oCreature, string sPlayerName, string sCDKey);

/// @brief Turn a fake player's creature back into an ordinary creature.
/// @note BootPC() does the same.
/// @param oCreature The fake player's creature.
/// @return TRUE if oCreature had a fake player.
int NWNX_TestPlayer_Detach(object oCreature);

/// @brief Complete the area transition of a fake player, as a client does when it has loaded the new area.
/// @note A player that is moved to another area leaves its area and only enters the new one when its client has
/// loaded it. Until then GetArea() returns OBJECT_INVALID for the creature and its action queue waits.
/// @param oCreature The fake player's creature.
/// @return TRUE if oCreature was between areas and is now in the new one.
int NWNX_TestPlayer_CompleteAreaTransition(object oCreature);

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
/// @note Counts start when the player is attached or NWNX_TestPlayer_ClearMessages() is called. They include the
/// messages sent to all players at once.
/// The types are the MessageMajor and Message*Minor constants in NWNXLib/API/Constants/Messages.hpp.
/// @param oCreature The fake player's creature.
/// @param nMajor The major type of the messages.
/// @param nMinor The minor type of the messages, or -1 for all of them.
/// @return The number of messages, or 0 if oCreature has no fake player.
int NWNX_TestPlayer_GetMessageCount(object oCreature, int nMajor, int nMinor = -1);

/// @brief Get the number of messages stored for a fake player.
/// @note The most recent messages sent to each fake player are stored, up to `NWNX_TESTPLAYER_MESSAGE_STORE_SIZE` of
/// them. Then the oldest is dropped for each new one, which shifts the indexes of the others.
/// @param oCreature The fake player's creature.
/// @return The number of stored messages, or 0 if oCreature has no fake player.
int NWNX_TestPlayer_GetStoredMessageCount(object oCreature);

/// @brief Get a message stored for a fake player.
/// @param oCreature The fake player's creature.
/// @param nIndex The index of the message, from 0 for the oldest to NWNX_TestPlayer_GetStoredMessageCount() - 1.
/// @return A NWNX_TestPlayer_Message struct, with nMajor -1 if there is no such message.
struct NWNX_TestPlayer_Message NWNX_TestPlayer_GetStoredMessage(object oCreature, int nIndex);

/// @brief Find a message stored for a fake player by the text in it.
/// @note Strings are sent as they are, so this finds the text of SendMessageToPC(), SpeakString() and so on.
/// @param oCreature The fake player's creature.
/// @param sText The text to look for in the first 1024 bytes of the message. An empty string matches any message.
/// @param nMajor The major type of the message, or -1 for any.
/// @param nMinor The minor type of the message, or -1 for any.
/// @param nStart The index of the first message to look at.
/// @return The index of the first matching message, or -1 if there is none.
int NWNX_TestPlayer_FindStoredMessage(object oCreature, string sText, int nMajor = -1, int nMinor = -1, int nStart = 0);

/// @brief Reset the message counts of a fake player and drop its stored messages.
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

int NWNX_TestPlayer_CompleteAreaTransition(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "CompleteAreaTransition");
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

int NWNX_TestPlayer_GetStoredMessageCount(object oCreature)
{
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "GetStoredMessageCount");
    return NWNXPopInt();
}

struct NWNX_TestPlayer_Message NWNX_TestPlayer_GetStoredMessage(object oCreature, int nIndex)
{
    NWNXPushInt(nIndex);
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "GetStoredMessage");

    struct NWNX_TestPlayer_Message str;
    str.sData = NWNXPopString();
    str.nSize = NWNXPopInt();
    str.nMinor = NWNXPopInt();
    str.nMajor = NWNXPopInt();
    return str;
}

int NWNX_TestPlayer_FindStoredMessage(object oCreature, string sText, int nMajor = -1, int nMinor = -1, int nStart = 0)
{
    NWNXPushInt(nStart);
    NWNXPushInt(nMinor);
    NWNXPushInt(nMajor);
    NWNXPushString(sText);
    NWNXPushObject(oCreature);
    NWNXCall(NWNX_TestPlayer, "FindStoredMessage");
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
