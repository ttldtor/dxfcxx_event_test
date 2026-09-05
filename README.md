# dxFeed Graal C++ latency test

This project is a two-process load-testing tool. The server publishes configurable batches of synthetic `Quote`,
`Trade`, `TradeETH`, and `Summary` events and can publish an initial `Profile` state. The client measures the time
between the server's `publishEvents` call and delivery to the C++ event listener. A third executable converts QD
monitoring logs into machine-readable CSV files and compares repeated runs.

## Native build

The build requires CMake 3.21 or newer, a C++23 compiler, and network access for the pinned dxFeed Graal C++ API and
Graal Native SDK archives.

On Linux or macOS, use a single-configuration build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DDXFCXX_BUILD_DOC=OFF -DDXFCXX_BUILD_SAMPLES=OFF -DDXFCXX_BUILD_TOOLS=OFF
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

On Windows, run the following from a Visual Studio developer shell:

```powershell
cmake -S . -B build -A x64 `
  -DDXFCXX_BUILD_DOC=OFF -DDXFCXX_BUILD_SAMPLES=OFF -DDXFCXX_BUILD_TOOLS=OFF
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

The executables are under `build/` with a single-configuration generator and under `build/Release/` with Visual
Studio. Run any executable with `--help` to see all options.

### Runtime properties and code style

The default [`dxfeed.system.properties`](dxfeed.system.properties) file is loaded by the dxFeed Graal C++ API when
the isolate is initialized. It enables `dxscheme.nanoTime=true` for all dxFeed entities in the process. Docker
images set `DXFEED_dxfeed.system.properties` to the installed copy explicitly; for a native run, keep the file in
the process working directory or set the same environment variable to a different properties file. Options that
are expected to vary per run, such as `monitoring.stat`, are still applied programmatically before the first
endpoint is created and therefore also have process-wide scope.

First-party code targets C++23. clang-format 20.1.8 is the primary formatter; Uncrustify 0.83.0 adds the
statement-level blank-line rules from [`CONTRIBUTING.md`](CONTRIBUTING.md) that clang-format cannot express. After
configuring the project, equivalent cross-platform formatting commands are available as CMake targets:

```sh
cmake --build build --target format
cmake --build build --target check-format
```

In CLion, enable clang-format for the project and run the `format` target when statement-level spacing also needs to
be applied. Both tools must match the versions above so local output remains identical to CI.

Tests use the header-only doctest framework, fetched at its pinned release by CMake.

## Executables

`latency_server` is a QD publisher. It listens on `--address` (default `:7400`) and waits for a client to subscribe to
a task string. It supports one active task at a time and waits until the publisher observes the complete requested
symbol set for every market event type before publishing the initial Profiles and starting the recurring load. It
creates stable synthetic event objects for that task and publishes combined event/marker batches at the configured
cadence until the subscription is removed. The marker timestamp is captured immediately before `publishEvents()`;
server logs separately report preparation time,
publisher call time, achieved rate, and missed publication deadlines. `--monitoring-stat` controls the QD statistics
period and accepts `0` to disable it. For portable sub-20-ms scheduling, the generator yields during the final 20 ms
before a deadline; high-frequency profiles can therefore consume one CPU core on the publisher.

`latency_client` requests the task, discards the warm-up interval, and records latency during the measurement
interval. Its main options are:

| Option | Default | Purpose |
|---|---:|---|
| `--address` | `127.0.0.1:7400` | Server endpoint. |
| `--task` | `SUB:Q100` | Synthetic event quantities, cadence, and optional subscribed universe. |
| `--warmup` | `30s` | Data collection period excluded from reports. |
| `--duration` | `5m` | Reported measurement period. |
| `--window` | `10s` | Size of each summary row's time window. |
| `--batch-timeout` | `30s` | Maximum wait for an incomplete marker/event batch. |
| `--startup-timeout` | `30s` | Maximum wait for all unique initial Profile symbols before warm-up. |
| `--listener-delay` | `0` | Artificial delay at the start of each market-event callback. |
| `--events-batch-limit` | `optimal` | Maximum market events per native notification: `optimal`, `maximum`, or a positive integer. |
| `--monitoring-stat` | `10s` | QD statistics period; `0` disables it. |
| `--role` | `stream-feed` | Endpoint role: `stream-feed` preserves updates; `feed` permits conflation. |
| `--output` | `latency` | Path and filename prefix for generated CSV files. |

