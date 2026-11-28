#!/bin/sh
# Bot benchmarks on naboo_theed, uncapped frame rate, from devgame_naboo (NicJK names).
#   run_bot_bench.sh client <width> <height> <label> [render_width render_height]   31 bots + the local player
#   run_bot_bench.sh server <label>                                                 32 bots on a dedicated server
# The game allows 32 clients, so a client-side frame-rate test has room for 31 bots; 32 bots fill a dedicated
# server, where the measure is the server frame time (resolution does not apply).
# Bots come from the user's own list through the shared helper tools\bot_names.ps1 (read, never edited).
# A render size larger than the window (4K on a 1440p screen) uses r_renderWidth / r_renderHeight.
# Refuses to start while another game is running unless ALLOW_BUSY=1. BENCH_CMDS="r_tworldStats 1; ..." runs console commands
# when the bots are in (the command line is full: the engine takes at most 32 "+" commands).
R="/e/jedi academy modding archive/TaystJK_AI_Claude_Code_improvements"
D="$R/devgame_naboo"; OUT="$R/naboo/export/city/bench"
GD="E:\\jedi academy modding archive\\Jedi Academy TaystJK_AI test\\GameData"
mkdir -p "$OUT"
busy=$(powershell -NoProfile -Command "(Get-Process | Where-Object { \$_.Name -like 'nicjk*' -or \$_.Name -like 'taystjk*' -or \$_.Name -eq 'blender' } | Measure-Object).Count" 2>/dev/null | tr -d '\r\n ')
if [ "$busy" != "0" ] && [ "$ALLOW_BUSY" != "1" ]; then echo "BUSY: $busy game or Blender process(es) running; not starting"; exit 3; fi
cd "$D" || exit 1
bots() { powershell -NoProfile -Command ". '$(cygpath -w "$R/tools/bot_names.ps1")'; Get-BotLines -Bots $1" | tr -d '\r' > "nicjk/naboo_bots_user$1.cfg"; echo "bot lines: $(grep -c addbot "nicjk/naboo_bots_user$1.cfg")"; }

if [ "$1" = "client" ]; then
  W=$2; H=$3; L=$4; RW=${5:-0}; RH=${6:-0}
  bots 31
  # one pass: five camera positions, 3000 frames each; the log's timestamps give the duration
  py - "$D/nicjk/naboo_botbench.cfg" <<'EOF'
import sys, os
EXTRA = os.environ.get("BENCH_CMDS", "").strip().rstrip(";")
EXTRA = (EXTRA + "; ") if EXTRA else ""
P = [(669, 3583, 60, 42), (3031, 4291, 140, -167), (-1693, 1457, 110, 42), (669, 3583, 1400, 42), (700, -9000, 5000, 90), (362, 9000, 400, 90),
     (-672, 8300, 620, 90), (360, 5400, 160, 90)]      # last two: a palace hall with many candles, the palace forecourt
L = []
for c in (1, 2):
    for i, (x, y, z, yaw) in enumerate(P):
        nxt = "vstr c%d_p%d" % (c, i + 1) if i + 1 < len(P) else ("vstr c2_p0" if c == 1 else "echo BENCH_END; wait 20; quit")
        # r_tworldBench: every change of the label prints the renderer's frame-time figures of the section that ended
        L.append('set c%d_p%d "r_tworldBench move; setviewpos %d %d %d %d; wait 150; r_tworldBench pass%d_pos%d; echo BENCH_BEGIN pass %d position %d; wait 3000; echo BENCH_STOP pass %d position %d; r_tworldBench move; %s"' % (c, i, x, y, z, yaw, c, i, c, i, c, i, nxt))
