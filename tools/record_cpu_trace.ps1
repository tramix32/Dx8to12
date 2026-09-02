# Records a CPU sampling trace while the game runs, for "what is the CPU
# actually doing" questions that a frame-time log cannot answer.
#
# Run AS ADMINISTRATOR -- WPR refuses to record otherwise. Start the game
# first, get to the spot you want measured, then run this and keep playing;
# it stops on its own.
#
#   powershell -ExecutionPolicy Bypass -File tools\record_cpu_trace.ps1
#   powershell -ExecutionPolicy Bypass -File tools\record_cpu_trace.ps1 -Seconds 40 -Label dlss

param(
    [int]$Seconds = 25,
    [string]$Label = "trace"
)

# Deliberately not "Stop": in Windows PowerShell 5.1 anything a native .exe
# writes to stderr becomes a NativeCommandError, which "Stop" then treats as
# fatal. wpr writes to stderr routinely (including "nothing to cancel"), so
# this script checks $LASTEXITCODE instead, which is what actually says
# whether wpr succeeded.
$ErrorActionPreference = "Continue"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "This needs to run as Administrator -- WPR cannot record otherwise." -ForegroundColor Red
    exit 1
}

$wpr = Join-Path $env:SystemRoot "System32\wpr.exe"
$outDir = Join-Path $PSScriptRoot "..\traces"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$etl = Join-Path $outDir "$Label-$stamp.etl"

# Cancel a recording left running by an earlier interrupted attempt; WPR
# refuses to start a second one and the message does not say why. Routed
# through cmd so its "nothing to cancel" chatter stays out of PowerShell's
# error stream entirely.
cmd /c "`"$wpr`" -cancel >nul 2>&1"

Write-Host "Starting CPU trace..." -ForegroundColor Cyan
& $wpr -start CPU -filemode
if ($LASTEXITCODE -ne 0) { Write-Host "wpr -start failed." -ForegroundColor Red; exit 1 }

Write-Host "RECORDING for $Seconds seconds -- play now, in the spot you want measured." -ForegroundColor Green
for ($i = $Seconds; $i -gt 0; $i--) {
    Write-Host -NoNewline "`r  $i  "
    Start-Sleep -Seconds 1
}
Write-Host "`rStopping and writing the trace (this takes a moment)..." -ForegroundColor Cyan

& $wpr -stop $etl
if ($LASTEXITCODE -ne 0) { Write-Host "wpr -stop failed." -ForegroundColor Red; exit 1 }

$size = [math]::Round((Get-Item $etl).Length / 1MB, 1)
Write-Host "Wrote $etl ($size MB)" -ForegroundColor Green