The task DSL is `SUB:<type><quantity>[;...][@<period>][#<symbols>][~<shuffle-seed>]`: `Q`, `T`, `E`, and `S` mean
Quote, Trade, TradeETH, and Summary. Each type may occur once and quantities must be positive. All configured types
are recurring and contribute to the nominal event rate. The default period is `1s`. The optional final symbol count
expands the subscribed instrument universe without changing the number of events published in each batch. It cannot
be smaller than any configured event quantity. A final shuffle seed enables reproducible per-publication shuffling
of the configured event-type blocks; events inside each block retain their order.

For example, `SUB:Q375;T375;E375;S375@10ms` publishes 1,500 recurring events every 10 ms (150,000 events/s).
The common instrument universe contains 375 symbols named `SYM000` through `SYM374`; the numeric width is derived
once from the largest configured quantity. A type with a smaller quantity uses a prefix of the same universe. The
client automatically includes `Profile` in its single combined market-event subscription. The test server waits for
that subscription and every requested recurring event subscription to contain the complete common universe, then
publishes one initial Profile for every symbol. The client starts its warm-up only after all unique Profile symbols
have arrived, so subscription propagation is excluded from the configured warm-up duration.

For example, `SUB:Q375;T375;E375;S375@10ms#3750` still publishes 1,500 events every 10 ms, but subscribes to
`SYM0000` through `SYM3749` and publishes 3,750 initial Profiles. Every recurring publication advances each event
type by 375 symbols through this universe. This keeps throughput constant while varying subscription cardinality.

`latency_analyzer` is a standalone post-processing utility and does not connect to dxFeed. It reads a directory of
latency summaries and captured QD logs, then writes `monitoring.csv` and `monitoring-summary.csv`. Pass
`--run-directory` and the same `--monitoring-period` that was used by server and client. Durations accepted by all
tools use `ms`, `s`, `m`, or `h` suffixes.

## Running a benchmark

Start the server:

```sh
./build/latency_server --address :7400
```

Then start the client in another terminal:

```sh
./build/latency_client --address 127.0.0.1:7400 --task "SUB:Q1000;T1000;E1000;S1000"
```

Append `.exe` and use `build/Release/` for a Visual Studio build. By default, the client performs a 30-second warm-up
followed by a five-minute measurement divided into 10-second windows. It writes `<prefix>-summary.csv`,
`<prefix>-callbacks.csv`, and `<prefix>-outliers.csv`; use `--output <prefix>` to choose their location and basename.
The callback report contains per-window and whole-run distributions for the number of events in each market-event
notification and the time spent in its user callback.

Both processes default `monitoring.stat` to `10s`, making QD print internal endpoint statistics every 10 seconds.
Pass `--monitoring-stat 0` to disable the reports or provide another positive duration. Redirect stdout and stderr
when the logs will be analyzed later:

```sh
./build/latency_server --address :7400 > run/q1k-server.log 2>&1
./build/latency_client --address 127.0.0.1:7400 --task "SUB:Q1000;S1000;T1000" \
  --output run/q1k > run/q1k-client.log 2>&1
./build/latency_analyzer --run-directory run --monitoring-period 10s
```

PowerShell uses the same executable and options. `latency_analyzer` looks for matching `<profile>-summary.csv`,
`<profile>-server.log`, and `<profile>-client.log` files. It writes every parsed interval to `monitoring.csv` and
profile/process aggregates for intervals wholly inside the measurement phase to `monitoring-summary.csv`. QD log
timestamps have no UTC offset and are interpreted in the analyzer process's local time zone; analyze moved logs with
the same `TZ` setting as the machine that produced them.

## Repeated local benchmark suite

