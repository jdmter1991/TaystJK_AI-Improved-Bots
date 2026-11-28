# Engine multithreading and server frame optimisation

DRAFT - numbers to follow.

## 3. The job system (`com_jobs`)

`codemp/qcommon/q_jobs.cpp`: a fixed pool of worker threads, started when `com_jobs` changes
(between frames, never during one) and joined at shutdown.

- `Com_JobsParallelFor( count, fn, ctx )` runs `fn( ctx, index, thread )` for every index. The
  calling thread takes part; indices are claimed one at a time through an atomic counter, so items
  of uneven size balance themselves. The call returns when every index has finished.
- Determinism: a job writes only to memory that belongs to its index; the caller reads the results
  afterwards in index order. Which thread ran which index never shows in the result.
- Nothing is allocated while a loop runs (one descriptor: function pointer, context, atomics).
- `com_jobs N`: N worker threads; `-1` = one per hardware thread beyond the first (at most 15);
  `0` = none: every loop runs serially on the main thread, i.e. the original code.
- Workers spin for `com_jobsSpin` microseconds (100) after a loop, since a frame starts its loops
  close together, then sleep on a condition variable.
- Portable C++11 (`std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`);
  `thread_local` only in the executables (works on XP, Linux, macOS); a pause / yield hint in spin
  loops. No OS-specific thread API.

The game module reaches it through three functions appended to `gameImport_t` (`JobsThreads`,
`JobsParallelFor`, `WorldGeneration`). An older engine passes a shorter table, so the engine
announces them with the read-only cvar `sv_gameJobsApi 1` and the game checks it before use.

## 4. Thread-safe collision

A parallel job may call `trap->Trace` (without Ghoul2 flags), `trap->PointContents`, `trap->InPVS`
and read game memory. The following was changed so that these are safe on several threads at
once while nothing links entities. The main thread's code path is unchanged.

| Shared state | Before | Now |
| --- | --- | --- |
| `checkcount` in every brush and patch ("already tested in this trace") | one counter per clip map, written into the map data during every trace | the main thread keeps using it; each job thread has its own stamp arrays per clip map (`cmStamp_t`, allocated by `CM_ThreadScratchEnsure` before a loop when the map or the thread count changed: 4 bytes per brush and patch per thread) |
| trace accelerator switch `cm_accelActive`, swept volume `cm_sweep` | globals set per trace | fields of `traceWork_t` |
| temporary box model (`CM_TempBoxModel`: every trace against a non-brush entity) | one global box | one per thread (the hunk holds 16 box brushes, planes and sides instead of 1) |
| collision work counters | global | per thread, merged after the loop |
| `c_traces` and other statistics, debug-surface pointers in the patch code | global | main thread only |
| entity list of `SV_ClipMoveToEntities` | `static int touchlist[]` | one per thread |
| `SV_Trace` / `SV_PointContents` profile counters | global | per thread, merged after the loop |
| world sectors (entity lists of the world) | read by traces, written by `SV_LinkEntity` | only read during a loop; linking during a loop is a fatal error |
| Ghoul2 hit tests inside traces | global `G2VertSpace`, transform caches, statics | not done from a job (never asked for; counted and reported if it happens) |
