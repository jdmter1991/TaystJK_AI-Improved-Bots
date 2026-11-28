# The bots in NicJK Unlimited

The bots are the original TaystJK / jaPRO bots (`codemp/game/ai_main.c`), extended piece by piece. Every piece sits behind a switch and was compared against the bots without it in live matches before being turned on. Pieces that lost that comparison are still in the code but off.

The bots play by the rules: they press the same keys a player has, their moves start only when the game would accept them from a player, and they see and hear nothing through walls.

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `bot_features` | `46131711` | Sum of the switches below. |
| `bot_fixes` | `1` | Corrections to old bot code (memory safety, Force power selection). |
| `bot_profile <seconds>` | `0` | Prints the cost of the bot AI and the `BOTEVAL` counters every so many seconds: `BOTPROF` lines (time per frame with percentiles, per bot, per think, thread cycles, think scheduling), `BOTZONE` lines (where the time goes: enemy scan, navigation, visibility, saber, movement checks, usercmd building, ...; self time, calls and traces per frame). See [the performance report](reports/BOT_PERFORMANCE.md). |
| `bot_profileEval` | `1` | `0` leaves the `BOTEVAL` counters out of the `bot_profile` output. |
| `bot_perf` | `7` | Bot AI optimisations (sum of: 1 nearest visible waypoint searched nearest first, 2 dangerous objects listed once per frame for all bots, 4 the shield trace of the waypoint check skipped while no shield exists, 8 enemy scan does not re-trace a candidate found hidden for `bot_scanCacheMs`). 1, 2 and 4 give exactly the old decisions (checked with `bot_perfVerify 1`); 8 can notice someone stepping out of cover up to 100 ms later. |
| `bot_mt` | `15` | Parallel bot sensing on the engine's job threads (`com_jobs`): sum of 1 enemy scan, 2 waypoint and enemy line of sight, 4 wall and duck probes, 8 cliff check of the movement command. The bots decide exactly as with `0` (a stored trace result is only used for a bit-identical trace in an unchanged world; checked with `bot_mtVerify 1`). See [the multithreading report](reports/ENGINE_MULTITHREADING.md). |
| `bot_mtVerify` | `0` | Testing: `1` traces every result taken from the parallel phase again and counts differences (`BOTPROF parallel sense` line). |
| `bot_mtBase` | `-1` | `bot_mt` of the baseline group in an A/B test (`-1` = as `bot_mt`). |
| `bot_perfVerify` | `0` | Testing: `1` also runs the old code for `bot_perf` 1, 2 and 4 and counts disagreements in the `BOTPROF verify` line. |
| `bot_perfBase` | `-1` | `bot_perf` of the baseline group in an A/B test (`-1`: the same). |
| `bot_scanCacheMs` | `100` | How long a hidden enemy candidate is not re-traced with `bot_perf` 8. |
| `bot_ai_fps` | `0` | How many times a second a bot that is not fighting runs its decision making. `0`: every server frame (the original). A bot that is hurt, in the air, duelling, evading, in a move, near its enemy or near any enemy thinks every frame regardless. Movement commands are built every frame. |
| `bot_ai_combatFps` | `0` | The same for the fighting bots; `0`: every server frame (the original). |
| `bot_ai_combatDist` / `bot_ai_threatDist` | `1024` / `600` | Distance to the current enemy / to any enemy at which a bot counts as fighting for `bot_ai_fps`. |
| `bot_ai_fpsBase` | `-1` | `bot_ai_fps` of the baseline group in an A/B test (`-1`: the same). |
| `bot_abTest <n>` | `0` | Testing: client slots `0..n-1` play with `bot_abBase`, slots `n..2n-1` with `bot_features`. `BOTEVAL` then reports kills and deaths per group. |
| `bot_abBase` | `0` | The feature set of the first group in an A/B test. |
| `bot_jk2Tactics` | `15` | Which parts of switch 33554432 (JK2 1.02 tactics) run: 1 guard, 2 red DFA, 4 saber-lock tapping, 8 heal, 16 back attack, 32 red stance, 64 yellow stance, 128 push knockdown, 256 patient fighting. Only 1, 2, 4 and 8 won their tests. |
| `bot_tuneExplore` | `1` | `0` plays the tuned values exactly and learns nothing (for measuring). |
| `bot_tuneEpisode` | `180` | Seconds a bot plays one set of values before it is scored. |

### `bot_features` switches