The repository includes native launchers for a longer cadence comparison suite. They run three repetitions of four
mixed profiles in `FEED` mode, all nominally producing 150,000 recurring events/s: 150,000 events every second,
15,000 every 100 ms, 1,500 every 10 ms, and 150 every 1 ms. Each profile also sends one initial `Profile` per
instrument. The 1 ms profile is a scheduler/publisher stress case and should be interpreted separately. Every run
uses a one-minute warm-up, a ten-minute measurement, ten-second windows, and a fresh server/client pair. Profile
order rotates between repetitions and a 30-second cool-down separates runs.

Build the Release binaries first, then run the launcher for the host operating system. On Windows:

```powershell
.\tools\run-benchmark.ps1 -BinaryDirectory .\build\Release
```

On Linux or macOS:

```sh
bash ./tools/run-benchmark.sh --binary-directory ./build
```

Use `-DryRun` or `--dry-run` to validate the suite and display all planned commands without starting a benchmark.
Both launchers read `tools/benchmark-suite.conf`, including the independent `STARTUP_TIMEOUT`; pass `-Config` or
`--config` to use a modified suite. Results are
written below `benchmark-results/<UTC timestamp>/`. A full default run takes approximately two hours and twenty
minutes plus any machine-dependent startup overhead.

A `PROFILE` line may override the endpoint role and events batch limit for that profile using
`PROFILE=name|task|client-role|events-batch-limit`. Omitted fields inherit `CLIENT_ROLE` and `EVENTS_BATCH_LIMIT`
from the suite; the latter defaults to `optimal`. Command-line `-EventsBatchLimit`/`--events-batch-limit` provides a
suite-wide override for profiles that do not specify one.

For a short contract A/B, run `tools/conflation-diagnostic.conf` once with the default `feed` role and once with a
`stream-feed` override. The task, symbol set, cadence, warm-up, and measurement duration remain identical:

```powershell
.\tools\run-benchmark.ps1 -BinaryDirectory .\build\Release `
    -Config .\tools\conflation-diagnostic.conf -ClientRole feed
.\tools\run-benchmark.ps1 -BinaryDirectory .\build\Release `
    -Config .\tools\conflation-diagnostic.conf -ClientRole stream-feed
```

```sh
bash ./tools/run-benchmark.sh --binary-directory ./build \
    --config ./tools/conflation-diagnostic.conf --client-role feed
bash ./tools/run-benchmark.sh --binary-directory ./build \
    --config ./tools/conflation-diagnostic.conf --client-role stream-feed
```

To test whether client-side listener speed contributes to FEED supersession, repeat the FEED diagnostic with a
controlled delay before every market-event callback. `0` disables the delay:

```powershell
.\tools\run-benchmark.ps1 -BinaryDirectory .\build\Release `
    -Config .\tools\conflation-diagnostic.conf -ClientRole feed -ListenerDelay 1ms
```

```sh
bash ./tools/run-benchmark.sh --binary-directory ./build \
    --config ./tools/conflation-diagnostic.conf --client-role feed --listener-delay 1ms
```

`tools/symbol-cardinality.conf` compares 375, 3,750, and 10,000 subscribed symbols while keeping the recurring
workload at 150,000 events/s:

```powershell
.\tools\run-benchmark.ps1 -BinaryDirectory .\build\Release `
    -Config .\tools\symbol-cardinality.conf
```

```sh
bash ./tools/run-benchmark.sh --binary-directory ./build \
    --config ./tools/symbol-cardinality.conf
```

`tools/event-order.conf` keeps the 375-symbol, 150,000-events/s workload fixed and places a different event type at
the end of each publication. The server preserves the event-type order written in the task DSL. This isolates an
event-class effect from a serialization-position effect:

```powershell
.\tools\run-benchmark.ps1 -BinaryDirectory .\build\Release `
    -Config .\tools\event-order.conf
```

```sh
bash ./tools/run-benchmark.sh --binary-directory ./build \
    --config ./tools/event-order.conf
