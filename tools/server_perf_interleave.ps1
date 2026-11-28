# Interleaved server benchmark: ONE dedicated server, the settings to compare switched at run time in
# turn (A B C A B C ...), each phase PhaseSec long, profile windows of WindowSec. Windows that overlap a
# switch are dropped; the others are averaged per setting by tools/server_perf_interleave.py. Machine
# load that drifts during a run hits every setting alike, which separate back-to-back runs cannot
# guarantee on a busy machine.
#   server_perf_interleave.ps1 -DevDir <dir> -Map naboo_theed -Bots 32 -SvFps 40 -Configs "base=set com_jobs 0;set bot_mt 0","mt=set com_jobs 3;set bot_mt 15" -Cycles 6
# Config values are console commands separated by ';' (no '+'), so the command line stays short (the
# engine accepts at most 32 '+' commands). Results: one JSON line per setting in -Results.
param(
    [Parameter(Mandatory = $true)][string]$DevDir,
    [string]$Map = "naboo_theed",
    [int]$Bots = 32,
    [int]$SvFps = 40,
    [string[]]$Configs = @(),
    [int]$Cycles = 6,
    [int]$PhaseSec = 20,
    [int]$WindowSec = 5,
    [int]$Warmup = 30,
    [string]$Common = "",
    [string]$Tag = "il",
    [int]$Port = 29250,
    [string]$Seed = "",
    [string]$Results = "",
    [string]$TestBase = "E:\jedi academy modding archive\Jedi Academy TaystJK_AI test\GameData",
    [string]$SteamBase = "B:\SteamLibrary\steamapps\common\Jedi Academy\GameData"
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "bot_names.ps1")
$base = if ($Map -like "mp/*") { $TestBase } else { $SteamBase }
$modDir = Join-Path $DevDir "nicjk"
if (-not $Results) { $Results = Join-Path $DevDir "perf_interleave.jsonl" }
if ($Seed) { Copy-Item (Join-Path $Seed "*.dat") $modDir -Force }
Remove-Item (Join-Path $modDir "nicjk_server.cfg") -Force -ErrorAction SilentlyContinue
# "wait" counts server command-buffer frames, about sv_fps per second on a dedicated server
$framesPerPhase = $PhaseSec * $SvFps
$cfgLines = @("wait $($Warmup * $SvFps)")
for ($c = 0; $c -lt $Cycles; $c++) {
    foreach ($cfg in $Configs) {
        $name, $cmds = $cfg -split "=", 2
        $cfgLines += ($cmds -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        $cfgLines += "echo ILCFG $name"
        $cfgLines += "wait $framesPerPhase"
    }
}
$cfgLines += "echo ILCFG end"
$cfgLines += "wait 40"
$cfgLines += "quit"
($cfgLines -join "`r`n") + "`r`n" | Out-File -Encoding ascii (Join-Path $modDir "il_phases.cfg")
$lines = Get-BotLines -Bots $Bots -Cycle 0
(($lines + @("exec il_phases.cfg")) -join "`r`n") + "`r`n" | Out-File -Encoding ascii (Join-Path $modDir "il_bots.cfg")
$qlog = Join-Path $modDir "qconsole.log"
if (Test-Path $qlog) { Remove-Item $qlog -Force }
$argList = "+set fs_basepath `"$base`" +set fs_homepath `"$DevDir`" +set fs_game nicjk +set dedicated 1 +set net_port $Port +set com_hunkmegs 2048 +exec bench.cfg " +
    "+set sv_fps $SvFps +set sv_profile $WindowSec +set bot_profile $WindowSec +set g_profile $WindowSec +set bot_profileEval 0 $Common +set logfile 1 +map $Map +exec il_bots.cfg"
$cmd = Start-Process -FilePath "cmd.exe" -ArgumentList "/c `"`"$DevDir\nicjkded.x86_64.exe`" $argList > NUL 2>&1`"" -WorkingDirectory $DevDir -PassThru -WindowStyle Hidden
$p = $null
for ($i = 0; $i -lt 50 -and -not $p; $i++) {
    Start-Sleep -Milliseconds 200
    $child = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($cmd.Id)" | Where-Object { $_.Name -like "nicjkded*" } | Select-Object -First 1
    if ($child) { $p = Get-Process -Id $child.ProcessId }
}
if (-not $p) { throw "server process did not start" }
try { $p.PriorityClass = "High" } catch {}
$limit = (Get-Date).AddSeconds(300 + $Warmup * 2 + $Cycles * $Configs.Count * $PhaseSec * 2)
while (-not $p.HasExited -and (Get-Date) -lt $limit) { Start-Sleep 5; $p.Refresh() }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "$Tag : timed out" }
$out = Join-Path $DevDir "il_$Tag.qconsole.log"
Copy-Item $qlog $out -Force
& python (Join-Path $PSScriptRoot "server_perf_interleave.py") $out $Tag $Map $Bots $SvFps $WindowSec $Results
