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
| NWNX_PROFILER_ENABLE_SCRIPTS                 | bool     | true    |
| NWNX_PROFILER_SCRIPTS_AREA_TIMINGS           | bool     | true    |
| NWNX_PROFILER_SCRIPTS_TYPE_TIMINGS           | bool     | true    |
| NWNX_PROFILER_ENABLE_TICKRATE                | bool     | true    |

## Pathing outcomes

`NWNX_PROFILER_ENABLE_PATHING_OUTCOMES` counts how each in-area path request (`CNWSModule::PlotPathInArea`) ends and pushes the counts every `NWNX_PROFILER_PATHING_OUTCOMES_INTERVAL` seconds as `PathingOutcome` with field `Count` and tags:

| Tag | Values |
| --- | ------ |
| Area | Area resref |
| Outcome | `Complete`, `Partial` (complete, but the last waypoint stops more than 1m short of the requested range: the engine's straight-line fallback), `Failed` |
| Controller | `PC` (player-controlled) or `NPC` |
| GridSearch | `true` if the short-range grid search was used |
| NoTileRoute | `true` if the tile-to-tile route search found no route during the request |