```

After the fixed-order comparison, `tools/event-order-shuffle.conf` uses seed `22805` to reshuffle the four event-type
blocks for every publication. The operation shuffles four indices, not all 1,500 events, so its cost is negligible
and included in the reported server preparation time:

```powershell
.\tools\run-benchmark.ps1 -BinaryDirectory .\build\Release `
    -Config .\tools\event-order-shuffle.conf
```

```sh
bash ./tools/run-benchmark.sh --binary-directory ./build \
    --config ./tools/event-order-shuffle.conf
```

`tools/events-batch-limit.conf` compares native notification limits while holding the shuffled 375-symbol workload
at 150,000 events/s. It includes `optimal`, `1`, `375`, `1500`, and `maximum` FEED profiles plus a STREAM_FEED
control. Limit `1` is intentionally a callback-overhead stress case:

```powershell
.\tools\run-benchmark.ps1 -BinaryDirectory .\build\Release `
    -Config .\tools\events-batch-limit.conf
```

```sh
bash ./tools/run-benchmark.sh --binary-directory ./build \
    --config ./tools/events-batch-limit.conf
```

Each output prefix includes its repetition, for example `150k-100ms-r02`. The analyzer additionally writes
`latency-runs.csv`, `latency-comparison.csv`, `monitoring-comparison.csv`, and a concise `REPORT.md`. Comparison CSVs
contain the minimum, median, and maximum of run-level values; original summaries and logs remain available for more
detailed analysis. A failed run is recorded in `run-manifest.csv`, its partial CSV files are preserved with a
`.partial.csv` suffix, and the remaining profiles still run.

The client retains exact latency values to calculate whole-run percentiles. Each cadence profile records up to 90
million event samples over ten minutes and can temporarily require several gigabytes of memory while final totals
are copied and sorted. Run the suite on an otherwise idle machine with sufficient RAM. The launchers are intended
for local native measurements; GitHub Actions only performs their dry-run validation because hosted-runner latency
is not treated as benchmark data.

## Docker

The Linux and Windows images each contain `latency_server`, `latency_client`, and `latency_analyzer`. There is no fixed
entrypoint: put the desired executable immediately after the image name. Compose is not required.

### Linux containers

Build for the current Linux engine and CPU architecture:

```sh
docker --context linux-engine build -t dxfcxx-latency:linux -f Dockerfile .
```

Build one architecture explicitly with buildx:

```sh
docker --context linux-engine buildx build --load --platform linux/amd64 \
  -t dxfcxx-latency:linux-amd64 -f Dockerfile .
docker --context linux-engine buildx build --load --platform linux/arm64 \
  -t dxfcxx-latency:linux-arm64 -f Dockerfile .
```

Use `linux/arm64` on an Apple Silicon Colima engine to build and run natively. A multi-platform manifest requires an
image registry; replace `--load` with `--push`, specify both platforms, and use a registry-qualified tag.

### Windows containers

The Windows image targets Windows Server Core LTSC 2025 and `amd64`. Its builder installs Visual Studio Build Tools,
so the first build is large and may take considerable time and disk space:

```powershell
docker --context win-engine build --memory 4g --isolation hyperv `
  -t dxfcxx-latency:windows -f Dockerfile.windows .
```

The Windows container version must be compatible with the Windows host. Use Hyper-V isolation when the host cannot
run the LTSC 2025 image with process isolation.

### Running two containers

Create a private network and a host directory for results. On Linux or macOS:

```sh
docker network create latency-test
mkdir -p benchmark-results/docker-run

docker run -d --rm --name latency-server --network latency-test \
  dxfcxx-latency:linux latency_server --address :7400 --monitoring-stat 10s

docker run --rm --network latency-test \
  --mount type=bind,source="$(pwd)/benchmark-results/docker-run",target=/work \
  dxfcxx-latency:linux latency_client --address latency-server:7400 \
  --task "SUB:Q1000;S1000;T1000" --output q1k --monitoring-stat 10s \
  > benchmark-results/docker-run/q1k-client.log 2>&1

docker logs latency-server > benchmark-results/docker-run/q1k-server.log 2>&1
docker stop latency-server
docker run --rm --mount type=bind,source="$(pwd)/benchmark-results/docker-run",target=/work \
  dxfcxx-latency:linux latency_analyzer --run-directory /work --monitoring-period 10s
```

