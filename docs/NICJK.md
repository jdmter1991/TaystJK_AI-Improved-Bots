# What NicJK Unlimited changes

NicJK Unlimited is TaystJK with the work needed to run very large maps with a full server of bots. This page lists what is different and what was measured. Numbers come from one development PC (8 logical cores, 16 GB RAM) on the SWTOR Korriban map with 24 to 32 bots; treat them as an indication, not a guarantee.

## Engine limits

The map format limits are raised to four times the stock values (`codemp/qcommon/qfiles.h`), so maps with far more brushes, planes, nodes, leafs, shaders and models load. The renderer's per-frame entity limit in rd-vulkan is raised from 2047 to 4095, and the renderer warns once per map when it has to drop entities or surfaces instead of failing silently.

## Collision

World traces on huge maps were dominated by leafs holding tens of thousands of brushes. Collision now builds a brush hierarchy per large leaf when the map loads (`CM_BuildLeafAccel`), so a trace only tests the brushes near it. The results are the same as before; only the cost changes.

## Server

- `sv_profile <seconds>` prints where server time goes (bot AI, game, snapshots) and logs every frame that took too long
- `g_profile <seconds>` splits the game simulation into zones (Pmove, saber collision, Ghoul2 bolts, Force, missiles, ...) with average, 99th percentile and maximum per frame
- `com_jobs` worker threads for parallel server work (`-1` = one per hardware thread beyond the first, `0` = off, every loop serial); `sv_traceBench` times collision traces on the main thread and on the job threads (see [the multithreading report](reports/ENGINE_MULTITHREADING.md))
- `g_fastJPTrace 1` (default): the duel/race-mode trace trick only looks at entities that can have a client (same results, less work on every trace)
- `sv_botSnapshots -1` (default): no snapshots for bots until the game module reads one (the NicJK bots never do); `1` = always build them, with the number and cost of traces in it.
- `bot_profile <seconds>` prints the bot AI's cost per function and the `BOTEVAL` counters described in the bot document.
- A client that is still loading the map is no longer sent transient commands (chat, prints, kill messages). With a full server these filled its command queue and dropped it at "awaiting gamestate".
- Team spawning: when no spawn point was free, the game handed out the first one of that kind, enabled or not, so a whole wave of players spawned on one spot. It now picks a random enabled one.

Measured on the earlier Korriban map with 32 bots: about 8.6 ms of server time per 25 ms frame.

## Renderer (rd-vulkan)

- Pipeline lookups are cached and static model culling is cheaper.
- Static models no longer vanish because of a point test against the map's visibility data.
- MD3 frame bounds are grown to cover the loaded vertices, which fixes models being culled while still on screen.
- `cg_forceFxInterval` rate-limits force lightning and drain effects, which could stall the client when many players used them at once.

Measured uncapped on the earlier Korriban map: 94 to 159 frames per second at the test positions, the same with 31 bots as with 26.

## Per-model animations

`per-model-anims-config.cfg` lets the same move play a different animation per player model (for example a Sith model's stance, taunt or kata), off unless `g_permodelanims` / `cg_permodelanims` is set. Gameplay timing stays the same unless a line asks for it. See [NICJK_ANIMREMAP.md](NICJK_ANIMREMAP.md).

## Bots

The bot AI is the largest part of the fork. It has its own page: [NICJK_BOTS.md](NICJK_BOTS.md).

## What stays the same

Game rules, movement, saber and Force mechanics are TaystJK's. The bots play through the same input a player has; nothing gives them abilities, damage or information a player would not have. Network protocol and mod compatibility are unchanged: the fork still identifies its extra features to servers and clients the way TaystJK does. Only the names changed: programs, settings file and mod folder are `nicjk` instead of `taystjk`.
