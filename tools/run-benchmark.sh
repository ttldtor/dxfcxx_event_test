#!/usr/bin/env bash
set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
binary_directory=""
output_root="benchmark-results"
config="$script_dir/benchmark-suite.conf"
dry_run=false

while (($#)); do
    case "$1" in
        --binary-directory) binary_directory=$2; shift 2 ;;
        --output-root) output_root=$2; shift 2 ;;
        --config) config=$2; shift 2 ;;
        --dry-run) dry_run=true; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$binary_directory" ]] || { echo "--binary-directory is required" >&2; exit 2; }

repetitions=""
warmup=""
duration=""
window=""
batch_timeout=""
monitoring_period=""
cooldown_seconds=""
address=""
listen_address=""
profile_names=()
profile_tasks=()
while IFS= read -r raw_line || [[ -n "$raw_line" ]]; do
    line="${raw_line%$'\r'}"
    [[ -z "$line" || "$line" == \#* ]] && continue
    key=${line%%=*}
    value=${line#*=}
    [[ "$key" != "$line" ]] || { echo "Invalid suite configuration line: $line" >&2; exit 2; }
    if [[ "$key" == PROFILE ]]; then
        [[ "$value" == *'|'* ]] || { echo "Invalid PROFILE line: $line" >&2; exit 2; }
        profile_names+=("${value%%|*}")
        profile_tasks+=("${value#*|}")
    else
        case "$key" in
            REPETITIONS) repetitions=$value ;;
            WARMUP) warmup=$value ;;
            DURATION) duration=$value ;;
            WINDOW) window=$value ;;
            BATCH_TIMEOUT) batch_timeout=$value ;;
            MONITORING_PERIOD) monitoring_period=$value ;;
            COOLDOWN_SECONDS) cooldown_seconds=$value ;;
            ADDRESS) address=$value ;;
            LISTEN_ADDRESS) listen_address=$value ;;
            *) echo "Unknown suite setting: $key" >&2; exit 2 ;;
        esac
    fi
done < "$config"