L.append('set benchgo "com_maxfps 125; wait 3500; team s; wait 400; cg_draw2D 1; cg_drawFPS 1; ' + EXTRA + 'echo BENCH_BOTS_READY; set com_maxfps 0; vstr c1_p0"')
L.append("vstr benchgo")
open(sys.argv[1], "w").write("\n".join(L) + "\n")
EOF
  EXTRA=""
  if [ "$RW" != "0" ]; then EXTRA="+set r_renderWidth $RW +set r_renderHeight $RH +set r_renderScale 1"; else EXTRA="+set r_renderScale 0"; fi
  rm -f nicjk/qconsole.log
  ( sleep 150; nvidia-smi --query-gpu=memory.used,utilization.gpu --format=csv,noheader > "$OUT/gpu_$L.txt" 2>/dev/null ) &
  timeout 1500 ./nicjk.x86_64.exe +set fs_basepath "$GD" +set fs_homepath "$(cygpath -w "$D")" +set fs_game nicjk +set cl_renderer rd-vulkant \
    +set r_tworldInstancing 1 +set r_tworldShadows 1 +set r_tworldPBR 1 +set r_tworldLamps 1 +set r_tworldIndoor 0.55 +set r_tworldAO 0.6 \
    +set r_mode -1 +set r_fullscreen 0 +set r_customwidth $W +set r_customheight $H $EXTRA +set r_swapInterval 0 \
    +set model NikolayAngelovPrequel/default_hot +set color1 4 +set color2 4 \
    +set sv_maxclients 32 +set g_gametype 0 +set bot_minplayers 0 +set g_inactivity 0 +set fraglimit 0 +set timelimit 0 +set sv_profile 30 \
    +set net_port 29181 +set logfile 2 +devmap naboo_theed +exec naboo_bots_user31.cfg +exec naboo_botbench.cfg > "client_bench_$L.out" 2>&1
  echo "exit=$?"; wait
  cp nicjk/qconsole.log "$OUT/client_$L.qconsole.log"
  py - "$OUT/client_$L.qconsole.log" "$L" "$OUT/gpu_$L.txt" <<'EOF'
import sys, re, datetime, json, os
log = open(sys.argv[1], errors="replace").read().splitlines()
def ts(l): return datetime.datetime.strptime(l[:19], "%Y-%m-%d %H:%M:%S")
beg = {}; res = {}
for l in log:
    m = re.search(r"BENCH_(BEGIN|STOP) pass (\d) position (\d)", l)
    if m:
        k = (int(m.group(2)), int(m.group(3)))
        if m.group(1) == "BEGIN": beg[k] = ts(l)
        elif k in beg: res[k] = 3000.0 / max((ts(l) - beg[k]).total_seconds(), 0.5)
bots = sum(1 for l in log if "entered the game" in l)
missing = sorted({re.sub(r".*Unable to find bot '?", "", l).strip("' ") for l in log if "Unable to find bot" in l or "Bot not found" in l})
modes = [l[20:] for l in log if "MODE:" in l or "render:" in l.lower()][:2]
# frame rates from the renderer's recorder (TWBENCH lines), not from wait counts
tw = {}
for l in log:
    m = re.search(r"TWBENCH pass(\d)_pos(\d+): frames (\d+), avg ([\d.]+) fps \(([\d.]+) ms\), 1% low ([\d.]+) fps", l)
    if m:
        tw[(int(m.group(1)), int(m.group(2)))] = (float(m.group(4)), float(m.group(6)), int(m.group(3)), float(m.group(5)))
NP = 1 + max([k[1] for k in tw] or [0])
per = {"position %d" % p: [[round(tw[(c, p)][0]), round(tw[(c, p)][1])] if (c, p) in tw else None for c in (1, 2)] for p in range(NP)}
tot = [round(sum(tw[(c, p)][2] for p in range(NP)) / (sum(tw[(c, p)][2] * tw[(c, p)][3] for p in range(NP)) / 1000.0)) if all((c, p) in tw for p in range(NP)) else None for c in (1, 2)]
gpu = open(sys.argv[3]).read().strip() if os.path.exists(sys.argv[3]) else None
print(json.dumps({"label": sys.argv[2], "fps_avg_and_1pct_low_per_position_pass1_pass2": per, "fps_whole_pass": tot, "clients_entered": bots, "bots_not_found": missing, "gpu_memory_and_load_at_150s": gpu, "mode_lines": modes,
                  "errors": [l[20:140] for l in log if "ERROR" in l][:3]}, indent=1))
EOF
elif [ "$1" = "server" ]; then
  L=$2
  bots 32
  rm -f nicjk/qconsole.log
  timeout 420 ./nicjkded.x86_64.exe +set fs_basepath "$GD" +set fs_homepath "$(cygpath -w "$D")" +set fs_game nicjk +set dedicated 1 +set net_port 29180 \
    +exec naboo_server.cfg +set sv_profile 30 +set bot_profile 30 +devmap naboo_theed +exec naboo_bots_user32.cfg +wait 9000 +quit > "server_bench_$L.out" 2>&1
  echo "exit=$?"
  cp nicjk/qconsole.log "$OUT/server_$L.qconsole.log"
  grep -aE "SVPROF (window|ms/frame|botai|game )|ERROR" nicjk/qconsole.log | cut -c21-260 | tail -12
  echo "hitch lines: $(grep -ac 'SVPROF HITCH' nicjk/qconsole.log)   clients entered: $(grep -ac 'entered the game' nicjk/qconsole.log)"
  grep -aiE "Unable to find bot|Bot not found" nicjk/qconsole.log | cut -c21-120 | sort | uniq -c | head
fi
exit 0
