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

function Read-SuiteConfig([string]$Path) {
    $settings = @{}
    $profiles = [System.Collections.Generic.List[object]]::new()

    foreach ($rawLine in Get-Content -LiteralPath $Path) {
        $line = $rawLine.Trim()
        if (-not $line -or $line.StartsWith("#")) { continue }
        $parts = $line.Split("=", 2)
        if ($parts.Count -ne 2) { throw "Invalid suite configuration line: $rawLine" }
        if ($parts[0] -eq "PROFILE") {
            $profile = $parts[1] -split '\|', 4
            if ($profile.Count -lt 2) { throw "Invalid PROFILE line: $rawLine" }
            $profiles.Add([pscustomobject]@{
                Name = $profile[0]
                Task = $profile[1]
                ClientRole = if ($profile.Count -ge 3) { $profile[2] } else { "" }
                EventsBatchLimit = if ($profile.Count -ge 4) { $profile[3] } else { "" }
            })
        } else {
            $settings[$parts[0]] = $parts[1]
        }
    }

    foreach ($required in "REPETITIONS", "WARMUP", "DURATION", "WINDOW", "BATCH_TIMEOUT",
             "STARTUP_TIMEOUT", "MONITORING_PERIOD", "CLIENT_ROLE", "COOLDOWN_SECONDS", "ADDRESS",
             "LISTEN_ADDRESS") {
        if (-not $settings.ContainsKey($required)) { throw "Missing $required in $Path" }
    }
    if ($profiles.Count -eq 0) { throw "No PROFILE entries in $Path" }
    return [pscustomobject]@{ Settings = $settings; Profiles = $profiles }
}

function Get-Binary([string]$Name) {
    $path = Join-Path $BinaryDirectory "$Name.exe"
    if (-not (Test-Path -LiteralPath $path)) { $path = Join-Path $BinaryDirectory $Name }
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing benchmark binary: $path" }
    return (Resolve-Path -LiteralPath $path).Path
}

$suite = Read-SuiteConfig $Config
$serverBinary = Get-Binary "latency_server"
$clientBinary = Get-Binary "latency_client"
$analyzerBinary = Get-Binary "latency_analyzer"
$settings = $suite.Settings
$repetitions = [int]$settings.REPETITIONS
$clientRole = if ($ClientRole) { $ClientRole } else { $settings.CLIENT_ROLE }
$listenerDelay = if ($ListenerDelay) {
    $ListenerDelay
} elseif ($settings.ContainsKey("LISTENER_DELAY")) {
    $settings.LISTENER_DELAY
} else {
    "0"
}
$eventsBatchLimit = if ($EventsBatchLimit) {
    $EventsBatchLimit
} elseif ($settings.ContainsKey("EVENTS_BATCH_LIMIT")) {
    $settings.EVENTS_BATCH_LIMIT
} else {
    "optimal"
}

if ($repetitions -le 0) { throw "REPETITIONS must be positive" }

if ($DryRun) {
    for ($repetition = 1; $repetition -le $repetitions; ++$repetition) {
        for ($position = 0; $position -lt $suite.Profiles.Count; ++$position) {
            $profile = $suite.Profiles[($position + $repetition - 1) % $suite.Profiles.Count]
            $prefix = "{0}-r{1:d2}" -f $profile.Name, $repetition
            $effectiveRole = if ($profile.ClientRole) { $profile.ClientRole } else { $clientRole }
            $effectiveBatchLimit = if ($profile.EventsBatchLimit) {
                $profile.EventsBatchLimit
            } else {
                $eventsBatchLimit
            }
            Write-Output "$prefix : $($profile.Task) ; role=$effectiveRole events-batch-limit=$effectiveBatchLimit listener-delay=$listenerDelay startup-timeout=$($settings.STARTUP_TIMEOUT) warmup=$($settings.WARMUP) duration=$($settings.DURATION)"
        }
    }
    Write-Output "Analyzer: $analyzerBinary --monitoring-period $($settings.MONITORING_PERIOD)"
    exit 0
}

$timestamp = [DateTime]::UtcNow.ToString("yyyyMMdd'T'HHmmss'Z'")
$runDirectory = Join-Path $OutputRoot $timestamp
New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
$runDirectory = (Resolve-Path -LiteralPath $runDirectory).Path
Copy-Item -LiteralPath $Config -Destination (Join-Path $runDirectory "suite.conf")
$manifest = Join-Path $runDirectory "run-manifest.csv"
'profile,repetition,task,client_role,events_batch_limit,status,client_exit_code' |
    Set-Content -LiteralPath $manifest -Encoding utf8

@(
    "started_utc=$([DateTime]::UtcNow.ToString('o'))"
    "git_commit=$(git rev-parse HEAD 2>$null)"
    "os=$([System.Runtime.InteropServices.RuntimeInformation]::OSDescription)"
    "architecture=$([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture)"
    "processor_count=$([Environment]::ProcessorCount)"
    "binary_directory=$((Resolve-Path -LiteralPath $BinaryDirectory).Path)"
    "suite_config=$((Resolve-Path -LiteralPath $Config).Path)"
    "client_role=$clientRole"
    "listener_delay=$listenerDelay"
    "events_batch_limit=$eventsBatchLimit"
) | Set-Content -LiteralPath (Join-Path $runDirectory "environment.txt") -Encoding utf8

