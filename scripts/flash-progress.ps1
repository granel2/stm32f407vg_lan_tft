<#
.SYNOPSIS
  Runs the same openocd mass-erase/write/verify/reset sequence as before,
  wrapped with a text progress bar - openocd itself only prints a one-line
  summary at the very end ("wrote N bytes ... in X s"), so without this the
  terminal just sits silent for the ~20 s the whole sequence takes.

  The bar is time-based (elapsed vs $EstimatedSeconds), not a byte count -
  openocd's `flash write_image`/`verify_image` don't expose live progress on
  this build, so an actual bytes-transferred bar isn't available without
  patching openocd itself. Capped at 99% until the process actually exits,
  so a slower-than-usual run doesn't show a misleading "100%" while still
  working.

  $EstimatedSeconds default (20 s) is measured wall-clock time for this
  project's image size (~175 KB) on this hardware: mass-erase + write
  (~1.9 s reported) + verify (~1.5 s reported) + SWD connect/speed-
  negotiation overhead not covered by openocd's own per-step timings.
  Re-measure (`Measure-Command`) and adjust here if the image grows a lot.

.NOTES
  Real openocd stdout/stderr is captured and printed in full after the bar
  finishes, exactly as it would have streamed before this wrapper existed -
  errors are never hidden behind the progress bar, only delayed until the
  process (successfully or not) exits.
#>
param(
  [string]$Bin = "build/stm32f407vg_lan_tft.bin",
  [double]$EstimatedSeconds = 20.0
)

$ErrorActionPreference = 'Stop'
$barWidth = 30

$openocdArgs = @(
  '-f', 'interface/stlink.cfg',
  '-f', 'target/stm32f4x.cfg',
  '-c', 'init',
  '-c', 'reset halt',
  '-c', 'stm32f4x mass_erase 0',
  '-c', "flash write_image $Bin 0x08000000",
  '-c', "verify_image $Bin 0x08000000",
  '-c', 'reset run',
  '-c', 'exit'
)

# Start-Process -ArgumentList joins the array into one string and only
# quotes elements it *guesses* need it - unreliably, in Windows PowerShell
# 5.1 it drops the quotes around "reset halt" etc. entirely, so openocd
# sees "reset" and "halt" as two separate -c commands and rejects the
# second. ProcessStartInfo.ArgumentList (which sidesteps this by passing
# each element as its own argv entry) isn't available on the .NET Framework
# build Windows PowerShell 5.1 runs on, so instead build the classic
# .Arguments string ourselves, quoting exactly the elements that need it -
# CreateProcess (what Process.Start uses under the hood) then splits it
# back into the original arguments the same way a normal command line would.
function ConvertTo-ArgumentString([string[]]$ArgsIn)
{
  ($ArgsIn | ForEach-Object {
    if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
  }) -join ' '
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = 'openocd'
$psi.Arguments = ConvertTo-ArgumentString $openocdArgs
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false

$proc = [System.Diagnostics.Process]::Start($psi)
$stdoutTask = $proc.StandardOutput.ReadToEndAsync()
$stderrTask = $proc.StandardError.ReadToEndAsync()

$sw = [System.Diagnostics.Stopwatch]::StartNew()
while (-not $proc.HasExited) {
  $pct = [Math]::Min(99, [Math]::Floor(($sw.Elapsed.TotalSeconds / $EstimatedSeconds) * 100))
  $filled = [Math]::Floor($barWidth * $pct / 100)
  $bar = ('#' * $filled) + ('.' * ($barWidth - $filled))
  Write-Host -NoNewline ("`rFlashing  [{0}] {1,3}%" -f $bar, $pct)
  Start-Sleep -Milliseconds 200
}
$proc.WaitForExit()

$ok = ($proc.ExitCode -eq 0)
$finalBar = if ($ok) { '#' * $barWidth } else { ('#' * [Math]::Floor($barWidth * 0.3)) + ('.' * ($barWidth - [Math]::Floor($barWidth * 0.3))) }
$finalLabel = if ($ok) { '100% done' } else { 'FAILED    ' }
Write-Host ("`rFlashing  [{0}] {1}" -f $finalBar, $finalLabel)
Write-Host ""

$stdoutTask.Result
$stderrTask.Result

exit $proc.ExitCode