For a Windows container, use Windows paths and executable names:

```powershell
$resultDir = (New-Item -ItemType Directory -Force benchmark-results\docker-run).FullName
docker --context win-engine network create --driver nat latency-test
docker --context win-engine run -d --rm --name latency-server --network latency-test `
  dxfcxx-latency:windows latency_server.exe --address :7400 --monitoring-stat 10s
docker --context win-engine run --rm --network latency-test `
  --mount "type=bind,source=$resultDir,target=C:\work" `
  dxfcxx-latency:windows latency_client.exe --address latency-server:7400 `
  --task "SUB:Q1000;S1000;T1000" --output q1k --monitoring-stat 10s `
  *> "$resultDir\q1k-client.log"
docker --context win-engine logs latency-server *> "$resultDir\q1k-server.log"
docker --context win-engine stop latency-server
docker --context win-engine run --rm --mount "type=bind,source=$resultDir,target=C:\work" `
  dxfcxx-latency:windows latency_analyzer.exe --run-directory C:\work --monitoring-period 10s
```

Use the selected Docker context on every command if it is not current. Container bridge networking, CPU quotas,
emulation, and bind mounts can affect latency. Record the image architecture, Docker context, network mode, and
resource limits with benchmark results; use native processes when measuring the lowest host-level latency.

## Timestamping and correlation

Both processes set `dxscheme.nanoTime=true` before creating their endpoints. The server fills the nano-time and
sequence fields of event types that support them. However, `eventTime` is not transmitted through the network QTP
connection, and `Quote` loses its sequence and fractional seconds with the tested scheme. An accompanying
`TextMessage` with the payload `LATENCY_BATCH:<unix_ns>` therefore carries the exact publish timestamp in the same
batch.

`Trade` events are correlated with the marker by sequence, `Summary` events by a synthetic `dayId`, and `Quote`
events by the seconds component of their exchange time. The last mapping is unambiguous at the fixed rate of one
batch per second. This is a synthetic wire contract used by the test; `Summary::dayId` does not represent a trading
date here.

The client defaults to `STREAM_FEED`, so conflation does not intentionally hide intermediate updates. The benchmark
suite selects `FEED` explicitly to reproduce normal feed semantics. Latency is stored in nanoseconds and displayed
in microseconds. A value above `Q3 + 1.5 * IQR` for the current window is classified as an outlier. `STREAM_FEED`
preserves intermediate updates, but its agent buffer is finite: QD uses `DROP_OLDEST` by default and increments the
monitoring `Dropped` counter if a consumer falls far enough behind to overflow that buffer.

The summary contains aggregate `event` and `batch` rows plus rows for each recurring event type. Their
`expected_per_batch` value is the expected sample count for that row (`1` for `batch`). The delivery-accounting
columns report published and listener-observed recurring events, `listener_coverage`, `listener_deficit`, excess
events, and full/partial/empty correlated publications. `listener_deficit` is deliberately named as an observation,
not a cause or a transport-loss counter. In `FEED` mode it may contain TICKER states superseded before listener
delivery; compare it with QD `Dropped`, buffer, lag, and publication diagnostics before attributing every deficit to
conflation. Events whose timestamp marker was not delivered are counted as
`uncorrelated_events`; they are excluded from the conditional listener coverage and cannot produce a latency sample.
Initial `Profile` delivery is reported separately in the client log and is excluded from recurring latency/rate
statistics. Event outliers are classified against the IQR threshold of their own event type and use the corresponding
type-specific sample kind in the outliers file.

## QD monitoring statistics

Periodic `{latency-server}` and `{latency-client}` records include subscription, storage, outgoing-buffer,
dropped-record, I/O-rate, data-lag, round-trip-time, and process-CPU statistics when available. I/O rates describe
the interval since the previous report. CPU is normalized to the total capacity of all logical processors, so one
fully occupied logical processor on a 32-processor machine is approximately `3.125%`.
