# dxFeed Graal C++ latency test

This project is a two-process load-testing tool. The server publishes configurable batches of synthetic `Quote`,
`Trade`, `TradeETH`, `Summary`, and `TimeAndSale` events and can publish initial `Profile` and bounded TimeAndSale
history. The client measures the time between the server's `publishEvents` call and delivery to the C++ event
listener. A third executable converts QD monitoring logs into machine-readable CSV files and compares repeated runs.

## Native build

The build requires CMake 3.21 or newer, a C++23 compiler, and network access for the pinned dxFeed Graal C++ API and
Graal Native SDK archives.

`LATENCY_DXFCXX_RELEASE` selects a supported release stack. It defaults to `v8.0.0`; `v5.0.0` and `v7.0.0` remain
pinned for controlled historical comparisons. Treat v8.0.0 as a separate comparison point because it contains
breaking corrections to packed event fields and native conversion and parsing behavior. It includes Graal Native
SDK 3.2.13 and QD 3.353. Use a separate build directory for every release because FetchContent selections are
cached. The compiler, selected CXX API, its Native SDK, and QD dependency versions are recorded in each benchmark
`environment.txt`.

Historical v5.0.0 builds still resolve Graal Native SDK 2.6.2 from the dxFeed JFrog repository by default. That
location is being retired, and the historical build does not declare an equivalent GitHub-hosted 2.6.2 asset.
Preserve the original SDK archive if this control must remain reproducible. The v5 CMake project accepts an alternate
archive through the `DXFEED_GRAAL_NATIVE_SDK_URL` environment variable; build directories that already populated
FetchContent are only local caches and are not a durable replacement for the artifact.

On Linux or macOS, use a single-configuration build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DDXFCXX_BUILD_DOC=OFF -DDXFCXX_BUILD_SAMPLES=OFF -DDXFCXX_BUILD_TOOLS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For example, configure the older comparison stack with:

```sh
cmake -S . -B build-v5 -DCMAKE_BUILD_TYPE=Release -DLATENCY_DXFCXX_RELEASE=v5.0.0
```

On Windows, run the following from a Visual Studio developer shell:

```powershell
cmake -S . -B build -A x64 `
  -DDXFCXX_BUILD_DOC=OFF -DDXFCXX_BUILD_SAMPLES=OFF -DDXFCXX_BUILD_TOOLS=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Quote the release definition when passing it from PowerShell:

