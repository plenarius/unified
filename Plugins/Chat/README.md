@page chat Readme
@ingroup chat

Allows chat events to be captured, skipped, and manual chat messages to be dispatched.

## Environment Variables

| Variable Name | Value | Notes |
| ------------- | :---: | ----- |
| `NWNX_CHAT_CHAT_SCRIPT` | string | Set the nwscript that receives all the chat messages
| `NWNX_CHAT_LOG_CHANNELS` | channel list | Only these channels are logged. Default: all.
| `NWNX_CHAT_LOG_CONTENT_CHANNELS` | channel list | Only these channels are logged with their text. Default: all but tells, which never include their text unless named here.
| `NWNX_CHAT_LOG_MESSAGE_LEVEL` | 2-7 | The level chat messages are logged at, checked against `NWNX_CHAT_LOG_LEVEL`. Default: 7 (debug).

Channel lists are comma separated names: `talk`, `shout`, `whisper`, `tell`, `server`, `party`, `dm`, `dm_talk`, `dm_shout`, `dm_whisper`, `dm_tell`, `dm_party`, `dm_dm`. For example, to log public chat at info level without tells: `NWNX_CHAT_LOG_CHANNELS=talk,shout,whisper,party`, `NWNX_CHAT_LOG_MESSAGE_LEVEL=6`, `NWNX_CHAT_LOG_LEVEL=6`.

In JSON logs (`NWNX_CORE_LOG_FORMAT=json`) a chat message is a `chat message` record with `channel`, `channel_id`, `speaker`, `speaker_name`, `skipped`, `target` (tells) and `text`.
