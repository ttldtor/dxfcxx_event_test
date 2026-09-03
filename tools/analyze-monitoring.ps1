param(
    [Parameter(Mandatory = $true)]
    [string]$RunDirectory,

    [TimeSpan]$MonitoringPeriod = [TimeSpan]::FromSeconds(10)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$INVARIANT_CULTURE = [Globalization.CultureInfo]::InvariantCulture
$NUMBER_PATTERN = '[-+]?[0-9][0-9,]*(?:\.[0-9]+)?'
$METRICS = @(
    'subscription',
    'sticky',
    'storage',
    'buffer',
    'dropped',
    'read_bps',
    'read_subscription_rps',
    'read_data_rps',
    'read_data_lag_us',
    'write_bps',
    'write_subscription_rps',
    'write_data_rps',
    'write_data_lag_us',
    'rtt_us',
    'cpu_percent'
)

function Convert-QdNumber {
    param([string]$Text)

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return $null
    }

    return [double]::Parse($Text.Replace(',', ''), $INVARIANT_CULTURE)
}

function Find-QdNumber {
    param(
        [string]$Text,
        [string]$Pattern
    )

    $match = [regex]::Match($Text, $Pattern)

    if (-not $match.Success) {
        return $null
    }

    return Convert-QdNumber $match.Groups[1].Value
}

function Read-MonitoringLog {
    param(
        [string]$Path,
        [string]$Profile,
        [string]$Process,
        [DateTimeOffset]$MeasurementStart,
        [DateTimeOffset]$MeasurementEnd,
        [double]$NominalEventsPerSecond
    )

    $result = [Collections.Generic.List[object]]::new()
    $previousEnd = $null

    foreach ($line in Get-Content -LiteralPath $Path) {
        $lineMatch = [regex]::Match(
            $line,
            '^[A-Z]\s+(?<date>[0-9]{6})\s+(?<time>[0-9]{6}\.[0-9]{3}).*?\{(?<endpoint>[^}]+)\}\s+(?<stats>Subscription:.*)$'
        )

        if (-not $lineMatch.Success) {
            continue
        }

        $localEnd = [datetime]::ParseExact(
            $lineMatch.Groups['date'].Value + ' ' + $lineMatch.Groups['time'].Value,
            'yyMMdd HHmmss.fff',
            $INVARIANT_CULTURE,
            [Globalization.DateTimeStyles]::AssumeLocal
        )
        $intervalEnd = [DateTimeOffset]$localEnd.ToUniversalTime()
        $intervalStart = if ($null -eq $previousEnd) { $intervalEnd - $MonitoringPeriod } else { $previousEnd }
        $previousEnd = $intervalEnd
        $stats = $lineMatch.Groups['stats'].Value
        $read = [regex]::Match($stats, "(?:^|; )Read: ($NUMBER_PATTERN) Bps(?: \(([^)]*)\))?")
        $write = [regex]::Match($stats, "(?:^|; )Write: ($NUMBER_PATTERN) Bps(?: \(([^)]*)\))?")
        $readDetails = if ($read.Success) { $read.Groups[2].Value } else { '' }
        $writeDetails = if ($write.Success) { $write.Groups[2].Value } else { '' }

        $result.Add([pscustomobject][ordered]@{
            profile = $Profile
            process = $Process
            endpoint = $lineMatch.Groups['endpoint'].Value
            interval_start_utc = $intervalStart.ToString('o', $INVARIANT_CULTURE)
            interval_end_utc = $intervalEnd.ToString('o', $INVARIANT_CULTURE)
            in_measurement = $intervalStart -ge $MeasurementStart -and $intervalEnd -le $MeasurementEnd
            nominal_events_per_second = $NominalEventsPerSecond
            subscription = Find-QdNumber $stats "Subscription: ($NUMBER_PATTERN)"
            sticky = Find-QdNumber $stats "Sticky: ($NUMBER_PATTERN)"
            storage = Find-QdNumber $stats "Storage: ($NUMBER_PATTERN)"
            buffer = Find-QdNumber $stats "Buffer: ($NUMBER_PATTERN)"
            dropped = Find-QdNumber $stats "Dropped: ($NUMBER_PATTERN)"
            read_bps = if ($read.Success) { Convert-QdNumber $read.Groups[1].Value } else { $null }
            read_subscription_rps = Find-QdNumber $readDetails "sub ($NUMBER_PATTERN) rps"
            read_data_rps = Find-QdNumber $readDetails "data ($NUMBER_PATTERN) rps"
            read_data_lag_us = Find-QdNumber $readDetails "lag ($NUMBER_PATTERN) us"
            write_bps = if ($write.Success) { Convert-QdNumber $write.Groups[1].Value } else { $null }
            write_subscription_rps = Find-QdNumber $writeDetails "sub ($NUMBER_PATTERN) rps"
            write_data_rps = Find-QdNumber $writeDetails "data ($NUMBER_PATTERN) rps"
            write_data_lag_us = Find-QdNumber $writeDetails "lag ($NUMBER_PATTERN) us"
            rtt_us = Find-QdNumber $stats "(?:^|; )rtt ($NUMBER_PATTERN) us"
            cpu_percent = Find-QdNumber $stats "CPU: ($NUMBER_PATTERN)%"
        })
    }

    return $result
}

