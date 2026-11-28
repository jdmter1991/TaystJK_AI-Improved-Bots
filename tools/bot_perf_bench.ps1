# Bot AI performance benchmark: runs a dedicated server with N bots and reports server frame time,
# bot AI time per frame, time per bot and the bot_profile zone breakdown, averaged over the windows
# that fall after the warm-up. One run per call; loop over it for a matrix (see docs/reports/BOT_PERFORMANCE.md).
#   bot_perf_bench.ps1 -DevDir <dir> -Map mp/ffa3 -Bots 16 [-SvFps 40] [-Extra "+set bot_ai_fps 20"] -Tag name
# Nothing is written outside -DevDir: the server runs with fs_homepath = DevDir and reads -BasePath only.
# -Seed: a folder with botlearn.dat / bottune.dat copied into DevDir\nicjk before the run, so every
# run starts from the same learned tables (the bots keep learning while they play).
param(
    [Parameter(Mandatory = $true)][string]$DevDir,
    [int]$Bots = 8,
    [string]$Map = "mp/ffa3",
    [int]$SvFps = 40,
    [int]$Warmup = 30,
    [int]$Duration = 60,
    [int]$Window = 10,
    [string]$BasePath = "E:\jedi academy modding archive\Jedi Academy TaystJK_AI test\GameData",
    [int]$Port = 29240,
    [string]$Extra = "",
    [string]$Tag = "",
    [string]$Seed = "",
    [string]$Results = "",
    [switch]$Eval       # also print the BOTEVAL counters (kills, deaths, ...) each window
)
$ErrorActionPreference = "Stop"
$exe = Join-Path $DevDir "nicjkded.x86_64.exe"
$modDir = Join-Path $DevDir "nicjk"
. (Join-Path $PSScriptRoot "bot_names.ps1")
$cycle = if ($Extra -match 'bot_abTest\s+(\d+)') { [int]$Matches[1] } else { 0 }
$lines = Get-BotLines -Bots $Bots -Cycle $cycle
($lines -join "`r`n") + "`r`n" | Out-File -Encoding ascii (Join-Path $modDir "perf_bots.cfg")
if ($Seed) { Copy-Item (Join-Path $Seed "*.dat") $modDir -Force }
# archived cvars from an earlier run (nicjk_server.cfg is written by the server) must not carry over
Remove-Item (Join-Path $modDir "nicjk_server.cfg") -Force -ErrorAction SilentlyContinue
if (-not $Tag) { $Tag = "{0}_b{1}_fps{2}" -f ($Map -replace '[/\\]', '_'), $Bots, $SvFps }
if (-not $Results) { $Results = Join-Path $DevDir "perf_results.jsonl" }
$qlog = Join-Path $modDir "qconsole.log"
if (Test-Path $qlog) { Remove-Item $qlog -Force }

$argList = "+set fs_basepath `"$BasePath`" +set fs_homepath `"$DevDir`" +set fs_game nicjk +set dedicated 1 " +
    "+set net_port $Port +set com_hunkmegs 2048 +exec bench.cfg +set sv_fps $SvFps +set sv_profile $Window +set bot_profile $Window +set g_profile $Window +set bot_profileEval $(if ($Eval) { 1 } else { 0 }) " +
    "$Extra +set logfile 1 +map $Map +exec perf_bots.cfg"
# console output goes to NUL: writing it to a redirected file stalled the server for seconds
# (NtWriteFile inside Com_Printf, seen with com_sampleProfile), which distorted every timing
$cmd = Start-Process -FilePath "cmd.exe" -ArgumentList "/c `"`"$exe`" $argList > NUL 2>&1`"" -WorkingDirectory $DevDir -PassThru -WindowStyle Hidden
$p = $null
for ($i = 0; $i -lt 50 -and -not $p; $i++) {
    Start-Sleep -Milliseconds 200
    $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($cmd.Id)" | Where-Object { $_.Name -like "nicjkded*" } | Select-Object -First 1
    if ($child) { $p = Get-Process -Id $child.ProcessId }
}
if (-not $p) { throw "server process did not start" }
try { $p.PriorityClass = "High" } catch {}
$start = Get-Date
try {
    # wait until the map is loaded and the bots are in (the first BOTPROF window), then the warm-up
    $deadline = (Get-Date).AddSeconds(240)
    while (-not ((Test-Path $qlog) -and (Select-String -Path $qlog -Pattern "^.{20}BOTPROF window" -Quiet))) {
        Start-Sleep -Seconds 1
        $p.Refresh()
        if ($p.HasExited) { throw "server exited during load, code $($p.ExitCode)" }
        if ((Get-Date) -gt $deadline) { throw "no BOTPROF output within 240 s" }
    }
    $loaded = Get-Date
    Start-Sleep -Seconds $Warmup
    $p.Refresh()
    $cpu0 = $p.TotalProcessorTime.TotalSeconds; $w0 = Get-Date
    Start-Sleep -Seconds $Duration
    $p.Refresh()
    if ($p.HasExited) { throw "server exited during the run, code $($p.ExitCode)" }
    $cpu = $p.TotalProcessorTime.TotalSeconds - $cpu0
    $wall = ((Get-Date) - $w0).TotalSeconds
    $measureFrom = $w0; $measureTo = Get-Date
}
finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $null = $p.WaitForExit(15000) }
}
Copy-Item $qlog (Join-Path $DevDir "perf_$Tag.qconsole.log") -Force