| Value | On | What it does |
| ---: | :---: | --- |
| 1 | yes | The Force power a bot picked is the one it uses. |
| 2 | yes | Taunts, bows and flourishes after a kill when nothing threatens. |
| 4 | yes | The saber swing direction the bot chose is the one the game gets. |
| 8 | yes | Katas only when one can start and has a chance to land. |
| 16 | yes | Saber stance chosen from what the enemy is doing. |
| 32 | yes | Jump attacks, lunges, roll stabs, back attacks and stab-downs entered with the exact input each needs. |
| 64 | yes | No hesitation in saber range. |
| 128 | yes | Finding the way on maps without bot routes: steering round obstacles, jumping ledges and gaps, doors, getting unstuck. |
| 256 | yes | Evading katas, specials and heavy swings; breaking off when badly hurt. |
| 512 | no | Tactics from a player-written saber guide. Lost its test. |
| 1024 | no | Strafe-swing counters from the same guide. Lost its test. |
| 2048 | yes | Blade prediction: the bot sees where a blade will be and steps out of its path. Reaction time depends on skill. |
| 4096 | no | Learning swing directions from results. Showed no gain. |
| 8192 | yes | Force powers and saber throw used at the ranges and in the situations where the game's code makes them work. |
| 16384 | yes | Stepping in when the bot's own swing is predicted to fall short. |
| 32768 | yes | Tuning by play: distances, timings and chances are adjusted from kills and deaths over many matches and kept in `bottune.dat`. |
| 65536 | yes | All-round awareness: nearby enemies are noticed in any direction, and the bot turns on the bigger threat. |
| 131072 | yes | Force economy: stop and recover at low Force; break a grip with a push at the gripper. |
| 262144 | yes | Guns: lead the target, keep a distance that suits the weapon, release charged shots. |
| 524288 | yes | The world: the use key on doors and buttons that need it, touch buttons, lifts, shoot-to-open, use-triggers. |
| 1048576 | yes | Objectives: flag jobs in Capture the Flag and Capture the Ysalamiri, objectives and classes in Siege. |
| 2097152 | yes | No stepping or jumping off cliffs. |
| 4194304 | no | Walking in a saber fight, running only to close in, chase or get away. Lost its test. |
| 8388608 | yes | Duel timing, from JK2 1.02 duelling: hold at the edge of reach while a swing that will not reach passes, strike at once into a swing that ends, bounces or is parried, strike straight back after blocking, circle at the edge of reach. |
| 33554432 | yes | Tactics for the JK2 1.02 saber rules (only with `g_jk2_102sabersystem 1`; it does nothing otherwise). Parts chosen by `bot_jk2Tactics`, default: let go of attack and face a swing that is about to land (1.02 blocks only a defender in the ready pose with attack released), the red DFA from a jump at the start of a swing, tapping attack in saber locks, heal when hurt. Won 7 of 8 matches (see below). |
| 16777216 | no | MB2 manual block (only with `g_mb2manualblock 1`): a swing read in time is met with a manual block on the side it comes from, aimed for the perfect block when read well. Lost its test (K/D 0.78 against 1.07 over two 7-minute matches): the direction keys walk the bot into the swing, and only about 3 in 10 matched the swing that landed. |

## Saber systems

### MB2-style manual block: `g_mb2manualblock`

Off by default (`0`). With it off the game plays exactly as base JKA. With `1` the server uses blocking rules modelled on Movie Battles II (MB2 is closed source; the rules follow its community dueling guides), and the bots play by them.

| Action | Input | Effect |
| --- | --- | --- |
| Guard | Hold alt attack, saber lit | Saber hits from the front are blocked, at a cost in block points. You keep your stance's normal pose (as in MB2) and can only walk. Attack while guarding is a manual block, not a swing. |
| Manual block | While guarding, press attack with a direction key (W top, A left, D right, W+A top left, S+A bottom left, and so on) | Shows the parry pose for that side and stays open for 0.4 s, then cannot be used again for 1.5 s. If the swing that lands comes from that side, it costs half the block points. If it is also a perfect block, the attacker's saber is knocked out of his hand, unless he was guarding himself. |
| Perfect block | Guard with your aim on the side the swing comes from (an 8° zone, `g_mb2pbZone`) | Costs nothing, gives you 2 points back, takes 5 from the attacker and bounces his swing. |
| Saber throw | Alt attack + use | As base JKA. |

Block points (0 to 100) are shown in the fuel bar on the right: gold, turning red as they run out, labelled GUARD while guarding and MANUAL while a manual block is open. The server keeps them in the cloak fuel field, so the bar also shows in clients without this build. A block costs `100 × attacker's offence ÷ defender's defence` per saber style (blue 7/55, yellow 9/50, red 12/48, purple 11/48, cyan 8/48, duals 8/50, staff 8/53), more when crouched, in the air, at low saber defence or after a manual block on the wrong side; less against a chained swing. Points come back (`g_mb2bpRegen`, every `g_mb2bpRegenTime` ms) only when not running or attacking. Guarding with no points left gets your saber knocked away and the swing lands. Hits from behind are never blocked.