$failed = $false
for ($repetition = 1; $repetition -le $repetitions; ++$repetition) {
    for ($position = 0; $position -lt $suite.Profiles.Count; ++$position) {
        $profile = $suite.Profiles[($position + $repetition - 1) % $suite.Profiles.Count]
        $prefix = "{0}-r{1:d2}" -f $profile.Name, $repetition
        $effectiveRole = if ($profile.ClientRole) { $profile.ClientRole } else { $clientRole }
        $effectiveBatchLimit = if ($profile.EventsBatchLimit) {
            $profile.EventsBatchLimit
        } else {
            $eventsBatchLimit
        }
        $outputPrefix = Join-Path $runDirectory $prefix
        $serverOut = "$outputPrefix-server.stdout.log"
        $serverErr = "$outputPrefix-server.stderr.log"
        $serverLog = "$outputPrefix-server.log"
        $clientLog = "$outputPrefix-client.log"
        Write-Output "Starting $prefix ($($profile.Task))"
        $server = Start-Process -FilePath $serverBinary -ArgumentList @(
            "--address", $settings.LISTEN_ADDRESS, "--monitoring-stat", $settings.MONITORING_PERIOD
        ) -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr -NoNewWindow -PassThru
        $clientExit = -1
        $status = "failed"

        try {
            $ready = $false
            for ($attempt = 0; $attempt -lt 30; ++$attempt) {
                Start-Sleep -Seconds 1
                $server.Refresh()
                if ($server.HasExited) { break }
                if ((Test-Path $serverOut) -and
                    (Select-String -LiteralPath $serverOut -SimpleMatch "Latency server listening on" -Quiet)) {
                    $ready = $true
                    break
                }
            }
            if (-not $ready) { throw "Latency server did not become ready" }

            & $clientBinary --address $settings.ADDRESS --task $profile.Task `
                --role $effectiveRole `
                --listener-delay $listenerDelay `
                --events-batch-limit $effectiveBatchLimit `
                --warmup $settings.WARMUP --duration $settings.DURATION --window $settings.WINDOW `
                --batch-timeout $settings.BATCH_TIMEOUT --startup-timeout $settings.STARTUP_TIMEOUT `
                --monitoring-stat $settings.MONITORING_PERIOD `
                --output $outputPrefix *> $clientLog
            $clientExit = $LASTEXITCODE
            if ($clientExit -ne 0) { throw "Client exited with code $clientExit" }
            if (-not (Test-Path -LiteralPath "$outputPrefix-summary.csv")) { throw "Client summary was not produced" }
            for ($attempt = 0; $attempt -lt 50; ++$attempt) {
                if ((Test-Path $serverOut) -and
                    (Select-String -LiteralPath $serverOut -SimpleMatch "Generator summary" -Quiet)) { break }
                Start-Sleep -Milliseconds 100
            }
            $status = "passed"
        } catch {
            $failed = $true
            Add-Content -LiteralPath $clientLog -Value "Runner error: $($_.Exception.Message)"
            if (Test-Path -LiteralPath "$outputPrefix-summary.csv") {
                Move-Item -LiteralPath "$outputPrefix-summary.csv" -Destination "$outputPrefix-summary.partial.csv"
            }
            if (Test-Path -LiteralPath "$outputPrefix-outliers.csv") {
                Move-Item -LiteralPath "$outputPrefix-outliers.csv" -Destination "$outputPrefix-outliers.partial.csv"
            }
        } finally {
            if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
            $server.WaitForExit()
            Get-Content -LiteralPath $serverOut, $serverErr -ErrorAction SilentlyContinue |
                Set-Content -LiteralPath $serverLog -Encoding utf8
            Remove-Item -LiteralPath $serverOut, $serverErr -Force -ErrorAction SilentlyContinue
        }

        '"{0}",{1},"{2}",{3},{4},{5},{6}' -f $profile.Name, $repetition, $profile.Task, $effectiveRole,
            $effectiveBatchLimit, $status, $clientExit |
            Add-Content -LiteralPath $manifest -Encoding utf8
        if (-not ($repetition -eq $repetitions -and $position -eq $suite.Profiles.Count - 1)) {
            Start-Sleep -Seconds ([int]$settings.COOLDOWN_SECONDS)
        }
    }
}

$summaries = Get-ChildItem -LiteralPath $runDirectory -Filter "*-summary.csv"
if ($summaries.Count -gt 0) {
    & $analyzerBinary --run-directory $runDirectory --monitoring-period $settings.MONITORING_PERIOD `
        *> (Join-Path $runDirectory "analyzer.log")
    if ($LASTEXITCODE -ne 0) { $failed = $true }
} else {
    $failed = $true
    "No successful benchmark runs to analyze" | Set-Content (Join-Path $runDirectory "analyzer.log")
}

Write-Output "Benchmark results: $runDirectory"
if ($failed) { exit 1 }
