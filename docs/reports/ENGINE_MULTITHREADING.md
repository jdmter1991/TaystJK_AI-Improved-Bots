# Engine multithreading and server frame optimisation

Where a NicJK Unlimited dedicated server spends its frame, what now runs on several cores, and what
it gained. Every number was measured on the test machine (Intel i7-7700, 4 cores / 8 threads,
Windows 11, RelWithDebInfo x64, dedicated server, bots only, FFA with sabers, the user's bot roster)
with the tools in section 10. "31 bots" is the full server (32 slots, the 32nd `addbot` is refused).

## 1. Summary

- **Profile (section 2).** At 31 bots the frame is split roughly half bot AI, half game simulation,
  plus snapshots. In the game simulation, saber collision (with its Ghoul2 hit tests), `Pmove` and
  the saber position update (Ghoul2 bolt matrices) are the big three; bot snapshots cost
  0.5-0.8 ms per frame although nothing reads them.
- **Job system (`com_jobs`, section 3)**: a fixed worker pool in qcommon, sized from the CPU
  (`-1` = hardware threads - 1 = 7 here), deterministic, no allocation during play, `0` = the
  original serial code.
- **Thread-safe collision (section 4)**: traces can run on all threads at once with bit-identical
  results; `sv_traceBench` measures x3.1-4.2 on 4 threads and x5.1-6.9 on 8.
- **Parallel bot sensing (`bot_mt`, section 5)**: the bots' expensive traces (enemy scan,
  visibility, wall/duck probes, cliff checks) are made in parallel before the serial decisions
  use them; 56-76% of the bot AI traces on naboo_theed and 25-62% on mp/ffa3 are served this way. Exact by construction: a stored
  result is only used for a bit-identical trace in an unchanged world (verified, section 9).
- **Serial fixes found on the way (section 6)**: `g_fastJPTrace` (the duel trick no longer walks all
  entities on every trace, including every `Pmove` trace) and `sv_botSnapshots` (no snapshots for
  bots nobody reads).
- **Result (section 8)**, same server, settings switched in turn: 31 bots, `sv_fps 40`: frame
  **-13% (naboo_theed) / -31% (mp/ffa3)**, bot AI -21% / -35%; `sv_fps 125`: **-29% / -28%**, bot AI
  -32% / -23%. 8-24 bots: -5% to -34%.
- **Not done**: parallel Ghoul2 posing (section 7). It is where the next gain is (Ghoul2 is about a
  third of the game simulation on ffa3), but the Ghoul2 code is not thread-safe and the poses are
  computed in the middle of each client's serial turn, so a precomputed pose could not be proven
  identical.


## 2. The whole server frame, by category

Instrumentation (commit `56a91e683`), all off unless asked for:

- `g_profile <s>`: `GAMEPROF` / `GAMEZONE` lines. `G_RunFrame` split into zones (client think around
  `Pmove`, `Pmove`, client events, touch triggers, saber position / Ghoul2 bolts, saber collision
  `CheckSaberDamage`, missile block check, Force powers, missiles, items, movers, entity thinks,
  client end frame, rules and queues, ICARUS/ROFF). Zones nest and report self time; per zone the
  average, the 99th percentile and the maximum of its per-frame time.
- `sv_profile <s>`: besides the phases (bot AI / game / snapshots) now the engine-side parts: Ghoul2
  bolt matrices, Ghoul2 collision (direct and inside traces), Ghoul2 animation calls, snapshot
  building, snapshot sending, parallel loops, each with avg / p99 / max per frame.
- `bot_profile <s>`: the bot AI zones of the previous round (`BOTZONE`), plus the parallel phase.

Original code paths (`com_jobs 0`, `bot_mt 0`, `g_fastJPTrace 0`, `sv_botSnapshots 1`), the "base"
setting of the section 8 runs: 5-second windows, nine per point; avg = mean per frame, p99 = mean
of the windows' 99th percentiles, max = worst frame. The Ghoul2 rows are inside the zones above
them (bolts inside "saber position", collision inside "saber collision"); there were no human
clients, so nothing was sent.

naboo_theed, sv_fps 40: ms per frame, avg / p99 / max (base)

| Category | 8 bots | 16 bots | 24 bots | 31 bots |
| --- | ---: | ---: | ---: | ---: |
| **whole server frame** | 2.916 / 5.06 / 6.60 | 4.948 / 10.60 / 12.57 | 7.614 / 17.34 / 20.37 | 8.361 / 22.91 / 26.27 |
| bot AI (all of it) | 1.416 / 2.72 / - | 2.496 / 5.64 / - | 3.843 / 9.12 / - | 4.258 / 12.03 / - |
| game simulation (G_RunFrame) | 1.411 / 2.76 / 11.05 | 2.181 / 4.86 / 15.06 | 3.218 / 8.23 / 14.81 | 3.415 / 9.92 / 16.36 |
| - Pmove (movement physics) | 0.420 / 0.88 / 1.32 | 0.675 / 1.50 / 1.97 | 1.025 / 2.44 / 3.06 | 1.100 / 3.21 / 3.99 |
| - saber position (blades, ghoul2 bolts) | 0.348 / 0.64 / 1.04 | 0.547 / 1.16 / 2.22 | 0.719 / 1.73 / 3.07 | 0.708 / 1.98 / 2.95 |
| - saber collision (CheckSaberDamage) | 0.253 / 0.67 / 2.61 | 0.477 / 1.41 / 3.50 | 0.824 / 2.55 / 5.67 | 0.900 / 3.02 / 5.50 |
| - client think (around Pmove) | 0.098 / 0.18 / 0.40 | 0.160 / 0.35 / 0.43 | 0.239 / 0.57 / 0.82 | 0.255 / 0.73 / 1.16 |
| - force powers update | 0.009 / 0.02 / 0.08 | 0.015 / 0.04 / 0.15 | 0.025 / 0.10 / 1.03 | 0.025 / 0.09 / 1.17 |
| - client events | 0.001 / 0.00 / 0.01 | 0.001 / 0.00 / 0.01 | 0.002 / 0.00 / 0.02 | 0.002 / 0.00 / 0.01 |
| - touch triggers | 0.006 / 0.01 / 0.02 | 0.011 / 0.02 / 0.09 | 0.020 / 0.04 / 0.11 | 0.024 / 0.06 / 0.21 |
| - saber missile block check | 0.012 / 0.03 / 0.11 | 0.022 / 0.06 / 0.13 | 0.043 / 0.13 / 0.27 | 0.051 / 0.18 / 0.29 |
| - missiles | - | - | 0.001 / 0.01 / 0.12 | 0.004 / 0.02 / 0.08 |
| - items and physics objects | 0.005 / 0.02 / 0.07 | 0.016 / 0.04 / 0.10 | 0.024 / 0.07 / 0.13 | 0.041 / 0.13 / 0.27 |
| - movers | 0.001 / 0.00 / 0.00 | 0.000 / 0.00 / 0.01 | 0.000 / 0.00 / 0.02 | 0.001 / 0.00 / 0.02 |
| - entity think functions | 0.037 / 0.07 / 0.20 | 0.045 / 0.10 / 0.19 | 0.078 / 0.18 / 0.42 | 0.092 / 0.30 / 0.54 |
| - client end frame | 0.005 / 0.00 / 0.05 | 0.007 / 0.01 / 0.03 | 0.010 / 0.02 / 0.26 | 0.009 / 0.02 / 0.10 |
| - rules, votes, queues | 0.119 / 0.20 / 0.38 | 0.109 / 0.20 / 4.86 | 0.102 / 0.19 / 0.26 | 0.093 / 0.20 / 1.75 |
| - icarus, roff | 0.004 / 0.00 / 0.02 | 0.005 / 0.00 / 0.02 | 0.006 / 0.01 / 0.09 | 0.006 / 0.01 / 0.04 |
| - G_RunFrame upkeep (not below) | 0.061 / 0.21 / 0.34 | 0.067 / 0.23 / 0.36 | 0.070 / 0.24 / 0.44 | 0.073 / 0.26 / 0.36 |
| Ghoul2 bolt matrices (inside saber position etc.) | 0.173 / 0.33 / 0.51 | 0.294 / 0.64 / 0.87 | 0.376 / 0.93 / 1.17 | 0.374 / 1.04 / 1.52 |
| Ghoul2 collision (inside saber collision) | 0.013 / 0.28 / 2.28 | 0.037 / 0.56 / 2.60 | 0.100 / 1.42 / 3.95 | 0.145 / 1.41 / 3.26 |
| Ghoul2 animation state calls | 0.087 / 0.17 / 0.30 | 0.129 / 0.28 / 0.41 | 0.167 / 0.43 / 0.59 | 0.163 / 0.47 / 0.69 |
| snapshot building | 0.111 / 0.19 / 0.39 | 0.286 / 0.62 / 0.88 | 0.571 / 1.39 / 1.95 | 0.702 / 2.15 / 2.70 |
| snapshot sending (networking) | - | - | - | - |
| game traces per frame / ms | 90.533 / 0.41 / - | 181.672 / 0.66 / - | 271.456 / 1.02 / - | 346.767 / 1.09 / - |
| bot AI traces per frame / ms | 86.410 / 0.86 / - | 187.932 / 1.56 / - | 305.001 / 2.49 / - | 409.295 / 2.68 / - |