What the bots do with it: they guard when a saber can reach them, swing without the guard when nothing is about to land, aim for perfect blocks more often at higher skill, use manual blocks with switch 16777216 (off: it lost its test), and back off at walking pace when their points run out.

### JK2 1.02 saber system: `g_jk2_102sabersystem`

Off by default. With `1` the server plays the Jedi Outcast 1.02 saber, Force, movement and weapon rules, ported from jk2mv's game code (see `reports/JK2_102_COMPARISON.md` in the work folder). Run it at `sv_fps 20`, the 1.02 server rate.

What the bots do with it (switch 33554432, parts in `bot_jk2Tactics`). Everything is read from what a player sees: the enemy's animation (which saber move he is in and how far into it), where he stands and looks, and the bot's own state.

| Part | Value | What it does | On |
| --- | ---: | --- | :---: |
| Guard | 1 | In 1.02 a defender blocks by himself, but only in the ready pose or a parry, with attack released, and with the hit in front (saber defence 3: anything in front). When an enemy's blade is predicted to reach the bot within 150 ms while it stands ready, it lets go of attack and turns to face him instead of starting a swing that cannot land first. No guard against red below defence 3, back attacks or the red DFA, which cannot be blocked. | yes |
| Red DFA | 2 | The strong style's jump attack (+40 damage, unblockable) exists in 1.02 only as a forward jump within 500 ms of an attack move starting. With an enemy 100 to 200 units ahead the bot starts an overhead swing and jumps as its attack part begins. The JKA jump-attack input is not used for red in this mode. | yes |
| Lock | 4 | In a saber lock every third new press of attack pushes: the bot taps attack (one press every other frame). | yes |
| Heal | 8 | Heal costs 25 Force at every level in 1.02 (25 health at level 2, 50 at level 3): used when hurt by at least that much and no enemy swing is close. | yes |
| Back attack | 16 | Back + attack when an enemy is right behind, where the game's 128-unit back trace finds him (+20, unblockable). | no |
| Red stance | 32 | Red as the base stance (blue when hurt at close range). Replaces switch 16 in this mode. | no |
| Yellow stance | 64 | Yellow as the base stance. | no |
| Push | 128 | A level 3 push at an enemy within 120 (it knocks him down when his push level is lower or he is moving), then strike while he is down. | no |
| Patient | 256 | Stand ready instead of swinging into an enemy who can block or whose swing lands first; strike when he cannot block (return, bounce, broken parry, facing away, swinging elsewhere, or in a start long enough for our swing to land first). | no |

Results. Each line is 12 bots with switch 33554432 against 12 without, 24 bots, 7 minutes, sabers only, Free For All, `sv_fps 20`, `g_jk2_102sabersystem 1`; kills / deaths of each group summed over the matches. In these tests the bots in client slots 12 to 23 did worse than those in slots 0 to 11 even with identical settings (three A/A matches on each map: K/D 0.92 against 1.04 on mp/ffa3, 0.90 against 0.97 on swtor_korriban), so the final comparisons were run with the groups both ways round. The single parts were tested first, mostly with the switch in slots 12 to 23 (the numbers in brackets give how many matches were run each way), which puts them a little low.

| Setting | Map | Matches | Without | With | Kills with / without |
| --- | --- | ---: | --- | --- | ---: |
| 15 (default: guard, DFA, lock, heal) | mp/ffa3 | 4 (2 each way) | 827 / 905 | 973 / 927 | 1.18 |
| 15 | swtor_korriban | 4 (2 each way) | 466 / 545 | 530 / 526 | 1.14 |
| 79 (15 + yellow stance) | mp/ffa3 | 4 (2 each way) | 870 / 912 | 878 / 866 | 1.01 |
| 79 | swtor_korriban | 4 (2 each way) | 530 / 573 | 493 / 523 | 0.93 |
| 1 guard alone | mp/ffa3 | 3 | 665 / 640 | 643 / 684 | 0.97 |
| 1 guard alone | swtor_korriban | 3 | 346 / 362 | 330 / 357 | 0.95 |
| 2 red DFA alone | mp/ffa3 | 3 | 649 / 639 | 673 / 699 | 1.04 |
| 2 red DFA alone | swtor_korriban | 3 | 406 / 398 | 351 / 426 | 0.86 |
| 8 heal alone | mp/ffa3 | 3 (2 + 1) | 710 / 668 | 616 / 674 | 0.87 |
| 8 heal alone | swtor_korriban | 3 (1 + 2) | 403 / 454 | 365 / 369 | 0.91 |
| heal + push (before they were split) | mp/ffa3 | 3 | 658 / 665 | 637 / 662 | 0.97 |
| heal + push | swtor_korriban | 3 | 425 / 416 | 320 / 395 | 0.75 |
| 16 back attack | mp/ffa3 | 3 | 665 / 616 | 608 / 679 | 0.91 |
| 16 back attack | swtor_korriban | 3 | 362 / 340 | 326 / 392 | 0.90 |
| 32 red stance | mp/ffa3 | 3 | 693 / 669 | 669 / 719 | 0.97 |
| 32 red stance | swtor_korriban | 3 | 394 / 375 | 357 / 437 | 0.91 |
| 64 yellow stance | mp/ffa3 | 3 | 661 / 652 | 678 / 701 | 1.03 |
| 256 patient | mp/ffa3 | 3 (1 + 2) | 670 / 669 | 621 / 648 | 0.93 |

