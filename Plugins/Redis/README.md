@page redis Readme
@ingroup redis 

This plugin allows you to connect to a redis server. If you don't know what redis is, I'd suggest reading up here in detail: redis.io.

Key features:

* This plugin supports almost all commands
* Including pubsub with script callbacks. You can use this to connect external services, even websites, to your nwserver in a non-blocking way.
* Also great for linking servers.
* Redis makes for a very good and fast key-value store, with some advanced data structures available. You can even use it as a queue if so desired.

TODO:

* SCO/RCO, so resman can automatically load resources from redis without user intervention. This requires a more generic interface/hook in core.

## Hints

* There are two includes, `nwnx_redis` and `nwnx_redis_short`. They do the same, but `_short` is not prefixed with `NWNX_Redis`.
* `nwnx_redis_ps.nss` and `on_pubsub.nss` are not needed if PubSub functionality is not being used.

## Setup

* Set up redis-server. Start it. This should be as simple as apt-get install redis. There's also a docker image.
* Configure `NWNX_REDIS_HOST` and `NWNX_REDIS_PORT` for nwnx. `NWNX_REDIS_PORT` is not necessary if the default redis port is used.
* Include the .nss files in NWScript/ into your module.

## Accessing Redis from nwscript

To use Redis commands in nwscript include `nwnx_redis` and `nwnx_redis_lib`. The nwnx_redis include contains the redis commands like, for example, `int NWNX_Redis_GET(string key)`. All of the nwnx redis comands return an int which is the resultId. The nwnx_redis_lib include contains functions to get the data (and type) you can work with for this resultId. Three of these functions are:
* int NWNX_Redis_GetResultAsInt(int resultId)
* string NWNX_Redis_GetResultAsString(int resultId)
* float NWNX_Redis_GetResultAsFloat(int resultId)

There are also functions to work with result arrays and one to get the type of a result. You can look those up in [nwnx_redis_lib.nss](../NWScript/nwnx_redis_lib.nss).

`NWNX_Redis_GetResultAs<Type>` has to be called on all the commands that return a resultId even if the actual value is an int as well. The following is an example to set, check for existance of and get a key.
```c
#include "nwnx_redis"
#include "nwnx_redis_lib"

NWNX_Redis_SET("examples:examplekey", "testvalue");
if (NWNX_Redis_GetResultAsInt(NWNX_Redis_EXISTS("examples:examplekey")))
{
  string sValue = NWNX_Redis_GetResultAsString(NWNX_Redis_GET("examples:examplekey"));
  WriteTimestampedLogEntry("Value of redis key 'examples:examplekey': " + sValue);
}
```

## Getting started with PubSub

* Create a script called "on_pubsub" (or rename it through `NWNX_REDIS_PUBSUB_SCRIPT`). An example is included in NWScript/.
* Configure `NWNX_REDIS_PUBSUB_CHANNELS` to be "test".
* Hint: Subscriptions support wildcards.
* Start your module.
* Use `redis-cli` to connect to your running redis server and type `PUBLISH test hi there`.
* Hint: `redis-cli monitor` in a separate shell is a great way to figure out what's going on.
* Your script should trigger with the payload available through `NWNX_Redis_GetPubSubMessageData()`.

### Moving on

* Now think of a good naming scheme for your various pubsub channels. Keep traffic as low as feasible, since each incoming message will trigger a script run. There is no batching yet.
* Hint: By convention, a good namespace separator for channels is "."; for keys ":".
* Example: `NWNX_Redis_PUBLISH("nwserver.players.join", GetPCPlayerName(..));`
* Hint: A good pattern is to store data in a redis key named after the channel and object identifier (i.e. `HSET nwserver:players:PlayerName:.lastSeen 1234`) and then trigger a PubSub message with the same subject (`PUBLISH nwserver.players.joins PlayerName`). This cuts down on wire overhead.

## Environment Variables

| Variable Name                |  Type                   | Default Value                      |
| ---------------------------- | :---------------------: | ---------------------------------- |
| `NWNX_REDIS_HOST`            | string                  | (none)                             |
| `NWNX_REDIS_PORT`            | int16                   | 6379                               |
| `NWNX_REDIS_PUBSUB_SCRIPT`   | string                  | on_pubsub                          |
| `NWNX_REDIS_PUBSUB_CHANNELS` | comma-separated strings | ""                                 |

## Opt-in client: redis-plus-plus (experimental)

By default the plugin uses the bundled cpp_redis client. Set `NWNX_REDIS_CLIENT=redis-plus-plus` to use [redis-plus-plus](https://github.com/sewenew/redis-plus-plus) on [hiredis](https://github.com/redis/hiredis) instead (both bundled and linked statically). NWScript functions, result types and pubsub work the same; the new client adds:

* a connection pool that reconnects on its own after Redis restarts, and a pubsub connection that resubscribes;
* ACL users and passwords (`NWNX_REDIS_USERNAME`, `NWNX_REDIS_PASSWORD`), database selection, TLS;
* connect and read timeouts, so a hung Redis returns an error (`NWNX_REDIS_RESULT_ERROR`) instead of freezing the server.

Commands from NWScript still run synchronously on the game's main thread: while Redis is unreachable each command returns an error, and while it is hung each command can block for up to `NWNX_REDIS_SOCKET_TIMEOUT_MS` (after one timeout, commands fail at once for `NWNX_REDIS_FAILFAST_MS`).

| Variable Name                     | Type   | Default     | Notes |
| --------------------------------- | :----: | ----------- | ----- |
| `NWNX_REDIS_CLIENT`               | string | `cpp_redis` | `redis-plus-plus` to opt in |
| `NWNX_REDIS_USERNAME`             | string | ""          | ACL user; empty for `requirepass`/`default` |
| `NWNX_REDIS_PASSWORD`             | string | ""          | |
| `NWNX_REDIS_DATABASE`             | int    | 0           | |
| `NWNX_REDIS_TLS`                  | bool   | false       | |
| `NWNX_REDIS_TLS_CACERT`           | string | ""          | CA file to verify the server |
| `NWNX_REDIS_TLS_CACERTDIR`        | string | ""          | |
| `NWNX_REDIS_TLS_CERT`             | string | ""          | client certificate (mutual TLS) |
| `NWNX_REDIS_TLS_KEY`              | string | ""          | client key (mutual TLS) |
| `NWNX_REDIS_TLS_SNI`              | string | ""          | server name to send and verify |
| `NWNX_REDIS_TLS_VERIFY`           | bool   | true        | false skips certificate verification |
| `NWNX_REDIS_POOL_SIZE`            | int    | 4           | |
| `NWNX_REDIS_CONNECT_TIMEOUT_MS`   | int    | 1000        | |
| `NWNX_REDIS_SOCKET_TIMEOUT_MS`    | int    | 5000        | 0 = wait forever; must exceed the timeout of blocking commands (BLPOP...) |
| `NWNX_REDIS_FAILFAST_MS`          | int    | 3000        | 0 = off |
| `NWNX_REDIS_POOL_WAIT_TIMEOUT_MS` | int    | 1000        | |
| `NWNX_REDIS_RESP`                 | int    | 2           | 3 = RESP3; maps, sets, doubles and booleans are folded into arrays, strings and integers |