mp/ffa3, sv_fps 40: ms per frame, avg / p99 / max (base)

| Category | 8 bots | 16 bots | 24 bots | 31 bots |
| --- | ---: | ---: | ---: | ---: |
| **whole server frame** | 2.636 / 5.01 / 6.97 | 4.427 / 9.72 / 12.80 | 7.054 / 15.84 / 21.02 | 8.910 / 22.16 / 38.85 |
| bot AI (all of it) | 0.872 / 1.46 / - | 1.450 / 3.08 / - | 2.522 / 5.54 / - | 3.434 / 8.67 / - |
| game simulation (G_RunFrame) | 1.641 / 4.03 / 11.17 | 2.658 / 6.73 / 12.23 | 3.985 / 10.12 / 13.73 | 4.725 / 13.40 / 30.73 |
| - Pmove (movement physics) | 0.336 / 0.68 / 0.82 | 0.494 / 1.06 / 1.44 | 0.772 / 1.74 / 2.42 | 0.949 / 2.44 / 3.04 |
| - saber position (blades, ghoul2 bolts) | 0.393 / 0.80 / 1.50 | 0.594 / 1.25 / 1.98 | 0.858 / 2.14 / 4.34 | 0.966 / 2.56 / 3.75 |
| - saber collision (CheckSaberDamage) | 0.429 / 2.00 / 3.49 | 0.914 / 3.35 / 6.20 | 1.508 / 5.27 / 9.07 | 1.772 / 6.55 / 11.66 |
| - client think (around Pmove) | 0.099 / 0.21 / 0.34 | 0.158 / 0.33 / 1.59 | 0.248 / 0.56 / 1.76 | 0.304 / 0.80 / 1.26 |
| - force powers update | 0.012 / 0.05 / 0.12 | 0.024 / 0.09 / 0.95 | 0.038 / 0.18 / 1.28 | 0.046 / 0.21 / 0.96 |
| - client events | 0.001 / 0.00 / 0.01 | 0.001 / 0.00 / 0.01 | 0.002 / 0.00 / 0.05 | 0.002 / 0.00 / 0.02 |
| - touch triggers | 0.007 / 0.01 / 0.76 | 0.011 / 0.02 / 0.07 | 0.018 / 0.04 / 0.06 | 0.021 / 0.05 / 0.13 |
| - saber missile block check | 0.018 / 0.07 / 0.16 | 0.037 / 0.10 / 0.17 | 0.058 / 0.19 / 0.35 | 0.088 / 0.27 / 0.44 |
| - missiles | 0.001 / 0.01 / 0.15 | 0.003 / 0.01 / 0.05 | 0.006 / 0.02 / 0.32 | 0.010 / 0.04 / 0.08 |
| - items and physics objects | 0.004 / 0.00 / 0.04 | 0.003 / 0.00 / 0.02 | 0.003 / 0.00 / 0.02 | 0.003 / 0.00 / 0.24 |
| - movers | 0.003 / 0.02 / 0.13 | 0.004 / 0.02 / 0.51 | 0.002 / 0.01 / 0.03 | 0.001 / 0.00 / 0.04 |
| - entity think functions | 0.023 / 0.05 / 0.10 | 0.027 / 0.06 / 0.12 | 0.036 / 0.09 / 0.25 | 0.043 / 0.13 / 0.30 |
| - client end frame | 0.005 / 0.01 / 0.03 | 0.008 / 0.01 / 0.03 | 0.011 / 0.02 / 0.05 | 0.013 / 0.03 / 0.05 |
| - rules, votes, queues | 0.117 / 0.21 / 0.34 | 0.106 / 0.18 / 0.28 | 0.110 / 0.25 / 1.46 | 0.125 / 0.29 / 21.26 |
| - icarus, roff | 0.004 / 0.00 / 0.04 | 0.005 / 0.00 / 0.02 | 0.007 / 0.01 / 0.03 | 0.008 / 0.01 / 0.06 |
| - G_RunFrame upkeep (not below) | 0.159 / 0.40 / 0.66 | 0.246 / 0.61 / 0.93 | 0.284 / 0.67 / 2.81 | 0.332 / 0.88 / 1.56 |
| Ghoul2 bolt matrices (inside saber position etc.) | 0.204 / 0.40 / 0.59 | 0.321 / 0.66 / 1.06 | 0.465 / 1.00 / 1.40 | 0.515 / 1.29 / 1.68 |
| Ghoul2 collision (inside saber collision) | 0.166 / 1.57 / 2.95 | 0.452 / 2.59 / 5.42 | 0.707 / 3.84 / 7.42 | 0.820 / 4.41 / 8.89 |
| Ghoul2 animation state calls | 0.087 / 0.18 / 0.39 | 0.127 / 0.28 / 0.46 | 0.179 / 0.40 / 0.70 | 0.204 / 0.54 / 0.83 |
| snapshot building | 0.142 / 0.25 / 0.37 | 0.342 / 0.74 / 1.06 | 0.573 / 1.29 / 1.82 | 0.777 / 2.03 / 2.67 |
| snapshot sending (networking) | - | - | - | - |
| game traces per frame / ms | 84.402 / 0.36 / - | 162.910 / 0.53 / - | 240.137 / 0.73 / - | 311.957 / 0.91 / - |
| bot AI traces per frame / ms | 90.875 / 0.46 / - | 197.836 / 0.70 / - | 321.509 / 1.11 / - | 442.154 / 1.52 / - |