The parts alone gained nothing that stands out from the noise (and lost on the bigger map); together, guard, DFA, lock and heal won 7 of 8 matches. In the default set over those eight matches: 2,521 guards, of which 345 ended in a block and 326 in a hit taken (the rest saw the swing miss or turn away); 674 DFA jumps, 533 DFAs started, 89 landed for 58 kills; 290 heals. Duels (`g_gametype 3`, one bot each, mp/duel1): the default set won 13-0 and lost 5-7; the full set (63) won 12-1; set 79 won 12-3 and lost 6-12. The bots with the switch won all 9 saber locks in these duels. The duel results follow the client slot (the bot in slot 1 won all three of its duels, the one in slot 0 lost both), so they say little about the switch (an A/A duel went 10-3 to slot 1).

Why the others are off: back attacks start in only about a third of the tries (the game's narrow back trace) and walk the bot backwards meanwhile; pushes knock the enemy down (about 7 in 10) but throw him out of reach, so the fight breaks up; the red stance starts too slowly against bots that never stop swinging (1.02 gives 125 health and 25 armour, so red needs two hits); patient fighting leaves bots facing each other at the edge of reach, because the bots without it hold back there too (switch 8388608).

## How well it works

Measured in bot-against-bot matches on a dedicated server. One match is noisy: two identical groups of 12 bots can differ by 20% in kills over seven minutes, so results were repeated before a switch was turned on.

| Area | State |
| --- | --- |
| Saber fighting, Force use, awareness | Each switch that is on won or held its own in repeated A/B matches. |
| Free For All on a huge map without bot routes | Works. Bots find each other, use doors, get unstuck or respawn. |
| Team Free For All | Works. No kills of teammates in any test. |
| Capture the Flag | Bots take and capture flags (2 captures in a 12-minute, 16-bot match on a stock map, where the old bots captured none). Carriers still die too often. |
| Capture the Ysalamiri | Shares the flag logic. Not yet measured in a long match. |
| Guns | In gun-only matches the gun logic holds its own. Hit rate is about 25%; rocket lock-on and sniper zoom are not implemented. |
| Siege | Bots find the objective, pick classes and use explosives on what needs them, but did not complete an objective in the test matches. Work in progress. |
| Doors and switches that need the use key | Implemented, but not exercised by any test map so far. |
| Lifts | Bots board and ride them on stock maps. |
| JK2 1.02 rules (`g_jk2_102sabersystem 1`) | Bots play the 1.02 saber; switch 33554432 adds guard, red DFA, lock tapping and heal and won 7 of 8 A/B matches (kills +18% on mp/ffa3, +14% on swtor_korriban). Only about 13% of full saber hits are blocked: bots without the guard hold attack, and 1.02 does not block for someone holding attack. |
| MB2-style blocking (`g_mb2manualblock 1`) | Bots guard, perfect-block and back off at low block points. In a 24-bot, 7-minute match: about 2,000 guarded hits blocked (about 1 in 9 perfect), 140 got through, 60 disarms. |

## Performance

The bot AI costs about 14 collision traces per bot per server frame; with 31 bots that is 1.5-3 ms of a server frame on mp/ffa3 and naboo_theed. On large open maps the enemy scan is the biggest part (a third of the time on naboo_theed: many enemies in view but behind walls). `bot_perf 7` (default) removes redundant traces with exactly the same decisions (bot AI traces -4 to -10%). On servers above 40 Hz, `bot_perf 15` plus `bot_ai_fps 20` halves the bot AI time on naboo_theed at 125 Hz while fighting bots keep thinking every frame; it is not on by default because its A/B test on naboo_theed was not conclusive. Measurements, the A/B results and how to repeat them: [reports/BOT_PERFORMANCE.md](reports/BOT_PERFORMANCE.md) (`tools/bot_perf_bench.ps1`, `bot_perf_matrix.ps1`, `bot_perf_ab.ps1`).

## Reading the test output

With `bot_profile` set, the server log gets `BOTPROF` lines (time per function) and `BOTEVAL` lines: kills and deaths per group, what happened in the match (team scores, flags taken and captured, Siege objectives), kills by weapon type, shots and hits, falls, and a counter for nearly every decision described above. They are the basis for every claim on this page.