[[ -n "$repetitions" ]] || { echo "Missing REPETITIONS in $config" >&2; exit 2; }
[[ -n "$warmup" ]] || { echo "Missing WARMUP in $config" >&2; exit 2; }
[[ -n "$duration" ]] || { echo "Missing DURATION in $config" >&2; exit 2; }
[[ -n "$window" ]] || { echo "Missing WINDOW in $config" >&2; exit 2; }
[[ -n "$batch_timeout" ]] || { echo "Missing BATCH_TIMEOUT in $config" >&2; exit 2; }
[[ -n "$monitoring_period" ]] || { echo "Missing MONITORING_PERIOD in $config" >&2; exit 2; }
[[ -n "$cooldown_seconds" ]] || { echo "Missing COOLDOWN_SECONDS in $config" >&2; exit 2; }
[[ -n "$address" ]] || { echo "Missing ADDRESS in $config" >&2; exit 2; }
[[ -n "$listen_address" ]] || { echo "Missing LISTEN_ADDRESS in $config" >&2; exit 2; }
((${#profile_names[@]})) || { echo "No PROFILE entries in $config" >&2; exit 2; }

server_binary="$binary_directory/latency_server"
client_binary="$binary_directory/latency_client"
analyzer_binary="$binary_directory/latency_analyzer"
for binary in "$server_binary" "$client_binary" "$analyzer_binary"; do
    [[ -x "$binary" ]] || { echo "Missing benchmark binary: $binary" >&2; exit 2; }
done

[[ "$repetitions" =~ ^[1-9][0-9]*$ ]] || { echo "REPETITIONS must be positive" >&2; exit 2; }

if $dry_run; then
    for ((repetition=1; repetition<=repetitions; ++repetition)); do
        for ((position=0; position<${#profile_names[@]}; ++position)); do
            index=$(((position + repetition - 1) % ${#profile_names[@]}))
            printf '%s-r%02d : %s ; warmup=%s duration=%s\n' "${profile_names[$index]}" "$repetition" \
                "${profile_tasks[$index]}" "$warmup" "$duration"
        done
    done
    printf 'Analyzer: %s --monitoring-period %s\n' "$analyzer_binary" "$monitoring_period"
    exit 0
fi

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
run_directory="$output_root/$timestamp"
mkdir -p "$run_directory"
run_directory="$(cd "$run_directory" && pwd)"
cp "$config" "$run_directory/suite.conf"
manifest="$run_directory/run-manifest.csv"
printf '%s\n' 'profile,repetition,task,status,client_exit_code' > "$manifest"
{
    printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'git_commit=%s\n' "$(git rev-parse HEAD 2>/dev/null || true)"
    printf 'uname=%s\n' "$(uname -a)"
    printf 'processor_count=%s\n' "$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
    printf 'binary_directory=%s\n' "$(cd "$binary_directory" && pwd)"
    printf 'suite_config=%s\n' "$(cd "$(dirname "$config")" && pwd)/$(basename "$config")"
} > "$run_directory/environment.txt"

server_pid=""
cleanup_server() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    server_pid=""
}
trap cleanup_server EXIT
trap 'cleanup_server; exit 130' INT TERM

failed=false
for ((repetition=1; repetition<=repetitions; ++repetition)); do
    for ((position=0; position<${#profile_names[@]}; ++position)); do
        index=$(((position + repetition - 1) % ${#profile_names[@]}))
        profile=${profile_names[$index]}
        task=${profile_tasks[$index]}
        printf -v prefix '%s-r%02d' "$profile" "$repetition"
        output_prefix="$run_directory/$prefix"
        server_log="$output_prefix-server.log"
        client_log="$output_prefix-client.log"
        echo "Starting $prefix ($task)"
        "$server_binary" --address "$listen_address" \
            --monitoring-stat "$monitoring_period" > "$server_log" 2>&1 &
        server_pid=$!
        ready=false
        for ((attempt=0; attempt<30; ++attempt)); do
            sleep 1
            kill -0 "$server_pid" 2>/dev/null || break
            if grep -Fq "Latency server listening on" "$server_log"; then ready=true; break; fi
        done

        status=failed
        client_exit=-1
        if $ready; then
            "$client_binary" --address "$address" --task "$task" \
                --warmup "$warmup" --duration "$duration" \
                --window "$window" --batch-timeout "$batch_timeout" \
                --monitoring-stat "$monitoring_period" --output "$output_prefix" \
                > "$client_log" 2>&1
            client_exit=$?
            if ((client_exit == 0)) && [[ -f "$output_prefix-summary.csv" ]]; then
                for ((attempt=0; attempt<50; ++attempt)); do
                    grep -Fq "Generator summary" "$server_log" && break
                    sleep 0.1
                done
                status=passed
            fi
        else
            echo "Runner error: latency server did not become ready" > "$client_log"
        fi
        cleanup_server

        if [[ "$status" != passed ]]; then
            failed=true
            [[ -f "$output_prefix-summary.csv" ]] && mv "$output_prefix-summary.csv" "$output_prefix-summary.partial.csv"
            [[ -f "$output_prefix-outliers.csv" ]] && mv "$output_prefix-outliers.csv" "$output_prefix-outliers.partial.csv"
        fi
        printf '"%s",%d,"%s",%s,%d\n' "$profile" "$repetition" "$task" "$status" "$client_exit" >> "$manifest"
        if ! ((repetition == repetitions && position == ${#profile_names[@]} - 1)); then
            sleep "$cooldown_seconds"
        fi
    done
done

if compgen -G "$run_directory/*-summary.csv" >/dev/null; then
    "$analyzer_binary" --run-directory "$run_directory" --monitoring-period "$monitoring_period" \
        > "$run_directory/analyzer.log" 2>&1 || failed=true
else
    echo "No successful benchmark runs to analyze" > "$run_directory/analyzer.log"
    failed=true
fi

echo "Benchmark results: $run_directory"
$failed && exit 1
exit 0