naboo_theed, sv_fps 125: ms per frame, avg / p99 / max (base)

| Category | 8 bots | 16 bots | 24 bots | 31 bots |
| --- | ---: | ---: | ---: | ---: |
| **whole server frame** | 2.872 / 5.90 / 23.61 | 2.226 / 4.70 / 9.44 | 4.990 / 14.46 / 189.22 | 6.655 / 13.84 / 206.97 |
| bot AI (all of it) | 1.396 / 3.02 / - | 1.155 / 2.37 / - | 2.505 / 4.80 / - | 3.445 / 6.15 / - |
| game simulation (G_RunFrame) | 1.383 / 3.18 / 20.45 | 0.946 / 2.22 / 6.09 | 2.198 / 9.16 / 205.19 | 2.650 / 8.04 / 199.91 |
| - Pmove (movement physics) | 0.401 / 0.95 / 1.72 | 0.284 / 0.59 / 1.20 | 0.545 / 1.06 / 2.00 | 0.812 / 1.49 / 2.38 |
| - saber position (blades, ghoul2 bolts) | 0.377 / 0.94 / 1.40 | 0.235 / 0.54 / 1.17 | 0.404 / 0.83 / 1.39 | 0.589 / 1.11 / 1.59 |
| - saber collision (CheckSaberDamage) | 0.239 / 0.74 / 2.22 | 0.197 / 0.47 / 1.08 | 0.376 / 1.06 / 3.69 | 0.571 / 1.27 / 2.35 |
| - client think (around Pmove) | 0.096 / 0.23 / 0.76 | 0.064 / 0.13 / 0.26 | 0.120 / 0.24 / 0.56 | 0.177 / 0.33 / 0.61 |
| - force powers update | 0.009 / 0.02 / 1.34 | 0.006 / 0.01 / 0.04 | 0.012 / 0.03 / 0.30 | 0.018 / 0.05 / 0.19 |
| - client events | 0.001 / 0.00 / 0.05 | 0.001 / 0.00 / 0.01 | 0.001 / 0.00 / 0.17 | 0.001 / 0.00 / 0.04 |
| - touch triggers | 0.006 / 0.01 / 0.30 | 0.005 / 0.01 / 0.03 | 0.011 / 0.02 / 0.10 | 0.017 / 0.03 / 0.29 |
| - saber missile block check | 0.011 / 0.03 / 0.31 | 0.010 / 0.02 / 0.06 | 0.026 / 0.06 / 0.24 | 0.029 / 0.06 / 0.18 |
| - missiles | - | 0.001 / 0.00 / 0.08 | 0.000 / 0.00 / 0.07 | - |
| - items and physics objects | 0.002 / 0.01 / 0.08 | 0.006 / 0.01 / 0.11 | 0.017 / 0.04 / 0.16 | 0.026 / 0.07 / 0.19 |
| - movers | 0.001 / 0.00 / 0.02 | 0.000 / 0.00 / 0.01 | 0.000 / 0.00 / 0.02 | 0.000 / 0.00 / 0.01 |
| - entity think functions | 0.032 / 0.07 / 0.50 | 0.020 / 0.05 / 0.15 | 0.043 / 0.12 / 0.28 | 0.066 / 0.19 / 0.61 |
| - client end frame | 0.005 / 0.01 / 0.32 | 0.004 / 0.01 / 0.06 | 0.009 / 0.03 / 0.11 | 0.014 / 0.05 / 0.29 |
| - rules, votes, queues | 0.112 / 0.26 / 17.20 | 0.071 / 0.25 / 2.87 | 0.211 / 3.14 / 59.82 | 0.140 / 1.14 / 33.67 |
| - icarus, roff | 0.004 / 0.00 / 0.08 | 0.002 / 0.00 / 0.03 | 0.005 / 0.01 / 0.06 | 0.007 / 0.01 / 0.21 |
| - G_RunFrame upkeep (not below) | 0.061 / 0.27 / 0.88 | 0.036 / 0.15 / 2.07 | 0.086 / 0.47 / 32.47 | 0.084 / 0.22 / 63.71 |
| Ghoul2 bolt matrices (inside saber position etc.) | 0.194 / 0.51 / 1.12 | 0.129 / 0.30 / 0.60 | 0.262 / 0.72 / 4.57 | 0.349 / 0.90 / 5.83 |
| Ghoul2 collision (inside saber collision) | 0.014 / 0.22 / 1.86 | 0.009 / 0.13 / 0.53 | 0.045 / 0.82 / 6.67 | 0.049 / 0.54 / 2.79 |
| Ghoul2 animation state calls | 0.090 / 0.26 / 0.76 | 0.052 / 0.12 / 0.28 | 0.107 / 0.35 / 2.91 | 0.131 / 0.40 / 3.36 |
| snapshot building | 0.098 / 0.23 / 0.99 | 0.123 / 0.26 / 0.48 | 0.310 / 0.67 / 1.15 | 0.552 / 1.12 / 1.68 |
| snapshot sending (networking) | - | - | - | - |
| game traces per frame / ms | 95.748 / 0.38 / - | 187.159 / 0.30 / - | 306.000 / 0.62 / - | 351.609 / 0.80 / - |
| bot AI traces per frame / ms | 86.117 / 0.87 / - | 179.080 / 0.77 / - | 311.403 / 1.66 / - | 391.290 / 2.28 / - |

mp/ffa3, sv_fps 125: ms per frame, avg / p99 / max (base)

