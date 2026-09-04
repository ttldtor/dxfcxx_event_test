# dxFeed Graal C++ latency test

This project is a two-process load-testing tool. The server publishes one batch of synthetic `Quote`, `Trade`, and
`Summary` events per second. The client measures the time between the server's `publishEvents` call and delivery to
the C++ event listener. A third executable converts QD monitoring logs into machine-readable CSV files.

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

## Executables

`latency_server` is a QD publisher. It listens on `--address` (default `:7400`) and waits for a client to subscribe to
a task string. It supports one active task at a time, creates stable synthetic event objects for that task, and
publishes one combined event/marker batch per second until the subscription is removed. `--monitoring-stat` controls
the QD statistics period and accepts `0` to disable it.

`latency_client` opens a `STREAM_FEED` connection, requests the task, discards the warm-up interval, and records
latency during the measurement interval. Its main options are:

| Option | Default | Purpose |
|---|---:|---|
| `--address` | `127.0.0.1:7400` | Server endpoint. |
| `--task` | `SUB:Q100` | Synthetic event types and instrument counts. |
| `--warmup` | `30s` | Data collection period excluded from reports. |
| `--duration` | `5m` | Reported measurement period. |
| `--window` | `10s` | Size of each summary row's time window. |
| `--batch-timeout` | `30s` | Maximum wait for an incomplete marker/event batch. |
| `--monitoring-stat` | `10s` | QD statistics period; `0` disables it. |
| `--output` | `latency` | Path and filename prefix for generated CSV files. |

The task DSL is `SUB:<type><quantity>[;...]`, where `Q`, `T`, and `S` mean Quote, Trade, and Summary. Each type may
occur once and quantities must be positive. For example, `SUB:Q1000;S1000;T1000` publishes 3,000 events per batch;
`Q100` creates symbols `Q00` through `Q99`.

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
./build/latency_client --address 127.0.0.1:7400 --task "SUB:Q1000;S1000;T1000"
```

Append `.exe` and use `build/Release/` for a Visual Studio build. By default, the client performs a 30-second warm-up
followed by a five-minute measurement divided into 10-second windows. It writes `<prefix>-summary.csv` and
`<prefix>-outliers.csv`; use `--output <prefix>` to choose their location and basename.

Both processes set `monitoring.stat=10s`, making QD print internal endpoint statistics every 10 seconds. Pass
`--monitoring-stat 0` to disable the reports or provide another positive duration. Redirect stdout and stderr when
the logs will be analyzed later:

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

The client uses `STREAM_FEED` so conflation does not hide delayed events. Latency is stored in nanoseconds and
displayed in microseconds. A value above `Q3 + 1.5 * IQR` for the current window is classified as an outlier.
`STREAM_FEED` preserves intermediate updates, but its agent buffer is finite: QD uses `DROP_OLDEST` by default and
increments the monitoring `Dropped` counter if a consumer falls far enough behind to overflow that buffer.

The summary contains aggregate `event` and `batch` rows plus `event-quote`, `event-trade`, and `event-summary` rows.
Their `expected_per_batch` value is the expected sample count for that row (`1` for `batch`). Event outliers are
classified against the IQR threshold of their own event type and use the corresponding type-specific sample kind in
the outliers file.

## QD monitoring statistics

Periodic `{latency-server}` and `{latency-client}` records include subscription, storage, outgoing-buffer,
dropped-record, I/O-rate, data-lag, round-trip-time, and process-CPU statistics when available. I/O rates describe
the interval since the previous report. CPU is normalized to the total capacity of all logical processors, so one
fully occupied logical processor on a 32-processor machine is approximately `3.125%`.
