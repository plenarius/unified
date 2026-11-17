@page testplayer Readme
@ingroup testplayer

Creates fake players: a player with no client behind it, controlling an existing creature. Scripts that only work on player characters (`GetIsPC()`, `GetFirstPC()`, `GetPCPlayerName()`, `GetPCPublicCDKey()`, ...) can then be tested on a headless server without anyone logging in.

**Never enable this on a live server.** A fake player is counted as a player everywhere: it takes a player slot, it is in the player count the server advertises and in the player count of its area.

## Environment Variables

| Variable Name | Value | Default | Notes |
| ------------- | :---: | :-----: | ----- |
| `NWNX_TESTPLAYER_ENABLE` | true or false | false | The plugin does nothing unless this is true. |
| `NWNX_TESTPLAYER_ALLOW_ON_PUBLIC_SERVER` | true or false | false | `NWNX_TestPlayer_Attach()` fails on a server that posts itself to the internet (`-publicserver 1`) unless this is true. |
| `NWNX_TESTPLAYER_MESSAGE_STORE_SIZE` | 0 to 4096 | 256 | How many of the messages sent to each fake player are stored. Each takes up to 1 KiB. |

## Notes

* A fake player takes a free player slot, from the highest one (254) down to 192. Real clients get the lowest free slots. Its player name and public cd key can't be empty or in use by another player.
* `NWNX_TestPlayer_Attach()` does not run the module's OnClientEnter event, and `NWNX_TestPlayer_Detach()` and `BootPC()` do not run OnClientLeave.
* The area a fake player is in counts it as a player, so the area runs at its normal AI level and `DestroyArea()` refuses to destroy it.
* A player character has one record per level, which a creature made from a blueprint does not. `NWNX_TestPlayer_Attach()` adds the missing ones, each with its class and a share of the creature's hit points, so its maximum hit points stay the same and it can be saved (`ObjectToJson()`, `CopyObject()`, ...). No feats, skill ranks or spells are recorded for a level. The records stay after `NWNX_TestPlayer_Detach()`.
* Everything the server sends to a fake player is dropped. `NWNX_TestPlayer_GetMessageCount()` counts those messages by type, for example major 18 (client side message) for `SendMessageToPC()`. The most recent ones can be read with `NWNX_TestPlayer_GetStoredMessage()` and searched for text with `NWNX_TestPlayer_FindStoredMessage()`.
* Nothing answers for the client, so anything that waits for one (conversations, GUI and NUI events) never completes. The exception is changing area: a fake player jumped to another area leaves its area and arrives when `NWNX_TestPlayer_CompleteAreaTransition()` is called.
* A fake player's character is never saved to the vault: `ExportSingleCharacter()` and `ExportAllCharacters()` skip it.
* `BootPC()` detaches a fake player. As for any player, `DestroyObject()` does nothing to its creature. If the creature is destroyed some other way, the fake player is detached.
* All fake players are detached when the server shuts down.