| Category | 8 bots | 16 bots | 24 bots | 31 bots |
| --- | ---: | ---: | ---: | ---: |
| **whole server frame** | 1.327 / 3.09 / 10.35 | 2.269 / 4.58 / 9.15 | 3.598 / 6.69 / 94.51 | 4.914 / 13.13 / 131.54 |
| bot AI (all of it) | 0.455 / 0.91 / - | 0.726 / 1.42 / - | 1.246 / 2.27 / - | 1.733 / 3.09 / - |
| game simulation (G_RunFrame) | 0.813 / 2.45 / 9.21 | 1.381 / 3.41 / 7.67 | 2.030 / 4.44 / 92.33 | 2.717 / 8.26 / 129.02 |
| - Pmove (movement physics) | 0.100 / 0.22 / 0.47 | 0.198 / 0.41 / 0.87 | 0.383 / 0.71 / 1.27 | 0.496 / 0.89 / 1.76 |
| - saber position (blades, ghoul2 bolts) | 0.136 / 0.34 / 1.54 | 0.264 / 0.59 / 1.13 | 0.476 / 0.94 / 1.38 | 0.608 / 1.15 / 15.02 |
| - saber collision (CheckSaberDamage) | 0.102 / 0.43 / 1.04 | 0.310 / 1.07 / 2.57 | 0.594 / 1.61 / 2.88 | 0.892 / 2.26 / 8.34 |
| - client think (around Pmove) | 0.032 / 0.07 / 0.26 | 0.064 / 0.13 / 0.48 | 0.122 / 0.23 / 0.83 | 0.165 / 0.32 / 0.80 |
| - force powers update | 0.005 / 0.02 / 0.39 | 0.010 / 0.04 / 0.35 | 0.019 / 0.07 / 0.71 | 0.022 / 0.07 / 0.81 |
| - client events | 0.000 / 0.00 / 0.02 | 0.001 / 0.00 / 0.03 | 0.001 / 0.00 / 0.07 | 0.001 / 0.00 / 0.06 |
| - touch triggers | 0.002 / 0.00 / 0.09 | 0.005 / 0.01 / 0.11 | 0.009 / 0.02 / 0.09 | 0.013 / 0.03 / 0.19 |
| - saber missile block check | 0.008 / 0.02 / 0.09 | 0.018 / 0.04 / 0.24 | 0.032 / 0.08 / 0.35 | 0.044 / 0.11 / 0.63 |
| - missiles | 0.000 / 0.00 / 0.03 | 0.001 / 0.00 / 0.04 | 0.005 / 0.03 / 0.38 | 0.002 / 0.01 / 0.09 |
| - items and physics objects | 0.002 / 0.00 / 0.05 | 0.002 / 0.00 / 0.02 | 0.002 / 0.00 / 0.09 | 0.002 / 0.00 / 0.05 |
| - movers | 0.001 / 0.00 / 0.05 | 0.001 / 0.00 / 0.25 | 0.001 / 0.00 / 0.07 | 0.001 / 0.00 / 0.05 |
| - entity think functions | 0.010 / 0.03 / 0.12 | 0.014 / 0.04 / 0.10 | 0.023 / 0.06 / 0.38 | 0.027 / 0.07 / 0.24 |
| - client end frame | 0.002 / 0.00 / 0.06 | 0.004 / 0.01 / 0.08 | 0.010 / 0.03 / 0.17 | 0.013 / 0.04 / 0.21 |
| - rules, votes, queues | 0.038 / 0.45 / 5.01 | 0.063 / 0.34 / 2.33 | 0.134 / 1.54 / 63.59 | 0.140 / 1.54 / 17.86 |
| - icarus, roff | 0.002 / 0.00 / 0.06 | 0.003 / 0.00 / 0.07 | 0.005 / 0.01 / 0.19 | 0.007 / 0.02 / 0.24 |
| - G_RunFrame upkeep (not below) | 0.087 / 0.33 / 4.78 | 0.112 / 0.24 / 0.87 | 0.188 / 0.39 / 43.32 | 0.222 / 0.86 / 10.52 |
| Ghoul2 bolt matrices (inside saber position etc.) | 0.109 / 0.27 / 0.41 | 0.193 / 0.45 / 1.07 | 0.283 / 0.57 / 4.42 | 0.363 / 0.99 / 6.58 |
| Ghoul2 collision (inside saber collision) | 0.030 / 0.46 / 1.49 | 0.177 / 1.17 / 2.61 | 0.247 / 1.15 / 2.37 | 0.382 / 1.75 / 11.69 |
| Ghoul2 animation state calls | 0.047 / 0.13 / 0.32 | 0.068 / 0.17 / 0.49 | 0.092 / 0.21 / 1.62 | 0.115 / 0.37 / 3.47 |
| snapshot building | 0.059 / 0.13 / 0.43 | 0.161 / 0.34 / 0.63 | 0.318 / 0.69 / 1.77 | 0.460 / 0.89 / 1.43 |
| snapshot sending (networking) | - | - | - | - |
| game traces per frame / ms | 130.218 / 0.19 / - | 205.887 / 0.27 / - | 226.442 / 0.36 / - | 317.467 / 0.51 / - |
| bot AI traces per frame / ms | 94.336 / 0.21 / - | 198.329 / 0.33 / - | 304.625 / 0.53 / - | 440.718 / 0.76 / - |

What this says:

- **Bot AI vs game simulation.** At 31 bots and 40 Hz the bot AI is 4.3 ms (naboo) / 3.4 ms (ffa3)
  and the game simulation 3.4 / 4.7 ms; at 125 Hz 3.4 / 1.7 and 2.7 / 2.7 ms. So, as suspected, the
  simulation costs as much as the AI.
- **Inside the simulation** (31 bots, 40 Hz): saber collision 0.9 (naboo) / 1.8 ms (ffa3, more
  close combat) with the worst p99 (3.0 / 6.6 ms); `Pmove` 1.1 / 0.95 ms; saber position 0.7 /
  1.0 ms. Ghoul2 bolts + collision + animation calls together: 0.7 ms on naboo, 1.5 ms on ffa3.
  Everything else (Force, missiles, items, movers, triggers, entity thinks, end frame) is below
  0.1 ms each. "Rules, votes, queues" is 0.1 ms on average with rare spikes (scoreboard messages).
- **Snapshots**: 0.47-0.79 ms per frame at 31 bots for bots alone, i.e. 5-10% of the frame.
- **Traces**: 350 game traces and 400-440 bot traces per frame at 31 bots; the bot traces cost
  2.3-2.7 ms per frame on naboo (long sight lines through a large map).
- **The maxima** at 125 Hz (90-200 ms) are rare single frames spread over the zones and the
  settings alike (scheduling on this busy machine); the p99 shows the steady cost.


## 3. The job system (`com_jobs`)

`codemp/qcommon/q_jobs.cpp`: a fixed pool of worker threads, started when `com_jobs` changes
(between frames, never during one) and joined at shutdown.

- `Com_JobsParallelFor( count, fn, ctx )` runs `fn( ctx, index, thread )` for every index. The
  calling thread takes part; indices are claimed one at a time through an atomic counter, so items
  of uneven size balance themselves. The call returns when every index has finished.