# ---- parse the windows that ended inside the measured interval ----
$sv = @(); $bp = @(); $kc = @(); $sch = @(0, 0, 0); $why = @(0, 0, 0, 0, 0, 0); $bz = @{}; $pct = @(); $tr = @(); $ev = $null
$gp = @(); $gz = @{}; $pz = @{}
$inWin = $false
foreach ($line in Get-Content $qlog) {
    if ($line.Length -lt 20) { continue }
    $t = [datetime]::MinValue
    if (-not [datetime]::TryParseExact($line.Substring(0, 19), "yyyy-MM-dd HH:mm:ss", [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$t)) { continue }
    $msg = $line.Substring(20)
    if ($t -lt $measureFrom.AddSeconds($Window - 1) -or $t -gt $measureTo.AddSeconds(1)) { continue }
    if ($msg -match '^SVPROF window=\S+ frames=(\d+) gameframes=(\d+) bots=(\d+) humans=\d+ over_budget=(\d+)') {
        $cur = [ordered]@{ frames = [int]$Matches[1]; gameframes = [int]$Matches[2]; bots = [int]$Matches[3]; over = [int]$Matches[4] }
        $sv += , $cur
    } elseif ($msg -match '^SVPROF ms/frame avg\(max\): total ([\d.]+) \(([\d.]+)\)\s+botai ([\d.]+) \(([\d.]+)\)\s+game ([\d.]+) \(([\d.]+)\)\s+snap ([\d.]+) \(([\d.]+)\)') {
        if ($sv.Count) { $c = $sv[-1]; $c.total = [double]$Matches[1]; $c.totalMax = [double]$Matches[2]; $c.botai = [double]$Matches[3]; $c.botaiMax = [double]$Matches[4]; $c.game = [double]$Matches[5]; $c.gameMax = [double]$Matches[6]; $c.snap = [double]$Matches[7] }
    } elseif ($msg -match '^SVPROF percentiles ms: total p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+)\s+botai p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+)') {
        if ($sv.Count) { $c = $sv[-1]; $c.tP50 = [double]$Matches[1]; $c.tP95 = [double]$Matches[2]; $c.tP99 = [double]$Matches[3]; $c.bP99 = [double]$Matches[6] }
    } elseif ($msg -match '^SVPROF percentiles ms: game p50 ([\d.]+) p99 ([\d.]+)\s+snap p50 ([\d.]+) p99 ([\d.]+)') {
        if ($sv.Count) { $c = $sv[-1]; $c.gP99 = [double]$Matches[2]; $c.sP99 = [double]$Matches[4] }
    } elseif ($msg -match '^SVPROF part (.+?)\s+ms/frame avg ([\d.]+) p99 ([\d.]+) max ([\d.]+)\s+calls/frame ([\d.]+)') {
        $n = $Matches[1].Trim()
        if (-not $pz.ContainsKey($n)) { $pz[$n] = @() }
        $pz[$n] += , @([double]$Matches[2], [double]$Matches[3], [double]$Matches[4], [double]$Matches[5])
    } elseif ($msg -match '^GAMEPROF window=\S+ frames=(\d+) clients=\d+ bots=\d+ entities=(\d+)\s+game ms/frame avg ([\d.]+) p99 ([\d.]+) max ([\d.]+)') {
        $gp += , [ordered]@{ frames = [int]$Matches[1]; ents = [int]$Matches[2]; avg = [double]$Matches[3]; p99 = [double]$Matches[4]; max = [double]$Matches[5] }
    } elseif ($msg -match '^GAMEZONE (.+?)\s+self ms/frame avg\s+([\d.]+) p99\s+([\d.]+) max\s+([\d.]+)\s+[\d.]+%\s+calls/frame\s+([\d.]+)') {
        $n = $Matches[1].Trim()
        if (-not $gz.ContainsKey($n)) { $gz[$n] = @() }
        $gz[$n] += , @([double]$Matches[2], [double]$Matches[3], [double]$Matches[4], [double]$Matches[5])
    } elseif ($msg -match '^SVPROF botai traces/frame ([\d.]+)\s+trace ms/frame ([\d.]+)') {
        if ($sv.Count) { $sv[-1].aiTraces = [double]$Matches[1]; $sv[-1].aiTraceMs = [double]$Matches[2] }
    } elseif ($msg -match '^SVPROF game\s+traces/frame ([\d.]+)\s+trace ms/frame ([\d.]+)') {
        if ($sv.Count) { $sv[-1].gameTraces = [double]$Matches[1]; $sv[-1].gameTraceMs = [double]$Matches[2] }
    } elseif ($msg -match '^BOTPROF window=\S+ frames=(\d+) \(no-think frames (\d+)\) thinks=(\d+)\s+ai ms/frame avg ([\d.]+) max ([\d.]+)\s+think us avg ([\d.]+) max ([\d.]+)') {
        $bp += , [ordered]@{ frames = [int]$Matches[1]; thinks = [int]$Matches[3]; ai = [double]$Matches[4]; aiMax = [double]$Matches[5]; think = [double]$Matches[6]; thinkMax = [double]$Matches[7] }
    } elseif ($msg -match '^BOTPROF scheduler \(bot_ai_fps \d+, combat \d+\): thinks in a fight (\d+), on schedule (\d+), frames without a think (\d+)') {
        $sch[0] += [int]$Matches[1]; $sch[1] += [int]$Matches[2]; $sch[2] += [int]$Matches[3]
    } elseif ($msg -match '^BOTPROF scheduler, why in a fight: hurt (\d+), in the air (\d+), move or duel (\d+), enemy near (\d+), gun fight (\d+), threat near (\d+)') {
        for ($k = 0; $k -lt 6; $k++) { $why[$k] += [int]$Matches[$k + 1] }
    } elseif ($msg -match '^BOTPROF frame ai kcycles ([\d.]+) \(per bot ([\d.]+)\)') {
        $kc += , [ordered]@{ frames = 1; kc = [double]$Matches[1]; kcBot = [double]$Matches[2] }
    } elseif ($msg -match '^BOTPROF frame ai ms: p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+) max [\d.]+; bots (\d+), thinks per frame ([\d.]+), ai us per bot per frame ([\d.]+)') {
        $pct += , [ordered]@{ p50 = [double]$Matches[1]; p95 = [double]$Matches[2]; p99 = [double]$Matches[3]; bots = [int]$Matches[4]; tpf = [double]$Matches[5]; perBot = [double]$Matches[6] }
    } elseif ($msg -match '^BOTZONE (.+?)\s+self ms/frame\s+([\d.]+)\s+[\d.]+%\s+calls/frame\s+([\d.]+)\s+self us/call\s+[\d.]+\s+incl us/call\s+[\d.]+ max\s+[\d.]+\s+traces/frame\s+([\d.]+)') {
        $n = $Matches[1].Trim()
        if (-not $bz.ContainsKey($n)) { $bz[$n] = @() }
        $bz[$n] += , @([double]$Matches[2], [double]$Matches[3], [double]$Matches[4])
    } elseif ($msg -match '^BOTEVAL baseline: bots (\d+) kills (\d+) deaths (\d+) \| features \d+: bots (\d+) kills (\d+) deaths (\d+)') {
        $ev = "A bots $($Matches[1]) kills $($Matches[2]) deaths $($Matches[3]) | B bots $($Matches[4]) kills $($Matches[5]) deaths $($Matches[6])"
    } elseif ($msg -match '^BOTEVAL all bots: features \d+ bots (\d+) kills (\d+) deaths (\d+)') {
        $ev = "bots $($Matches[1]) kills $($Matches[2]) deaths $($Matches[3])"
    }
}
function WAvg($rows, $key) {
    $w = 0; $s = 0.0
    foreach ($r in $rows) { if ($null -ne $r[$key]) { $s += $r[$key] * $r.frames; $w += $r.frames } }
    if ($w) { [math]::Round($s / $w, 3) } else { $null }
}
function Avg($rows, $key) { $v = @($rows | ForEach-Object { $_[$key] } | Where-Object { $null -ne $_ }); if ($v.Count) { [math]::Round(($v | Measure-Object -Average).Average, 3) } else { $null } }
function MaxOf($rows, $key) { $v = @($rows | ForEach-Object { $_[$key] } | Where-Object { $null -ne $_ }); if ($v.Count) { ($v | Measure-Object -Maximum).Maximum } else { $null } }
$zones = [ordered]@{}
foreach ($k in ($bz.Keys | Sort-Object { - ($bz[$_] | ForEach-Object { $_[0] } | Measure-Object -Sum).Sum })) {
    $rows = $bz[$k]
    $zones[$k] = @([math]::Round(($rows | ForEach-Object { $_[0] } | Measure-Object -Average).Average, 4),
                   [math]::Round(($rows | ForEach-Object { $_[1] } | Measure-Object -Average).Average, 2),
                   [math]::Round(($rows | ForEach-Object { $_[2] } | Measure-Object -Average).Average, 2))
}
function ZoneSummary($h) {
    # per zone: [avg ms/frame, p99 (mean of the window p99s), max of the window maxima, calls/frame]
    $o = [ordered]@{}
    foreach ($k in ($h.Keys | Sort-Object { - ($h[$_] | ForEach-Object { $_[0] } | Measure-Object -Sum).Sum })) {
        $rows = $h[$k]
        $o[$k] = @([math]::Round(($rows | ForEach-Object { $_[0] } | Measure-Object -Average).Average, 4),
                   [math]::Round(($rows | ForEach-Object { $_[1] } | Measure-Object -Average).Average, 3),
                   [math]::Round(($rows | ForEach-Object { $_[2] } | Measure-Object -Maximum).Maximum, 3),
                   [math]::Round(($rows | ForEach-Object { $_[3] } | Measure-Object -Average).Average, 2))
    }
    $o
}
$res = [ordered]@{
    tag = $Tag; map = $Map; botsAsked = $Bots; bots = (MaxOf $sv 'bots'); svFps = $SvFps; extra = $Extra; windows = $sv.Count
    cpuCorePct = [math]::Round(100 * $cpu / $wall, 1)
    frameMs = (WAvg $sv 'total'); frameP50 = (Avg $sv 'tP50'); frameP95 = (Avg $sv 'tP95'); frameP99 = (Avg $sv 'tP99'); botaiP99 = (Avg $sv 'bP99'); frameMaxMs = (MaxOf $sv 'totalMax'); overBudget = (($sv | ForEach-Object { $_.over } | Measure-Object -Sum).Sum)
    gameFramesPerSec = if ($sv.Count) { [math]::Round((($sv | ForEach-Object { $_.gameframes } | Measure-Object -Sum).Sum) / ($sv.Count * $Window), 1) } else { $null }
    botaiMs = (WAvg $sv 'botai'); botaiMaxMs = (MaxOf $sv 'botaiMax'); gameMs = (WAvg $sv 'game'); gameMaxMs = (MaxOf $sv 'gameMax'); snapMs = (WAvg $sv 'snap')
    aiTracesPerFrame = (WAvg $sv 'aiTraces'); aiTraceMs = (WAvg $sv 'aiTraceMs'); gameTracesPerFrame = (WAvg $sv 'gameTraces'); gameTraceMs = (WAvg $sv 'gameTraceMs')
    aiMs = (WAvg $bp 'ai'); aiP50 = (Avg $pct 'p50'); aiP95 = (Avg $pct 'p95'); aiP99 = (Avg $pct 'p99'); aiMaxMs = (MaxOf $bp 'aiMax')
    thinkUs = (WAvg $bp 'think'); thinkMaxUs = (MaxOf $bp 'thinkMax'); thinksPerFrame = (Avg $pct 'tpf'); aiKCycles = (Avg $kc 'kc'); aiKCyclesPerBot = (Avg $kc 'kcBot'); usPerBotFrame = (Avg $pct 'perBot')
    sched = $sch; schedWhy = $why
    eval = $ev; zones = $zones
    gameP99 = (Avg $sv 'gP99'); snapP99 = (Avg $sv 'sP99')
    gfMs = (Avg $gp 'avg'); gfP99 = (Avg $gp 'p99'); gfMax = (MaxOf $gp 'max'); entities = (MaxOf $gp 'ents')
    gameZones = (ZoneSummary $gz); parts = (ZoneSummary $pz)
}
$json = $res | ConvertTo-Json -Compress -Depth 5
Add-Content -Path $Results -Value $json
"{0}: bots {1} frame {2} ms (p99 {11}, max {3}) ai kcycles {12} ({13}/bot) botai {4} ms game {5} ms | ai {6} ms p99 {7} | think {8} us, {9} us/bot/frame | cpu {10}%" -f `
    $Tag, $res.bots, $res.frameMs, $res.frameMaxMs, $res.botaiMs, $res.gameMs, $res.aiMs, $res.aiP99, $res.thinkUs, $res.usPerBotFrame, $res.cpuCorePct, $res.frameP99, $res.aiKCycles, $res.aiKCyclesPerBot