$resolvedRunDirectory = (Resolve-Path -LiteralPath $RunDirectory).Path
$samples = [Collections.Generic.List[object]]::new()
$summaryFiles = Get-ChildItem -LiteralPath $resolvedRunDirectory -Filter '*-summary.csv' -File |
    Where-Object Name -NotLike 'monitoring-summary.csv'

foreach ($summaryFile in $summaryFiles) {
    $profile = $summaryFile.BaseName -replace '-summary$', ''
    $latencyRows = @(Import-Csv -LiteralPath $summaryFile.FullName | Where-Object sample_kind -eq 'event')

    if ($latencyRows.Count -eq 0) {
        throw "No event windows found in $($summaryFile.FullName)"
    }

    $measurementStart = [DateTimeOffset]::Parse(
        $latencyRows[0].window_start_utc,
        $INVARIANT_CULTURE,
        [Globalization.DateTimeStyles]::AssumeUniversal
    )
    $measurementEnd = [DateTimeOffset]::Parse(
        $latencyRows[-1].window_end_utc,
        $INVARIANT_CULTURE,
        [Globalization.DateTimeStyles]::AssumeUniversal
    )
    $nominalEventsPerSecond = Convert-QdNumber $latencyRows[0].expected_per_batch

    foreach ($process in @('server', 'client')) {
        $logPath = Join-Path $resolvedRunDirectory "$profile-$process.log"

        if (-not (Test-Path -LiteralPath $logPath)) {
            throw "Missing monitoring log: $logPath"
        }

        foreach ($sample in Read-MonitoringLog $logPath $profile $process $measurementStart $measurementEnd $nominalEventsPerSecond) {
            $samples.Add($sample)
        }
    }
}

if ($samples.Count -eq 0) {
    throw 'No QD monitoring records were found'
}

$samplesPath = Join-Path $resolvedRunDirectory 'monitoring.csv'
$samples | Sort-Object profile, process, interval_end_utc | Export-Csv -LiteralPath $samplesPath -NoTypeInformation -Encoding utf8
$aggregates = [Collections.Generic.List[object]]::new()

foreach ($group in $samples | Where-Object in_measurement -eq $true | Group-Object profile, process) {
    $first = $group.Group[0]

    foreach ($metric in $METRICS) {
        $values = @($group.Group | ForEach-Object { $_.$metric } | Where-Object { $null -ne $_ })

        if ($values.Count -eq 0) {
            continue
        }

        $measure = $values | Measure-Object -Minimum -Maximum -Average -Sum
        $aggregates.Add([pscustomobject][ordered]@{
            profile = $first.profile
            process = $first.process
            nominal_events_per_second = $first.nominal_events_per_second
            metric = $metric
            samples = $values.Count
            minimum = $measure.Minimum
            mean = $measure.Average
            maximum = $measure.Maximum
            sum = $measure.Sum
        })
    }
}

$summaryPath = Join-Path $resolvedRunDirectory 'monitoring-summary.csv'
$aggregates | Sort-Object profile, process, metric | Export-Csv -LiteralPath $summaryPath -NoTypeInformation -Encoding utf8
Write-Output "Wrote $samplesPath"
Write-Output "Wrote $summaryPath"
