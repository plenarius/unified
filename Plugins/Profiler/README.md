@page profiler Readme
@ingroup profiler 

Acquires shared hooks to expose various useful metrics.

## Environment Variables

| Variable Name                  |   Type   | Default Value |
| -------------                  | :------: | ------------- |
| NWNX_PROFILER_ENABLE_OVERHEAD_COMPENSATION   | bool     | true    |
| NWNX_PROFILER_OVERHEAD_COMPENSATION_FORCE     | int64_t  | _none_  |
| NWNX_PROFILER_OVERHEAD_COMPENSATION_RUNS     | size_t   | 500     |
| NWNX_PROFILER_OVERHEAD_COMPENSATION_RECALIBRATE   | bool   | false   |
| NWNX_PROFILER_OVERHEAD_COMPENSATION_RECALIBRATION_PERIOD | uint32_t | 1000 |
| NWNX_PROFILER_ENABLE_AI_MASTER_UPDATES       | bool     | true    |
| NWNX_PROFILER_AI_MASTER_UPDATES_OVERKILL     | bool     | false   |
| NWNX_PROFILER_ENABLE_MAIN_LOOP               | bool     | true    |
| NWNX_PROFILER_ENABLE_NET_LAYER               | bool     | true    |
| NWNX_PROFILER_ENABLE_NET_MESSAGES            | bool     | true    |
| NWNX_PROFILER_ENABLE_OBJECT_AI_UPDATES       | bool     | false   |
| NWNX_PROFILER_ENABLE_OBJECT_EVENT_HANDLERS   | bool     | false   |
| NWNX_PROFILER_ENABLE_PATHING                 | bool     | true    |
| NWNX_PROFILER_ENABLE_PATHING_OUTCOMES        | bool     | false   |
| NWNX_PROFILER_PATHING_OUTCOMES_INTERVAL      | uint32_t | 60      |
| NWNX_PROFILER_PATHING_LOG_SLOW_MS            | uint32_t | 0       |
| NWNX_PROFILER_PATHING_LOG_FAILED             | bool     | false   |
| NWNX_PROFILER_PATHING_LOG_FAILED_MIN_AI_LEVEL | int32_t | 2       |
| NWNX_PROFILER_PATHING_LOG_INTERVAL           | uint32_t | 60      |
| NWNX_PROFILER_ENABLE_SCRIPTS                 | bool     | true    |
| NWNX_PROFILER_SCRIPTS_AREA_TIMINGS           | bool     | true    |
| NWNX_PROFILER_SCRIPTS_TYPE_TIMINGS           | bool     | true    |
| NWNX_PROFILER_ENABLE_TICKRATE                | bool     | true    |

## Pathing outcomes

`NWNX_PROFILER_ENABLE_PATHING_OUTCOMES` counts how each in-area path request (`CNWSModule::PlotPathInArea`) ends and pushes the counts every `NWNX_PROFILER_PATHING_OUTCOMES_INTERVAL` seconds as `PathingOutcome` with field `Count` and tags:

| Tag | Values |
| --- | ------ |
| Area | Area resref |
| AILevel | The creature's AI level, `-1` (default) to `4`. In an area without players creatures run at `0`, where a move whose straight line is blocked fails at once and is retried |
| Outcome | `Complete`, `Partial` (complete, but the last waypoint stops more than 1m short of the requested range: the engine's straight-line fallback), `Failed` |
| Controller | `PC` (player-controlled) or `NPC` |
| GridSearch | `true` if the short-range grid search was used |
| NoTileRoute | `true` if the tile-to-tile route search found no route during the request |

### Logging single requests

These settings need `NWNX_PROFILER_ENABLE_PATHING_OUTCOMES`. They write lines to the NWNX log, with the creature's tag, template, object id and AI level, and the start and goal:

- `NWNX_PROFILER_PATHING_LOG_SLOW_MS`: log each planner call (one `CNWSModule::PlotPath`, the same call the `PlotPath` timing measures) that takes at least this many milliseconds. Each call runs inside the main loop, so a slow call is a server stall. `0` turns this off. The line splits the time into the inter-area search (`ComputeInterAreaPath`), the in-area planner (`PlotPathInArea`) and everything else, so it shows where a stall goes. It is logged as the call ends, so requests that never finish are logged too.
- `NWNX_PROFILER_PATHING_LOG_FAILED`: log failed requests (counted when the in-area planner finishes) of creatures at `NWNX_PROFILER_PATHING_LOG_FAILED_MIN_AI_LEVEL` or higher. The default, `2` (normal), leaves out creatures in areas without players. Set it to `-1` to log every failed request. A request that fails in the inter-area search, before the in-area planner runs, is not counted.
- `NWNX_PROFILER_PATHING_LOG_INTERVAL`: log a creature at most once in this many seconds, separately for each kind of line.

```
Pathing slow call: NPC 'Commoner' (commoner001, 5a2f1), AI level 1, area cari_center, leg goal area cari_center, final goal area -, route areas 1 (resolved 1, in progress 0), from (42.10, 33.20) to (61.40, 70.85), range 1.00, call 2, complete, 31.40 ms: inter-area search 0.00, in-area planner 4.10, other 27.30
Pathing failed: area towerofnature001, NPC 'NW_DEER' (nw_deer, 2e0), AI level 2, from (46.83, 52.14) to (41.34, 45.45), range 1.00, 101 calls, 134.1 ms (slowest call 3.3 ms), grid search 0, no tile route 0
```