- Determinism: a job writes only to memory that belongs to its index; the caller reads the results
  afterwards in index order. Which thread ran which index never shows in the result.
- Nothing is allocated while a loop runs (one descriptor: function pointer, context, atomics).
- `com_jobs N`: N worker threads; `-1` = one per hardware thread beyond the first (at most 15; 7 on
  the test PC); `0` = none: every loop runs serially on the main thread.
- Workers spin for `com_jobsSpin` microseconds (100) after a loop, since a frame starts its loops
  close together, then sleep on a condition variable. (3000 µs was measured and is slower: the
  spinning threads take the CPU from the main thread's serial work.)
- Portable C++11 (`std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`);
  `thread_local` only in the executables (works on XP, Linux, macOS); `_mm_pause` / `pause` /
  ARM `yield` in spin loops. No OS-specific thread API.

The game module reaches it through three functions appended to `gameImport_t` (`JobsThreads`,
`JobsParallelFor`, `WorldGeneration`). An older engine passes a shorter table, so the engine
announces them with the read-only cvar `sv_gameJobsApi 1` and the game checks it before use; on
an older engine the bots simply stay serial.

## 4. Thread-safe collision

A parallel job may call `trap->Trace` (without Ghoul2 flags), `trap->PointContents`,
`trap->InPVS` and read game memory. The following was changed so that these are safe on several
threads at once while nothing links entities. The main thread's code path is unchanged.

| Shared state | Before | Now |
| --- | --- | --- |
| `checkcount` in every brush and patch ("already tested in this trace") | one counter per clip map, written into the map data during every trace | the main thread keeps using it; each job thread has its own stamp arrays per clip map (`cmStamp_t`, allocated by `CM_ThreadScratchEnsure` before a loop when the map or the thread count changed: 4 bytes per brush and patch per thread) |
| trace accelerator switch `cm_accelActive`, swept volume `cm_sweep` | globals set per trace | fields of `traceWork_t` |
| temporary box model (`CM_TempBoxModel`: every trace against a non-brush entity) | one global box | one per thread, each on its own cache lines |
| collision work counters | global | per thread, cache-line aligned, merged after the loop |
| `c_traces` and other statistics, debug-surface pointers in the patch code | global | main thread only |
| entity list of `SV_ClipMoveToEntities` | `static int touchlist[]` | one per thread |
| `SV_Trace` / `SV_PointContents` profile counters | global | per thread, merged after the loop |
| world sectors (the entity lists traces walk) | read by traces, written by `SV_LinkEntity` | only read during a loop; linking or unlinking during a loop is a fatal error |
| shared collision of the big maps (cm_shared.cpp, merged from NicJK_Unlimited at the end: .theight / .tcol) | one "visited by this query" stamp array and counter | per thread for the job threads (allocated with the other stamps), main thread unchanged. Only maps with these files use it; the measurements above (mp/ffa3, naboo_theed) do not |
| Ghoul2 hit tests inside traces | global `G2VertSpace`, transform caches, statics | not done from a job (never asked for; counted and reported if it happens) |

**False sharing.** The first version kept the per-thread counters and boxes in plain arrays, so
neighbouring threads wrote the same cache lines on every leaf and accelerator node; parallel bot
traces then cost two to three times as much as on the main thread. With the counters and boxes
cache-line aligned, `sv_traceBench` (the same random line-of-sight traces on the main thread and
spread over the job threads, results compared) gives on naboo_theed with 31 bots in the game:

| Trace length | Main thread µs/trace | 2 threads | 4 threads | 8 threads | Different results |
| --- | ---: | ---: | ---: | ---: | ---: |
| up to 300 units | 2.6-2.7 | | 0.62 (x4.2) | 0.43 (x6.3) | 0 |
| up to 1000 units | 5.3-7.4 | 2.86 (x2.2) | 1.53-1.73 (x3.3-4.2) | 0.97-1.07 (x5.5-6.9) | 0 |
| up to 4000 units | 9.1-11.0 | 5.06 (x1.8) | 2.61-2.96 (x3.1-4.0) | 1.80-1.92 (x5.1-5.7) | 0 |

So collision traces scale with the cores (and partly with the hyper-threads) on this 4-core /
8-thread i7-7700, and give bit-identical results on any thread.

## 5. Parallel bot sensing (`bot_mt`)

The bot AI reads and writes shared game state everywhere (other bots, `level`, entities, chat,
learning tables), so the decisions stay serial and in client order. What is parallel is the
expensive *reading of the world*: the traces.

**Before the thinks** (`BotMt_SensePhase`), one job per bot computes, from the state the think
will start from, the traces that bot's think is about to make, and stores them in that bot's slot:

- the enemy scan (`ScanForEnemies`): the same candidate choice (in view or in hearing, mind trick
  and stickiness rules), nearest first, PVS test and line of sight until one is visible;
- the waypoint visibility trace, the line of sight to the enemy the think will have (the kept one
  or the one the scan finds), and if that one is hidden the look at where it was last seen;
- the wall check along the view and the duck probe toward the last goal.

**Between the two passes of the movement commands** (`bot_mt 8`): the command building was split
in two (`BotUpdateInput_Head` / `BotUpdateInput_Tail`). All bots run the first part, then the cliff
check traces of all bots are made in parallel, then all bots run the cliff check and hand their
command to the engine. Nothing in a bot's command building reads another bot's command or cliff
result (checked in `ai_main.c`: no other bot's `lastucmd` / `pers.cmd` is read there), so the
order does not matter.

**The serial code is unchanged.** The trace wrappers of `ai_main.c` (`BotProf_JPTrace`,
`BotProf_RawTrace`) take a stored result only when

- start, end, mins, maxs, pass entity, mask, capsule, Ghoul2 flags, LOD and the trace kind
  (JP_Trace or plain) are bit-identical (`memcmp`),
- no entity was linked or unlinked since the parallel phase (the engine's world generation),
- and the duel/race-mode trick of `JP_Trace` would change nothing for this trace
  (`JP_TraceHackIdle`; while a duel is on, those traces go through `JP_Trace` as before).

Each stored result is used once. Anything else is traced as before. So the parallel phase only
decides *which traces are done early*, never what a trace returns: **the bots decide exactly as
without `bot_mt`**, whatever the prediction got wrong (a wrong prediction only costs a wasted
early trace). `bot_mtVerify 1` traces every used result again and compares (section 9).

The parallel phase only reads game state, writes nothing but its own bot's slot, prints nothing
and draws no random numbers (so no per-bot RNG was needed); the slots are used in client order.

## 6. Serial optimisations found on the way

- **`g_fastJPTrace`.** Every `JP_Trace` (all bot traces and every `Pmove` trace go through it)
  walked all entities twice to find duelling or racing clients (`BeginHack` / `EndHack`), touching
  one cache line per entity on every trace. Only entities with a client can match, and beyond the
  player slots only NPCs and weapon shooters get one; while none exists the scans stop at the
  player slots. Same entities found, same traces. A per-frame check catches an entity with a
  client that was not counted and falls back to the full scan for the rest of the map.
- **`sv_botSnapshots`.** Every server frame built a full snapshot (entity culling, PVS tests) for
  every bot. A bot's snapshot is only read through `BotGetSnapshotEntity` and by a demo of that
  bot; the NicJK bots never call it. Default `-1`: bots get no snapshot until the game module reads
  one (then always, so another game module's bots keep theirs); bots that record a demo and
  servers with `sv_autoDemo` still get theirs; `1` = always, `0` = never.

## 7. Parallel Ghoul2 posing: measured, not done

Section 2 shows the cost: Ghoul2 bolt matrices 0.37-0.52 ms, Ghoul2 collision 0.15-0.82 ms and
animation calls 0.16-0.20 ms per frame at 31 bots, 40 Hz (up to 1.5 ms on ffa3, about a third of
the game simulation). It was not parallelised because the result could not be kept identical:

- A client's skeleton is built inside its own turn of the `G_RunFrame` client loop, after
  `WP_SaberPositionUpdate` has set this frame's bone angles, and after the clients before it have
  moved, hit it (pain animations, knockback) or locked sabers with it. A pose computed in parallel
  at frame start would use last frame's inputs; the Ghoul2 bone cache is keyed on the frame time,
  not on its inputs, so a stale pose would be used silently.
- The Ghoul2 code (in the renderer library the dedicated server uses) keeps global scratch: world
  matrices, `G2VertSpace`, the bolt reconstruction flags set per call by the game API, and the
  model / bone cache lookups. Making it thread-safe means changing shared renderer code.

What it would take: per-instance input fingerprints (bone list, angles, origin, time) to validate a
precomputed skeleton, thread-local Ghoul2 scratch, and a frame order in which the poses of all
clients are known before the first saber collision. That is a larger change to the simulation
order than this round allowed. `Pmove` stays serial too (players interact, the order matters); the
cheaper serial part found for it is `g_fastJPTrace` (every `Pmove` trace goes through `JP_Trace`).

## 8. Before / after

Measured with `tools/server_perf_interleave.ps1`: one server per point, the two settings switched
in turn every 20 s (A B A B A B), 5-second profile windows, windows that overlap a switch dropped,
9 windows (45 s) per setting. Separate runs drifted by up to 30% within a batch on this machine
(other programs); switching inside one server makes the drift hit both settings alike.

- base: `com_jobs 0`, `bot_mt 0`, `g_fastJPTrace 0`, `sv_botSnapshots 1` (the original code)
- new: the new defaults, `com_jobs -1` (7 workers), `bot_mt 15`, `g_fastJPTrace 1`, `sv_botSnapshots -1`

﻿
| Map | sv_fps | Bots | Frame ms avg base -> new | p99 base -> new | max base -> new | Bot AI ms | Game ms | Snapshots ms | Bot AI traces/frame (made early) |
| --- | ---: | ---: | --- | --- | --- | --- | --- | --- | --- |
| naboo_theed | 40 | 8 | 2.92 -> 2.30 (-21%) | 5.1 -> 4.5 | 6.6 -> 6.5 | 1.42 -> 1.14 (-20%) | 1.41 -> 1.17 (-17%) | 0.114 -> 0.003 | 86 (63) |
| naboo_theed | 40 | 16 | 4.95 -> 3.65 (-26%) | 10.6 -> 8.9 | 12.6 -> 13.3 | 2.50 -> 1.83 (-27%) | 2.18 -> 1.84 (-16%) | 0.291 -> 0.003 | 188 (145) |
| naboo_theed | 40 | 24 | 7.61 -> 5.69 (-25%) | 17.3 -> 12.5 | 20.4 -> 17.2 | 3.84 -> 2.49 (-35%) | 3.22 -> 3.22 (+0%) | 0.578 -> 0.003 | 305 (235) |
| naboo_theed | 40 | 31 | 8.36 -> 7.25 (-13%) | 22.9 -> 16.2 | 26.3 -> 26.9 | 4.26 -> 3.35 (-21%) | 3.41 -> 3.93 (+15%) | 0.710 -> 0.004 | 409 (291) |
| mp/ffa3 | 40 | 8 | 2.64 -> 1.89 (-28%) | 5.0 -> 4.7 | 7.0 -> 7.3 | 0.87 -> 0.69 (-21%) | 1.64 -> 1.21 (-26%) | 0.146 -> 0.003 | 91 (62) |
| mp/ffa3 | 40 | 16 | 4.43 -> 2.94 (-34%) | 9.7 -> 7.7 | 12.8 -> 12.3 | 1.45 -> 1.01 (-30%) | 2.66 -> 1.95 (-27%) | 0.347 -> 0.005 | 198 (110) |
| mp/ffa3 | 40 | 24 | 7.05 -> 4.91 (-30%) | 15.8 -> 12.7 | 21.0 -> 20.2 | 2.52 -> 1.58 (-37%) | 3.99 -> 3.36 (-16%) | 0.580 -> 0.007 | 322 (187) |
| mp/ffa3 | 40 | 31 | 8.91 -> 6.19 (-31%) | 22.2 -> 20.3 | 38.8 -> 52.9 | 3.43 -> 2.23 (-35%) | 4.73 -> 4.00 (-15%) | 0.786 -> 0.010 | 442 (201) |
| naboo_theed | 125 | 8 | 2.87 -> 2.36 (-18%) | 5.9 -> 4.4 | 23.6 -> 12.3 | 1.40 -> 0.99 (-29%) | 1.38 -> 1.37 (-1%) | 0.102 -> 0.002 | 86 (68) |
| naboo_theed | 125 | 16 | 2.23 -> 2.07 (-7%) | 4.7 -> 4.4 | 9.4 -> 71.7 | 1.16 -> 0.90 (-22%) | 0.95 -> 1.17 (+24%) | 0.126 -> 0.002 | 179 (148) |
| naboo_theed | 125 | 24 | 4.99 -> 3.76 (-25%) | 14.5 -> 10.7 | 189.2 -> 109.9 | 2.50 -> 1.71 (-32%) | 2.20 -> 2.05 (-7%) | 0.314 -> 0.004 | 311 (189) |
| naboo_theed | 125 | 31 | 6.66 -> 4.72 (-29%) | 13.8 -> 10.5 | 207.0 -> 144.2 | 3.44 -> 2.33 (-32%) | 2.65 -> 2.40 (-9%) | 0.563 -> 0.006 | 391 (306) |
| mp/ffa3 | 125 | 8 | 1.33 -> 1.26 (-5%) | 3.1 -> 3.6 | 10.4 -> 27.7 | 0.46 -> 0.41 (-9%) | 0.81 -> 0.84 (+4%) | 0.061 -> 0.002 | 94 (64) |
| mp/ffa3 | 125 | 16 | 2.27 -> 1.94 (-15%) | 4.6 -> 4.3 | 9.2 -> 21.0 | 0.73 -> 0.63 (-13%) | 1.38 -> 1.31 (-5%) | 0.166 -> 0.003 | 198 (120) |
| mp/ffa3 | 125 | 24 | 3.60 -> 2.96 (-18%) | 6.7 -> 7.4 | 94.5 -> 144.9 | 1.25 -> 1.06 (-15%) | 2.03 -> 1.90 (-6%) | 0.326 -> 0.004 | 305 (122) |
| mp/ffa3 | 125 | 31 | 4.91 -> 3.54 (-28%) | 13.1 -> 5.9 | 131.5 -> 14.7 | 1.73 -> 1.33 (-23%) | 2.72 -> 2.20 (-19%) | 0.470 -> 0.005 | 441 (124) |

Reading:
- The **average frame** drops at every point but two small ones within noise; 13-34% at 40 Hz,
  5-29% at 125 Hz. The **bot AI** drops 9-37%: with more bots there is more to spread over the
  threads. The **game simulation** drops mostly through `g_fastJPTrace` (up to -27%); two points
  show it higher (+15%, +24%), inside the window-to-window spread of these short runs.
- **Snapshots** go from 0.06-0.79 ms to almost nothing (bots only; a human player still gets his).
- **p99** drops at 14 of 16 points (31 bots: 22.9 -> 16.2 ms naboo, 13.1 -> 5.9 ms ffa3 at 125 Hz).
- The **maxima** are single frames and go both ways (e.g. ffa3 31 bots 40 Hz 38.8 -> 52.9 ms, ffa3
  31 bots 125 Hz 131.5 -> 14.7 ms); they come from scheduling on this busy machine and are not
  attributable to a setting with this many samples.

Which part gives what (naboo_theed, 31 bots, 40 Hz, one interleaved run, 12 windows per setting,
`g_fastJPTrace 1` and `sv_botSnapshots -1` in all of them, so only the parallel part differs):

| Setting | Frame ms (p99) | Bot AI ms | parallel phase wall ms | enemy scan | visibility | command building | wall/duck |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| serial (`com_jobs 0`, `bot_mt 0`) | 6.95 (15.3) | 3.97 | - | 1.42 | 0.46 | 0.50 | 0.38 |
| `bot_mt 7`, 3 workers | 5.99 (12.8) | 3.04 | 0.62 | 0.58 | 0.13 | 0.50 | 0.18 |
| `bot_mt 15`, 3 workers | 5.04 (10.6) | 2.35 | 0.91 | 0.20 | 0.02 | 0.15 | 0.10 |
| `bot_mt 7`, 7 workers | 4.83 (13.1) | 2.33 | 0.61 | 0.25 | 0.05 | 0.41 | 0.12 |
| **`bot_mt 15`, 7 workers** | **4.49 (10.3)** | **2.02** | 0.69 | 0.18 | 0.02 | 0.13 | 0.10 |
| `bot_mt 15`, 3 workers, `com_jobsSpin 3000` | 6.17 (11.6) | 2.83 | 0.94 | 0.28 | 0.03 | 0.18 | 0.13 |

(zone columns: bot AI self time in ms per frame.) The cliff pass (`bot_mt 8`) pays for itself, 7
workers beat 3 on this 4-core / 8-thread CPU, and long spinning hurts. With `bot_mt 15` and 7
workers the bot AI went from 3.97 to 2.02 ms (-49%) and the frame from 6.95 to 4.49 ms (-35%) in
this run. How much work the parallel loops do: at 31 bots on naboo about 6 ms of item time per
frame spread over 4 threads in 1.9 ms of wall time (x3.3; `BOTPROF parallel sense loops` line).

The game phase (sections 2, 8) is the next target; the bot AI at 31 bots is now 2.0-3.4 ms.

## 9. Exactness: `bot_mtVerify`

With `bot_mtVerify 1` every trace result the bots take from the parallel phase is traced again on
the main thread, the normal way (through `JP_Trace` where the bot code uses it), and compared
field by field (fraction, end position, plane, surface flags, contents, entity, solid flags):

| Run | Length | Results used and re-checked | Different |
| --- | --- | ---: | ---: |
| naboo_theed, 31 bots, `sv_fps 125`, defaults (7 workers) | 300 s | 9,598,271 | **0** |
| mp/ffa3, 31 bots, `sv_fps 40`, defaults (7 workers) | 300 s | 2,326,707 | **0** |
| earlier smoke tests (8/16 bots, 3 workers, both maps) | 3 runs, 40-75 s | about 380,000 | **0** |

Since a bot can only see a trace result, and every result it used equalled a fresh trace, the bots
made exactly the decisions they would have made without `bot_mt`. The only remaining difference is
timing: the same traces run earlier and elsewhere.


## 9b. Stability

Two 60-minute runs, naboo_theed, 31 bots, `sv_fps 40`, the bots' real roster, profile every 60 s
(`sv_crashStacks 1` armed):

| Run | Settings | Ran | Crash / fatal error / Ghoul2-from-job warning | Kills in the hour | Parallel sense at the end | Frame / bot AI avg, median of the 59 minute windows |
| --- | --- | --- | --- | ---: | --- | --- |
| default | the new defaults (`com_jobs -1` = 7 workers, `bot_mt 15`, `g_fastJPTrace 1`, `sv_botSnapshots -1`) | 60.0 min, stopped by the script | none | 1770 | 305 traces/frame made early, 283 used, 0.79 ms wall, 3.5 ms of work over 8 threads | 4.74 / 2.30 ms |
| workers 0 | `com_jobs 0`, the rest default | 60.0 min, stopped by the script | none | 1952 | (the sensing ran serially in this run: 307 made early, 288 used, 2.0 ms on the main thread) | 5.63 / 3.43 ms |

Both servers ran the full hour with all 31 bots playing (kill counters rising in every minute), no
crash, no fatal error, no "linking during a parallel loop", no Ghoul2 trace asked for from a job
thread. (The only error line is the 32nd roster bot being refused, as always.)

Both runs show stalls of whole seconds that are not from the server: in the default run five
minutes (19:26-19:31) with frames of up to 12 s, in the workers-0 run frames of up to 1 s. They
sit in the serial game phase (the parallel loops had finished), the slowest single trace in them
took 30-78 ms (a trace takes microseconds; the process was not running), and a memory log taken
during the second run shows the cause: another program on the shared PC (a 5.5 GB Python process
of another session) pushed free memory down to 0.8 GB at 19:48-19:50, the minutes with the worst
frames of that run. Outside such episodes the default run's minute windows have maxima of 9-25 ms.

After the workers-0 run the bot code was changed so that `com_jobs 0` skips the parallel sensing
altogether (the original bot code; the run above exercised the sensing on the main thread, which
`bot_mtVerify 1` still does). The original code path itself ran in every "base" phase of section 8.

Desync: the server has no client-side state to diverge (bots only, no network protocol change);
what was checked is that the server's own state stays consistent, i.e. that every trace result
the bots used equals a fresh trace (section 9).


## 9c. Quality (A/B)

Section 9 already shows that the decisions are identical. As the brief asks for it, the usual A/B
match was played as well: naboo_theed, `sv_fps 40`, 24 bots (the same 12 characters twice,
`bot_abTest 12`), 7-minute FFA matches, `com_jobs -1`; one group with `bot_mt 15`, the other with
`bot_mt 0` (`bot_mtBase`), each orientation twice because slots 12-23 do worse with identical
settings. Kills / deaths:

| Match | Group A (slots 0-11) | Group B (slots 12-23) | `bot_mt 15` in |
| --- | --- | --- | --- |
| 1 | 88/88 (1.00) | 82/97 (0.85) | B |
| 2 | 81/79 (1.03) | 75/93 (0.81) | B |
| 3 | 86/86 (1.00) | 72/88 (0.82) | A |
| 4 | 80/80 (1.00) | 59/67 (0.88) | A |
| **total** | | | `bot_mt 15`: **323/356 = 0.91**; `bot_mt 0`: **300/322 = 0.93** |

Group B is lower in every match whichever setting it has (the known slot effect); the totals over
both orientations differ by 0.02, well inside the match-to-match spread (0.81-0.88 for group B
alone). Consistent with exact decisions; not a measurement of anything else.


## 10. Tools

- `tools/server_perf_interleave.ps1` / `.py`: one server, settings switched in turn at run time,
  per-setting averages of every profile line (frame avg / p99 / max, bot AI, game, snapshots,
  traces, game zones, engine parts, bot zones) as JSON lines. Settings are console commands, so the
  command line stays far below the engine's limit of 32 `+` commands.
- `tools/bot_perf_bench.ps1` (now with `g_profile` and the new lines), `bot_perf_matrix.ps1`,
  `bot_perf_ab.ps1`: as in the bot report.
- `sv_traceBench [count] [length]`: trace scaling over the job threads (section 4).
- `bot_mtVerify 1`: exactness check of the parallel sensing.

## 11. Cvars

| Cvar | Default | Meaning |
| --- | --- | --- |
| `com_jobs` | `-1` | Worker threads of the job system: `-1` = hardware threads - 1 (max 15), `0` = none (everything serial, the original code). Applied between frames. |
| `com_jobsSpin` | `100` | Microseconds a worker spins for the next loop before it sleeps. |
| `bot_mt` | `15` | Parallel bot sensing: 1 enemy scan, 2 visibility, 4 wall + duck probes, 8 cliff check of the movement command (two-pass command building). `0` = off. Needs `com_jobs` for a gain (with `com_jobs 0` the same work runs serially). |
| `bot_mtVerify` | `0` | `1`: every used result is traced again and compared (testing). |
| `bot_mtBase` | `-1` | `bot_mt` of the baseline group in an A/B test (`-1` = as `bot_mt`). |
| `g_fastJPTrace` | `1` | `JP_Trace`'s duel/race scan only over entities that can have a client (same results). `0` = scan every entity. |
| `sv_botSnapshots` | `-1` | Snapshots for bots: `-1` only once the game module reads one, `1` always (original), `0` never. |
| `g_profile` | `0` | Seconds between `GAMEPROF` / `GAMEZONE` reports. |
| `sv_profile` | `0` | Seconds between `SVPROF` reports (now with the engine parts). |
| `sv_gameJobsApi` | `1` (read-only) | Tells the game module that the engine has the job system. |

## 12. Implementation and compatibility

Files: `codemp/qcommon/q_jobs.*` (new), `cm_load.cpp`, `cm_local.h`, `cm_trace.cpp`, `cm_test.cpp`,
`cm_patch.cpp`, `cm_public.h`, `com_profile.h`, `common.cpp`; `codemp/server/sv_world.cpp`,
`sv_gameapi.cpp`, `sv_main.cpp`, `sv_snapshot.cpp`, `sv_bot.cpp`, `sv_ccmds.cpp`, `server.h`;
`codemp/game/ai_main.c`, `g_prof.*` (new), `g_syscalls.c`, `g_main.c`, `g_active.c`, `w_saber.c`,
`w_force.c`, `g_items.c`, `g_missile.c`, `g_mover.c`, `g_utils.c`, `g_misc.c`, `g_public.h`,
`g_local.h`, `g_xcvar.h`; `tools/`.

- No change to the network protocol, snapshots of players, entity bits or the 32-client limit.
- `gameImport_t` grew at its end; a game module built from this tree checks `sv_gameJobsApi`
  before using the new functions, so it still runs on an older engine (bots then serial), and an
  older game module runs on this engine unchanged.
- MSVC x64 builds the engine, the dedicated server and the game module without new warnings;
  all functions are declared before use (`/we4013`). The threading is standard C++11 and
  `thread_local` is only used in the executables (fine on XP, where `thread_local` in a loaded DLL
  is not); the spin hint compiles on GCC/Clang x86 without SSE2 headers and on ARM. Linux, macOS
  and XP were not built here (no compiler for them on this machine); CI will.
- Nothing is allocated while a loop runs; the per-thread collision stamps are allocated before a
  loop when the map or the thread count changed (once per map).

## 13. Limitations

- **Machine noise.** The PC ran other programs; the before/after numbers come from interleaved
  runs (section 8) to cancel drift, but single windows still vary, and maxima are single frames.
- **Hyper-threads and other load.** 7 workers on a 4-core / 8-thread CPU share cores with each
  other and with other programs. A worker that the OS deschedules in the middle of an item makes
  the main thread wait for it; the p99 numbers include that. `com_jobs 3` is the choice for a
  machine that must leave cores to other work.
- **What `bot_mt` cannot cover.** Traces whose inputs are only known in the middle of a think (the
  obstacle probes and saber-combat traces inside the command building and the combat code) stay
  serial: a quarter to a third of the bot traces on naboo_theed, but 40-75% on mp/ffa3, where the bots
  are mostly in close combat and the enemy scan (the largest covered part) has little to do, and the decision logic itself (enemy choice, destination
  choice, the command building) is serial by design. With humans on the server nothing changes in
  the parallel phase (human packets are processed outside the bot frame).
- **Duels.** While a duel is on, `JP_Trace` bends traces for duellists, so bot traces that the trick
  would change are not taken from the parallel phase (correct, but less gain during duels).
- **The game simulation** (`Pmove`, saber collision, Ghoul2) is still serial (section 7).
- **Only FFA with sabers on mp/ffa3 and naboo_theed was measured.** CTF/Siege and gun matches use the
  same code paths but were not timed.

