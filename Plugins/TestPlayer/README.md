@page testplayer Readme
@ingroup testplayer

Creates fake players: a player with no client behind it, controlling an existing creature. Scripts that only work on player characters (`GetIsPC()`, `GetFirstPC()`, `GetPCPlayerName()`, `GetPCPublicCDKey()`, ...) can then be tested on a headless server without anyone logging in.

**Never enable this on a live server.** A fake player is counted as a player everywhere, including the player count the server advertises.

## Environment Variables

| Variable Name | Value | Default | Notes |
| ------------- | :---: | :-----: | ----- |
| `NWNX_TESTPLAYER_ENABLE` | true or false | false | The plugin does nothing unless this is true. |

## Notes

* A fake player takes a free player slot, from the highest one (254) down to 192. Real clients get the lowest free slots.
* `NWNX_TestPlayer_Attach()` does not run the module's OnClientEnter event and `NWNX_TestPlayer_Detach()` does not run OnClientLeave.
* Everything the server sends to a fake player is dropped. `NWNX_TestPlayer_GetMessageCount()` counts those messages by type, for example major 18 (client side message) for `SendMessageToPC()`. What is inside a message can't be read.
* Nothing answers for the client, so anything that waits for one (conversations, GUI and NUI events) never completes. That includes changing area: a fake player jumped to another area leaves its area and never arrives.
* A fake player's character is never saved: `ExportSingleCharacter()` and `ExportAllCharacters()` skip it.
* `BootPC()` does nothing to a fake player and, as for any player, `DestroyObject()` does nothing to its creature. Detach it first.
* All fake players are detached when the server shuts down.