```powershell
cmake -S . -B build-v5 -A x64 '-DLATENCY_DXFCXX_RELEASE=v5.0.0'
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
before a deadline; high-frequency profiles can therefore consume one CPU core on the publisher. `--task` queues a
task directly and is intended for clients, such as the legacy C API, that cannot use the `TextMessage` control
channel. The queued task still waits for all required subscriptions before it starts.
`--time-series-history` limits the retained TimeAndSale events per base symbol (default `1000`). When a time-series
subscription arrives, the server publishes the retained range as a HISTORY snapshot with the standard snapshot
flags, then continues publishing live updates.

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
| `--time-series-prefill` | `2s` | Time to retain live TimeAndSale events before adding the time-series symbols. |
| `--time-series-subscribe-after` | disabled | Add the TimeAndSale subscription this long after measurement starts and report ticker latency before, during, and after its HISTORY snapshot. |
| `--time-series-unsubscribe-after-snapshot` | disabled | Remove delayed TimeAndSale symbols at global snapshot completion so the before/after ticker workloads match. |
| `--listener-delay` | `0` | Artificial delay at the start of each market-event callback. |
| `--events-batch-limit` | `optimal` | Maximum market events per native notification: `optimal`, `maximum`, or a positive integer. |
| `--aggregation-period` | `0` | Per-subscription market notification aggregation period; `0` disables explicit aggregation. |
| `--monitoring-stat` | `10s` | QD statistics period; `0` disables it. |
| `--role` | `stream-feed` | Endpoint role: `stream-feed` preserves updates; `feed` permits conflation. |
| `--output` | `latency` | Path and filename prefix for generated CSV files. |

The task DSL is `SUB:<type><quantity>[;...][@<period>][#<symbols>][&<regional-sources>][~<shuffle-seed>]`: `Q`, `T`,
`E`, `S`, and `N` mean Quote, Trade, TradeETH, Summary, and TimeAndSale. Each type may occur once and quantities must
be positive. All configured types are recurring and contribute to the nominal event rate. The default period is
`1s`. The optional final symbol count expands the subscribed instrument universe without changing the number of
events published in each batch. It cannot be smaller than any configured event quantity. An optional regional-source
count from 1 through 26 activates `&A`, `&B`, ... record keys in addition to the composite keys without changing the
event count per publication. A final shuffle seed enables reproducible per-publication shuffling of the configured
event-type blocks and regional-source selection.

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

`SUB:Q375;T375;E375;S375@10ms#375&26~22805` also remains at 1,500 events per publication and 150,000 events/s.
It creates 10,125 market record keys per type (`375 × (composite + 26 regional sources)`) and deterministically
distributes each publication across them. Profiles remain one per base instrument, so this task still publishes 375
initial Profiles. The Graal client explicitly subscribes to those composite and regional symbols; the default legacy
C API client adds only the 375 base symbols because that API performs its own regional expansion.

`N` has deliberately different subscription semantics. TimeAndSale uses the base symbols through a separate
`DXFeedTimeSeriesSubscription`, requires the `FEED` endpoint role, and is excluded from marker-correlated Q/T/E/S
delivery accounting. In the default mode, the client records `fromTime`, waits for `--time-series-prefill`, subscribes,
verifies snapshot completion, and only then starts warm-up and live measurement. For example,
`SUB:Q300;T300;E300;S300;N300@10ms#300~22805` publishes 1,500 recurring events every 10 ms (150,000 events/s), while
the two-second default prefill creates approximately 200 retained TimeAndSale events per symbol before the snapshot.
The exact snapshot size can be lower at the boundary or capped by `--time-series-history`; a truncated snapshot is
reported through `SNAPSHOT_SNIP`.

With `--time-series-subscribe-after`, the client starts ticker measurement without TimeAndSale, waits the requested
delay, sets `fromTime` to the subscription time minus `--time-series-prefill`, and adds the time-series symbols while
the recurring ticker load continues. The additional `<prefix>-snapshot-overlap.csv` divides Q/T/E/S latency,
listener coverage, CPU, and RSS into `BEFORE`, `DURING`, and `AFTER` phases. The total measurement duration remains
fixed; the option must therefore be shorter than `--duration`.

With `--time-series-unsubscribe-after-snapshot`, the delayed TimeAndSale symbols are removed as soon as all requested
symbol snapshots complete. The `after` phase then returns to the same Q/T/E/S subscription and nominal 150,000
events/s used by the `before` phase. This isolates post-burst recovery from the extra 37,500 live TimeAndSale events/s
that remain subscribed in the ordinary overlap experiment. Subscription removal is asynchronous, so the beginning of
the `after` phase also includes its propagation time.

`latency_analyzer` is a standalone post-processing utility and does not connect to dxFeed. It reads a directory of
latency summaries and captured QD logs, then writes `monitoring.csv` and `monitoring-summary.csv`. Pass
`--run-directory` and the same `--monitoring-period` that was used by server and client. Durations accepted by all
tools use `ms`, `s`, `m`, or `h` suffixes.

### Delivery-only API clients

`latency_graal_delivery_client` provides a low-overhead C++ API path for comparisons where timestamp-correlated E2E
samples are not required. It keeps one combined Q/T/E/S market subscription plus a separate Profile subscription,
counts native vector callback sizes and event types, and records CPU/RSS. It intentionally omits `TextMessage`
markers, per-event timestamps, pending-batch maps, latency samples, windows, and outlier calculation. Use
`--role stream-feed` for non-conflating delivery or `--role feed` to retain normal ticker supersession semantics.

The server must receive the same task directly because this client does not open the control subscription:

```powershell
.\build\Release\latency_server.exe --address :7400 --task "SUB:Q10;T10;E10;S10@100ms"
.\build\Release\latency_graal_delivery_client.exe --address 127.0.0.1:7400 `
    --task "SUB:Q10;T10;E10;S10@100ms" --role stream-feed --duration 10s --require-events
```

The delivery-only client is a measurement control, not a replacement for `latency_client`: it reports delivery rate,
callback shape, and resources but cannot report publisher-to-listener latency.

### Customer source-time methodology control

The full Graal client also writes `<prefix>-source-time-methods.csv` for Trade and TradeETH. All three rows are
calculated online from the same listener callbacks at millisecond resolution:

- `all-trade-events` retains every observation with a positive source timestamp;
- `per-series-strict` retains an observation only when time advances for its event-kind and symbol pair;
- `customer-global-strict` retains an observation only when time advances beyond one maximum shared by every Trade
  and TradeETH symbol.

The last row reproduces the decisive selection rule in the supplied customer test. It is not an alternative E2E
clock: a global maximum across many instruments turns the result into an order-dependent sample of record timestamps.
The server sets Trade and TradeETH source time to the publication time, while the existing TextMessage marker remains
the higher-resolution, batch-correlated control.

`tools/customer-methodology.conf` runs FEED with the optimal native batch limit, FEED with a one-event limit, and a
non-conflating STREAM_FEED control against the same shuffled 150,000-events/s workload. It is intended to distinguish
API delivery behavior from observations discarded by the measurement algorithm. The suite cannot recreate a
production delay without a recorded stream or an explicitly injected fault.

Set `LATENCY_BUILD_LEGACY_CLIENT=ON` to additionally build the legacy comparison path. CMake downloads the
official pinned dxFeed C API 5.11.0 no-TLS binary SDK and exposes it through an imported target; it does not embed the
upstream source project or manually copy its source lists. This optional target is supported only on 64-bit Windows
and Linux. It is kept in a separate process so the legacy native library and the Graal Native SDK are never loaded
into the same address space.

The legacy client deliberately measures connectivity and delivery shape. It subscribes to the task's
`Quote`, `Trade`, `TradeETH`, and `Summary` types plus `Profile`, then reports callback count, recurring event count,
initial Profile count, the largest legacy `data_count`, and measurement-interval CPU/RSS from
[`ttldtor/Process`](https://github.com/ttldtor/Process). It does not report E2E latency: `Trade` and `Quote` expose
`time_nanos`, while `Summary` and `Profile` do not expose an equivalent event timestamp, and none of those fields is
the benchmark server's `publishEvents()` timestamp.

Configure and build it on Windows with:

```powershell
cmake -S . -B build-legacy -A x64 '-DLATENCY_BUILD_LEGACY_CLIENT=ON'
cmake --build build-legacy --config Release --parallel
```

Then run the server and client in separate terminals with the same task:

```powershell
.\build-legacy\Release\latency_server.exe --address :7400 --task "SUB:Q10;T10;E10;S10@100ms"
.\build-legacy\Release\latency_legacy_client.exe --address 127.0.0.1:7400 `
    --task "SUB:Q10;T10;E10;S10@100ms" --duration 10s --contract default --require-events
```

Use `--contract ticker` or `--contract stream` to force the corresponding legacy subscription flag. The default
uses the C API's normal per-record contract selection, which is the relevant baseline for reproducing existing
legacy-client behavior. Pass `--trace-subscriptions` to `latency_server` to log the symbol cardinality it observes.
With the default C API contract, a smoke run using one base symbol produced 27 server-side subscriptions for each
recurring type: the composite symbol plus `&A` through `&Z`. `Profile` produced only the composite subscription.
Consequently, `SUB:Q1;T1;E1;S1` represents 109 observed server-side keys even though it publishes four recurring
composite events per period.

The two client paths remain isolated and intentionally measure different observables:

```mermaid
flowchart LR
    S["Synthetic Graal CXX publisher<br/>composite Q/T/E/S + Profile"]
    S --> QD["QD transport on loopback"]
    QD --> G["Graal CXX client<br/>STREAM_FEED or FEED"]
    QD --> L["Legacy C API client<br/>default/ticker/stream contract"]
    G --> GL["Marker-correlated E2E latency<br/>delivery and callback metrics"]
    L --> LL["Delivery rate and callback shape<br/>no common publish-time marker"]
```

`latency_runner` can select the client and TimeAndSale setup per profile using optional fields:
`PROFILE=name|task|client-role|events-batch-limit|aggregation-period|client-implementation|time-series-prefill|time-series-history|time-series-subscribe-after`.
The implementation is `graal` by default, preserving existing suite files; select `graal-delivery` for the minimal
C++ listener or `legacy` only in builds configured with `LATENCY_BUILD_LEGACY_CLIENT=ON`. Both delivery-only paths
write `<prefix>-delivery.csv`, and the analyzer produces separate
`delivery-runs.csv` and `delivery-comparison.csv` files instead of presenting delivery counters as latency.

The ready-to-run `tools/legacy-api-comparison.conf` suite rotates three repetitions of the Graal CXX `STREAM_FEED`
and default legacy C API clients at the same shuffled 150,000 composite events/s workload. It compares observable
delivery and callback shape. It does not claim that a callback-rate difference proves a transport regression or
that the two APIs expose equivalent latency timestamps.

`tools/api-capacity-discovery.conf` performs one short discovery run for the Graal CXX `STREAM_FEED`, Graal CXX
`FEED`, and default legacy C API paths at 150,000, 250,000, 300,000, 375,000, and 500,000 recurring events/s. Every
profile keeps 375 symbols and 1,500 events per publication; only the 10, 6, 5, 4, or 3 ms publication period changes.
Use it to select the light, boundary, and overloaded rates for a subsequent repeated confirmation suite. A FEED
listener deficit is not by itself overload because FEED may supersede intermediate ticker states by design.

`tools/api-capacity-confirmation.conf` repeats the selected 150,000 baseline, 375,000 pre-knee, and 500,000
publisher-knee rates three times in rotating order. Its longer warm-up and measurement intervals test whether the
discovery result is repeatable and whether either non-conflating client falls behind before the current publisher.

`tools/api-delivery-overhead.conf` repeats those three rates with the full marker-correlating Graal client, the
minimal Graal `STREAM_FEED` delivery-only client, and the default legacy delivery-only client. This isolates the CPU
and RSS consumed by the benchmark's correlation, windowing, and latency-sample retention from work performed by the
C++ API delivery path itself. The 500,000-events/s point remains a publisher-capacity observation rather than a clean
client limit.

`tools/graal-delivery-release-control.conf` runs only the minimal Graal `STREAM_FEED` delivery client at the same
three rates. Invoke it from separately configured v5.0.0, v7.0.0, and v8.0.0 build directories to compare release
stacks without marker correlation or retained latency samples. Each run directory records the selected CXX API,
Native SDK, and QD versions in `environment.txt`; do not combine rows until those identities have been verified.

`tools/inactive-subscription-cardinality.conf` holds recurring publication at 150,000 events/s on the first 375 base
symbols while increasing the subscribed universe and initial Profile state from 375 to 3,750 and 10,000 symbols. It
compares the minimal Graal `STREAM_FEED` client with legacy C API default delivery, including process CPU/RSS and QD
subscription/storage counters. The suite sets `ACTIVE_SYMBOLS=375`; without that server setting, `#N` remains a
rotating publication universe rather than a set of inactive subscriptions.

`tools/regional-fanout.conf` compares zero, one, four, and twenty-six active regional sources for both clients while
holding the aggregate recurring rate at 150,000 events/s. This separates record-key routing and subscription fan-out
from a simple increase in network throughput.

Run it from the same legacy-enabled build with:

```powershell
.\build-legacy\Release\latency_runner.exe --binary-directory .\build-legacy\Release `
    --config .\tools\regional-fanout.conf
```

```sh
./build-legacy/latency_runner --binary-directory ./build-legacy \
    --config ./tools/regional-fanout.conf
```

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
`<profile>-time-series.csv`, or `<profile>-delivery.csv`, plus `<profile>-server.log` and `<profile>-client.log` files.
It writes every parsed interval to
`monitoring.csv` and profile/process aggregates for intervals wholly inside the measurement phase to
`monitoring-summary.csv`. QD log timestamps have no UTC offset and are interpreted in the analyzer process's local
time zone; analyze moved logs with the same `TZ` setting as the machine that produced them.

## Repeated local benchmark suite

The repository includes native launchers for a longer cadence comparison suite. They run three repetitions of four
mixed profiles in `FEED` mode, all nominally producing 150,000 recurring events/s: 150,000 events every second,
15,000 every 100 ms, 1,500 every 10 ms, and 150 every 1 ms. Each profile also sends one initial `Profile` per
instrument. The 1 ms profile is a scheduler/publisher stress case and should be interpreted separately. Every run
uses a one-minute warm-up, a ten-minute measurement, ten-second windows, and a fresh server/client pair. Profile
order rotates between repetitions and a 30-second cool-down separates runs.

Build the Release binaries first, then run the cross-platform `latency_runner` executable. On Windows:

```powershell
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\benchmark-suite.conf
```

On Linux or macOS:

```sh
./build/latency_runner --binary-directory ./build \
    --config ./tools/benchmark-suite.conf
```

Use `--dry-run` to validate the suite and display all planned commands without starting a benchmark. Pass
`--config tools/benchmark-suite.conf` to select the suite, including its independent `STARTUP_TIMEOUT`. Results are
written below `benchmark-results/<UTC timestamp>/`. A full default run takes approximately two hours and twenty
minutes plus any machine-dependent startup overhead.

A `PROFILE` line may override the endpoint role, events batch limit, aggregation period, client implementation,
TimeAndSale prefill, server history limit, and delayed-subscription time for that profile using
`PROFILE=name|task|client-role|events-batch-limit|aggregation-period|client-implementation|time-series-prefill|time-series-history|time-series-subscribe-after`.
Omitted fields inherit `CLIENT_ROLE`, `EVENTS_BATCH_LIMIT`, `AGGREGATION_PERIOD`, `TIME_SERIES_PREFILL`, and
`TIME_SERIES_HISTORY` from the suite. `TIME_SERIES_SUBSCRIBE_AFTER` is optional and disabled when omitted. Batch limit
and aggregation default to `optimal` and `0`, while the client implementation defaults to `graal`. Command-line
`--events-batch-limit` and `--aggregation-period` provide suite-wide overrides for profiles that do not specify them.
`TIME_SERIES_UNSUBSCRIBE_AFTER_SNAPSHOT=true` requires a delayed TimeAndSale subscription and applies to every
TimeAndSale profile in the suite. The run manifest records the effective prefill, history limit, delayed-subscription
time, and post-snapshot removal mode for every execution.

A suite may describe its experiment with `EXPERIMENT_TITLE`, `EXPERIMENT_OBJECTIVE`, `EXPERIMENT_VARIABLE`,
`EXPERIMENT_CONTROLS`, `EXPERIMENT_SUCCESS_CRITERIA`, and `EXPERIMENT_LIMITATIONS`. These settings are optional for
backward compatibility, but when one is present all six are required and must be non-empty. The analyzer reads them
from the preserved `suite.conf` and writes an `Experiment definition` section near the top of `REPORT.md`. Success
criteria describe which measurements should be evaluated; they do not turn the report into an automatic pass/fail
decision.

[`tools/time-series-snapshot.conf`](tools/time-series-snapshot.conf) is the controlled TimeAndSale HISTORY experiment.
It runs three repetitions at 150,000 total recurring events/s and reports initial snapshot completeness and flags,
snapshot-to-live cutover, live TimeAndSale latency, regular ticker latency, QD monitoring, CPU, and RSS.
TimeAndSale HISTORY suites require the default CXX API v8.0.0 build; the pinned v5/v7 builds are retained only for
the non-HISTORY release-stack controls described below.

[`tools/time-series-scaling.conf`](tools/time-series-scaling.conf) keeps the Q/T/E/S/N publication rate fixed and
varies first the common subscribed symbol universe at a fixed 200-event history depth, then history depth at 375
symbols. A common 10.5-second prefill fills every bounded server-history cap without changing the effective JVM/QD
warm-up time between profiles. This separates cardinality scaling from depth scaling while preserving the same
steady-state offered event rate. The
TimeAndSale report includes Graal-client CPU and RSS sampled after snapshot completion during the measurement
interval.

[`tools/time-series-overlap.conf`](tools/time-series-overlap.conf) measures transient interference from a TimeAndSale
HISTORY subscription added ten seconds after ticker measurement begins. It keeps the common 375-symbol Q/T/E/S/N
workload and nominal 187,500 events/s fixed while varying retained history depth across 100, 200, and 1,000 events per
symbol. The analyzer writes `snapshot-overlap-runs.csv` and `snapshot-overlap-comparison.csv` and adds a
before/during/after table to `REPORT.md`.

[`tools/time-series-recovery.conf`](tools/time-series-recovery.conf) repeats that depth sweep but removes the
TimeAndSale symbols at global snapshot completion. Its Q/T/E/S-only `before` and `after` phases therefore have the
same 150,000 events/s subscribed workload, while the short `during` phase contains the HISTORY burst and live
TimeAndSale updates. This matched control tests whether ticker latency recovers after the transient time-series load.

For a short contract A/B, run `tools/conflation-diagnostic.conf` once with the default `feed` role and once with a
`stream-feed` override. The task, symbol set, cadence, warm-up, and measurement duration remain identical:

```powershell
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\conflation-diagnostic.conf --client-role feed
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\conflation-diagnostic.conf --client-role stream-feed
```

```sh
./build/latency_runner --binary-directory ./build \
    --config ./tools/conflation-diagnostic.conf --client-role feed
./build/latency_runner --binary-directory ./build \
    --config ./tools/conflation-diagnostic.conf --client-role stream-feed
```

To test whether client-side listener speed contributes to FEED supersession, repeat the FEED diagnostic with a
controlled delay before every market-event callback. `0` disables the delay:

```powershell
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\conflation-diagnostic.conf --client-role feed --listener-delay 1ms
```

```sh
./build/latency_runner --binary-directory ./build \
    --config ./tools/conflation-diagnostic.conf --client-role feed --listener-delay 1ms
```

`tools/symbol-cardinality.conf` compares 375, 3,750, and 10,000 subscribed symbols while keeping the recurring
workload at 150,000 events/s. A rotating 375-symbol slice ticks in each publication, so the entire universe can become
active over time. Use `tools/inactive-subscription-cardinality.conf` when only the first 375 symbols should ever tick:

```powershell
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\symbol-cardinality.conf
```

```sh
./build/latency_runner --binary-directory ./build \
    --config ./tools/symbol-cardinality.conf
```

`tools/event-order.conf` keeps the 375-symbol, 150,000-events/s workload fixed and places a different event type at
the end of each publication. The server preserves the event-type order written in the task DSL. This isolates an
event-class effect from a serialization-position effect:

```powershell
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\event-order.conf
```

```sh
./build/latency_runner --binary-directory ./build \
    --config ./tools/event-order.conf
```

After the fixed-order comparison, `tools/event-order-shuffle.conf` uses seed `22805` to reshuffle the four event-type
blocks for every publication. The operation shuffles four indices, not all 1,500 events, so its cost is negligible
and included in the reported server preparation time:

```powershell
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\event-order-shuffle.conf
```

```sh
./build/latency_runner --binary-directory ./build \
    --config ./tools/event-order-shuffle.conf
```

`tools/events-batch-limit.conf` compares native notification limits while holding the shuffled 375-symbol workload
at 150,000 events/s. It includes `optimal`, `1`, `375`, `1500`, and `maximum` FEED profiles plus a STREAM_FEED
control. Limit `1` is intentionally a callback-overhead stress case:

```powershell
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\events-batch-limit.conf
```

```sh
./build/latency_runner --binary-directory ./build \
    --config ./tools/events-batch-limit.conf
```

`tools/aggregation-period.conf` isolates the per-subscription aggregation setting with FEED profiles using `0`,
`1ms`, and `10ms`, plus a `STREAM_FEED` profile using `0` as a delivery-contract control. All profiles use the same
shuffled 375-symbol, 150,000-event/s workload. The aggregation setting is applied only to the combined recurring
market-event subscription before symbols are added. Initial `Profile` events use a separate subscription, and the
`TextMessage` control channel is also separate. The client records the effective value returned by the C++ API in
`aggregation_period_ms`; no Java system property is involved.

```powershell
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\aggregation-period.conf
```

```sh
./build/latency_runner --binary-directory ./build \
    --config ./tools/aggregation-period.conf
```

After that A/B test, `tools/aggregation-stream-control.conf` repeats the `0`, `1ms`, and `10ms` aggregation periods
with `STREAM_FEED`. It checks whether non-zero aggregation changes only notification batching and latency while the
non-conflating delivery contract still preserves every recurring event. The suite performs three repetitions and
takes approximately 15 minutes:

```powershell
.\build\Release\latency_runner.exe --binary-directory .\build\Release `
    --config .\tools\aggregation-stream-control.conf
```

The benchmark passes `monitoring.stat` directly to `DXEndpoint::Builder`. This is required for `STREAM_FEED`, whose
endpoint configuration does not import Java system properties. Both client and server monitoring should therefore
be present in the generated report; an `n/a` value still means that no parseable sample was emitted and must not be
interpreted as zero.

```sh
./build/latency_runner --binary-directory ./build \
    --config ./tools/aggregation-stream-control.conf
```

`tools/sdk-feed-control.conf` is the short release-stack FEED control. Run it from separately configured v5 and v7
build directories to compare natural TICKER supersession with no artificial listener delay or notification
aggregation. It performs three repetitions per stack and takes approximately seven minutes for each invocation:

```powershell
.\build-v5\Release\latency_runner.exe --binary-directory .\build-v5\Release `
    --config .\tools\sdk-feed-control.conf
.\build-v7\Release\latency_runner.exe --binary-directory .\build-v7\Release `
    --config .\tools\sdk-feed-control.conf
```

```sh
./build-v5/latency_runner --binary-directory ./build-v5 \
    --config ./tools/sdk-feed-control.conf
./build-v7/latency_runner --binary-directory ./build-v7 \
    --config ./tools/sdk-feed-control.conf
```

Each output prefix includes its repetition, for example `150k-100ms-r02`. The analyzer additionally writes
`latency-runs.csv`, `latency-comparison.csv`, `client-resource-runs.csv`, `client-resource-comparison.csv`,
`monitoring-comparison.csv`, `snapshot-overlap-runs.csv`, `snapshot-overlap-comparison.csv`, and a concise
`REPORT.md`. The snapshot-overlap CSVs contain headers only when the suite did not enable delayed TimeAndSale
subscription. Comparison CSVs contain the minimum, median, and maximum of run-level values; original summaries and
logs remain available for more detailed analysis. A failed run is recorded
in `run-manifest.csv`, its partial CSV files are preserved with a `.partial.csv` suffix, and the remaining profiles
still run.

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

dxFeed Graal CXX API v8.0.0 fixes packed-field setters, including `TimeAndSale::setSequence()`, so changing a sequence
preserves the timestamp stored in the same 64-bit index. This is required by the synthetic HISTORY publisher: older
versions could turn a current timestamp into a value near the Unix epoch, which HISTORY then correctly rejected as
older than the requested `fromTime`. A regression test protects this ordering in the benchmark build.

`Trade` events are correlated with the marker by sequence, `Summary` events by a synthetic `dayId`, and `Quote`
events by the seconds component of their exchange time. The last mapping is unambiguous at the fixed rate of one
batch per second. This is a synthetic wire contract used by the test; `Summary::dayId` does not represent a trading
date here.

The client defaults to `STREAM_FEED`, so conflation does not intentionally hide intermediate updates. The benchmark
suite selects `FEED` explicitly to reproduce normal feed semantics. Latency is stored in nanoseconds and displayed
in microseconds. A value above `Q3 + 1.5 * IQR` for the current window is classified as an outlier. The current QD
implementation explicitly changes the receiving agent's overflow strategy to `BLOCK` for `STREAM_FEED`. This
preserves intermediate updates by applying backpressure when its finite buffer fills. Other endpoint roles and QD
buffers can use different overflow behavior, so the benchmark still records `Dropped` on both processes.

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

The source-level distinction between normal `FEED` supersession and `STREAM_FEED` buffering is documented in
[`benchmark-results/QD-FEED-DELIVERY-PATH.md`](benchmark-results/QD-FEED-DELIVERY-PATH.md). A controlled comparison
of two CXX API, Native SDK, and QD release stacks is in
[`benchmark-results/SDK-VERSION-COMPARISON.md`](benchmark-results/SDK-VERSION-COMPARISON.md). The repeated delivery
comparison between the Graal CXX `STREAM_FEED` client and legacy C API default client is in
[`benchmark-results/20260906T145936Z/REPORT.md`](benchmark-results/20260906T145936Z/REPORT.md). The fixed-rate
regional fan-out comparison is in
[`benchmark-results/20260906T154238Z/REPORT.md`](benchmark-results/20260906T154238Z/REPORT.md). The first complete
TimeAndSale HISTORY snapshot-to-live run on CXX API v8.0.0 is in
[`benchmark-results/20260906T213400Z/REPORT.md`](benchmark-results/20260906T213400Z/REPORT.md). The controlled
cardinality/depth result and its interpretation are in
[`benchmark-results/20260906T230501Z/REPORT.md`](benchmark-results/20260906T230501Z/REPORT.md) and
[`benchmark-results/TIME-SERIES-SCALING.md`](benchmark-results/TIME-SERIES-SCALING.md). The in-measurement HISTORY
experiment and its interpretation are in
[`benchmark-results/20260907T095845Z/REPORT.md`](benchmark-results/20260907T095845Z/REPORT.md) and
[`benchmark-results/TIME-SERIES-OVERLAP.md`](benchmark-results/TIME-SERIES-OVERLAP.md).
The matched post-snapshot recovery control is in
[`benchmark-results/20260907T104556Z/REPORT.md`](benchmark-results/20260907T104556Z/REPORT.md) and
[`benchmark-results/TIME-SERIES-RECOVERY.md`](benchmark-results/TIME-SERIES-RECOVERY.md).
The first API delivery-capacity sweep and its interpretation are in
[`benchmark-results/20260907T111601Z/REPORT.md`](benchmark-results/20260907T111601Z/REPORT.md) and
[`benchmark-results/API-CAPACITY-DISCOVERY.md`](benchmark-results/API-CAPACITY-DISCOVERY.md).
The repeated baseline/pre-knee/publisher-knee confirmation is in
[`benchmark-results/20260907T113447Z/REPORT.md`](benchmark-results/20260907T113447Z/REPORT.md) and
[`benchmark-results/API-CAPACITY-CONFIRMATION.md`](benchmark-results/API-CAPACITY-CONFIRMATION.md).
The full-versus-delivery-only resource comparison is in
[`benchmark-results/20260907T123134Z/REPORT.md`](benchmark-results/20260907T123134Z/REPORT.md) and
[`benchmark-results/API-DELIVERY-OVERHEAD.md`](benchmark-results/API-DELIVERY-OVERHEAD.md).
The delivery-only release-stack control for v5.0.0, v7.0.0, and v8.0.0 is summarized in
[`benchmark-results/SDK-VERSION-COMPARISON.md`](benchmark-results/SDK-VERSION-COMPARISON.md), with source reports
under `20260907T133807Z`, `20260907T135741Z`, and `20260907T134755Z`, respectively.
The mostly idle subscription-cardinality comparison between Graal `STREAM_FEED` and legacy default delivery is in
[`benchmark-results/20260907T145209Z/REPORT.md`](benchmark-results/20260907T145209Z/REPORT.md) and
[`benchmark-results/INACTIVE-SUBSCRIPTION-CARDINALITY.md`](benchmark-results/INACTIVE-SUBSCRIPTION-CARDINALITY.md).
The controlled reproduction of the customer's global source-time filter is in
[`benchmark-results/20260907T160901Z/REPORT.md`](benchmark-results/20260907T160901Z/REPORT.md), with the complete
methodology review and interpretation in
[`benchmark-results/CUSTOMER-SOURCE-TIME-METHODOLOGY.md`](benchmark-results/CUSTOMER-SOURCE-TIME-METHODOLOGY.md).

The legacy C API does not implement the newer client-side FEED conflation mechanism, delivers events to its callback
one at a time, and does not support `TextMessage`, which the Graal benchmark uses as the exact per-publication
timestamp marker. The implemented legacy comparison therefore reports delivery rate and callback shape, not E2E
latency. A direct latency comparison still requires a different marker carried by an event supported by both APIs;
that marker must be validated for identical serialization and decoding before comparing its results with the Graal
reports.

## QD monitoring statistics

Periodic `{latency-server}` and `{latency-client}` records include subscription, storage, outgoing-buffer,
dropped-record, I/O-rate, data-lag, round-trip-time, and process-CPU statistics when available. I/O rates describe
the interval since the previous report. CPU is normalized to the total capacity of all logical processors, so one
fully occupied logical processor on a 32-processor machine is approximately `3.125%`.
