# Runs every obs-vst3 robustness scenario against the previous and the new obs-vst3.dll, inside the official
# OBS 31.0.4 runtime (obs.dll + Qt DLLs from the OBS release), and writes results\summary.md.
param(
  [string]$ObsBin,      # folder with the official obs.dll, Qt6*.dll and obs-vst3-harness.exe
  [string]$TestFx,      # built OBSTestFX.vst3
  [string]$Variants     # "base=<dir>;new=<dir>"  (each dir: obs-vst3.dll + data\)
)
$ErrorActionPreference = 'Stop'
$results = New-Item -ItemType Directory -Force (Join-Path $PWD 'results')
$harness = Join-Path $ObsBin 'obs-vst3-harness.exe'

$scenarios = @(
  @{ n = 'basic';             e = @{} },
  @{ n = 'swap';              e = @{ TESTFX_SLOW_PROCESS_US = '8000'; TESTFX_SLOW_ACTIVE_MS = '2' } },
  @{ n = 'restart_audio';     e = @{ TESTFX_RESTART_EVERY = '10' } },
  @{ n = 'restart_in_active'; e = @{ TESTFX_RESTART_IN_ACTIVE = '1' } },
  @{ n = 'init_retry';        e = @{ TESTFX_FAIL_INIT = '2' } },
  @{ n = 'nan';               e = @{ TESTFX_NAN_EVERY = '3' } },
  @{ n = 'state';             e = @{} },
  @{ n = 'state_retry';       e = @{ TESTFX_FAIL_SETSTATE = '1' } },
  @{ n = 'state_reject';      e = @{ TESTFX_FAIL_SETSTATE = '99' } },
  @{ n = 'rescan';            e = @{} },
  @{ n = 'stress';            e = @{ TESTFX_SLOW_PROCESS_US = '7000'; TESTFX_RESTART_EVERY = '7' } },
  @{ n = 'crash_process';     e = @{ TESTFX_CRASH_PROCESS_AFTER = '150'; TESTFX_CRASH_CLASS = '1' } },
  @{ n = 'throw_process';     e = @{ TESTFX_THROW_PROCESS_AFTER = '150'; TESTFX_CRASH_CLASS = '1' } },
  @{ n = 'crash_init';        e = @{ TESTFX_CRASH_INIT = '1'; TESTFX_CRASH_CLASS = '1' } },
  @{ n = 'crash_setstate';    e = @{ TESTFX_CRASH_SETSTATE = '1'; TESTFX_CRASH_CLASS = '1' } },
  @{ n = 'crash_initdll';     e = @{ TESTFX_CRASH_INITDLL = '1' } },
  @{ n = 'shell_rescan';      e = @{ TESTFX_DISCARDABLE = '1' }; cfg = 'shell' },
  @{ n = 'shell_next_start';  e = @{ TESTFX_DISCARDABLE = '1' }; cfg = 'shell' },
  @{ n = 'shell_cached';      e = @{ TESTFX_DISCARDABLE = '1' }; cfg = 'shell' }
)
$knobs = @('TESTFX_SLOW_PROCESS_US','TESTFX_SLOW_ACTIVE_MS','TESTFX_RESTART_EVERY','TESTFX_RESTART_IN_ACTIVE',
  'TESTFX_FAIL_INIT','TESTFX_NAN_EVERY','TESTFX_FAIL_SETSTATE','TESTFX_DISCARDABLE','TESTFX_CRASH_PROCESS_AFTER',
  'TESTFX_THROW_PROCESS_AFTER','TESTFX_CRASH_INIT','TESTFX_CRASH_SETSTATE','TESTFX_CRASH_INITDLL','TESTFX_CRASH_CLASS')

$summary = New-Object System.Collections.Generic.List[string]
$summary.Add('| Scenario | ' + (($Variants -split ';' | ForEach-Object { ($_ -split '=')[0] }) -join ' | ') + ' |')
$summary.Add('|---|' + (($Variants -split ';' | ForEach-Object { '---' }) -join '|') + '|')
$table = @{}
$newFailures = 0

foreach ($v in ($Variants -split ';')) {
  $name, $dir = $v -split '=', 2
  $dll = Join-Path $dir 'obs-vst3.dll'
  $data = Join-Path $dir 'data'
  $shellCfg = $null
  foreach ($s in $scenarios) {
    # every run gets its own VST3 folder (scanner path) and OBS config folder; the shell runs share one
    $testHome = Join-Path $env:RUNNER_TEMP ("h_" + $name + "_" + $s.n)
    if ($s.cfg -eq 'shell') {
      if (-not $shellCfg) { $shellCfg = Join-Path $env:RUNNER_TEMP ("h_" + $name + "_shell") }
      $testHome = $shellCfg
    }
    $vst3dir = Join-Path $testHome 'Programs\Common\VST3'
    New-Item -ItemType Directory -Force $vst3dir | Out-Null
    $fx = Join-Path $vst3dir 'OBSTestFX.vst3'
    if (-not (Test-Path $fx)) { Copy-Item $TestFx $fx }
    $cfg = Join-Path $testHome 'config'
    New-Item -ItemType Directory -Force $cfg | Out-Null

    foreach ($k in $knobs) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    foreach ($k in $s.e.Keys) { Set-Item "Env:$k" $s.e[$k] }
    $env:LOCALAPPDATA = $testHome

    $out = Join-Path $results ("{0}_{1}.txt" -f $name, $s.n)
    $err = Join-Path $results ("{0}_{1}.err.txt" -f $name, $s.n)
    $p = Start-Process -FilePath $harness -ArgumentList @("`"$dll`"", "`"$data`"", "`"$fx`"", "`"$cfg`"", $s.n) `
         -WorkingDirectory $ObsBin -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $null = $p.Handle # keeps the exit code readable after the process ends
    if (-not $p.WaitForExit(240000)) { $p.Kill(); $code = 'TIMEOUT' } else { $code = $p.ExitCode }
    $text = (Get-Content $out -Raw -ErrorAction SilentlyContinue) + ''
    $failed = ([regex]::Matches($text, 'CHECK .* FAILED')).Count
    if ($code -is [int] -and $code -ne 0 -and $code -ne 1) {
      $cell = ('CRASH 0x{0:X8}' -f ($code -band 0xFFFFFFFF))
    } elseif ($code -eq 'TIMEOUT') {
      $cell = 'TIMEOUT'
    } elseif ($code -eq 0 -and $failed -eq 0) {
      $cell = 'OK'
    } else {
      $cell = "FAIL ($failed)"
    }
    if ($name -eq 'new' -and $cell -ne 'OK') { $newFailures++ }
    if (-not $table.ContainsKey($s.n)) { $table[$s.n] = @{} }
    $table[$s.n][$name] = $cell
    Write-Output ("{0,-5} {1,-18} {2}" -f $name, $s.n, $cell)
  }
}

foreach ($s in $scenarios) {
  $row = '| ' + $s.n + ' | ' + (($Variants -split ';' | ForEach-Object { $table[$s.n][($_ -split '=')[0]] }) -join ' | ') + ' |'
  $summary.Add($row)
}
$summary | Set-Content (Join-Path $results 'summary.md') -Encoding utf8
"NEW_FAILURES=$newFailures" | Set-Content (Join-Path $results 'status.txt') -Encoding ascii
$summary | ForEach-Object { Write-Output $_ }
