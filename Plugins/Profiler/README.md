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

These settings need `NWNX_PROFILER_ENABLE_PATHING_OUTCOMES`. They write one line for a finished request to the NWNX log, with the creature's tag, template, object id and AI level, the start and goal, the number of planner calls, the total time and the slowest call:

- `NWNX_PROFILER_PATHING_LOG_SLOW_MS`: log a request with a planner call at least this many milliseconds long. Each call runs inside the main loop, so a slow call is a server stall. `0` turns this off.
- `NWNX_PROFILER_PATHING_LOG_FAILED`: log failed requests of creatures at `NWNX_PROFILER_PATHING_LOG_FAILED_MIN_AI_LEVEL` or higher. The default, `2` (normal), leaves out creatures in areas without players. Set it to `-1` to log every failed request.
- `NWNX_PROFILER_PATHING_LOG_INTERVAL`: log a creature at most once in this many seconds.

```
Pathing failed (slow): area towerofnature001, NPC 'NW_DEER' (nw_deer, 2e0), AI level 2, from (46.83, 52.14) to (41.34, 45.45), range 1.00, 101 calls, 134.1 ms (slowest call 3.3 ms), grid search 0, no tile route 0
```
