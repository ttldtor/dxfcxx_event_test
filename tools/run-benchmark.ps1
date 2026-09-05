param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryDirectory,
    [string]$OutputRoot = "benchmark-results",
    [string]$Config = (Join-Path $PSScriptRoot "benchmark-suite.conf"),
    [ValidateSet("", "feed", "stream-feed")]
    [string]$ClientRole = "",
    [string]$ListenerDelay = "",
    [string]$EventsBatchLimit = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$runner = Join-Path $BinaryDirectory "latency_runner.exe"

if (-not (Test-Path -LiteralPath $runner)) {
    $runner = Join-Path $BinaryDirectory "latency_runner"
}

if (-not (Test-Path -LiteralPath $runner)) {
    throw "Missing benchmark runner: $runner"
}

$runnerArguments = @(
    "--binary-directory", $BinaryDirectory,
    "--output-root", $OutputRoot,
    "--config", $Config
)

if ($ClientRole) {
    $runnerArguments += "--client-role", $ClientRole
}

if ($ListenerDelay) {
    $runnerArguments += "--listener-delay", $ListenerDelay
}

if ($EventsBatchLimit) {
    $runnerArguments += "--events-batch-limit", $EventsBatchLimit
}

if ($DryRun) {
    $runnerArguments += "--dry-run"
}

& $runner @runnerArguments
exit $LASTEXITCODE
