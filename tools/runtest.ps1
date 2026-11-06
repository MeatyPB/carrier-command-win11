# Runs the game headless with a script. Usage: runtest.ps1 <name> <scale> <timeoutSec> [extra args]
# Script file: analysis\<name>\s.txt; screenshots and carrier.log land next to it.
param([string]$name, [int]$scale = 2, [int]$timeout = 120, [string]$extra = '')
$root = Split-Path $PSScriptRoot -Parent
$d = Join-Path $root "analysis\$name"
$args2 = @('--headless', '--game', "$root\dist\game", '--scale', "$scale", '--script', "$d\s.txt")
if ($extra) { $args2 += $extra -split ' ' }
$x = Start-Process -FilePath "$root\dist\CarrierCommand.exe" -ArgumentList $args2 -WorkingDirectory $d -PassThru
$null = $x.WaitForExit($timeout * 1000)
if (-not $x.HasExited) { $x.Kill(); 'TIMEOUT' }
Get-Content "$d\carrier.log" -TotalCount 30
