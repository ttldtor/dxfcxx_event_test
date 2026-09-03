# dxFeed Graal C++ latency test

This project is a local two-process load-testing tool. The server publishes one batch of synthetic `Quote`, `Trade`,
and `Summary` events per second. The client measures the time between the server's `publishEvents` call and delivery
to the C++ event listener.

## Building and running

```text
cmake -S . -B build
cmake --build build --config Release

build/Release/latency_server.exe --address :7400
build/Release/latency_client.exe --address 127.0.0.1:7400 --task "SUB:Q100;S1;T5"
```

With a single-configuration generator, the executables are placed directly in `build/`. Run either executable with
`--help` to see all available options. By default, the client performs a 30-second warm-up followed by a five-minute
measurement period divided into 10-second windows. It writes `latency-summary.csv` and `latency-outliers.csv`.
Both processes also set `monitoring.stat=10s`, which makes QD report its internal endpoint statistics every 10
seconds. Pass `--monitoring-stat 0` to either process to disable these reports or provide another positive duration.

`Q100` creates the instruments `Q00` through `Q99`, each updated once per second. All event types generated for one
tick are sent in a single `publishEvents` call.

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

The client uses `STREAM_FEED` so that conflation does not hide delayed events. Latency is stored in nanoseconds and
displayed in microseconds. A value above `Q3 + 1.5 * IQR` for the current window is classified as an outlier.
`STREAM_FEED` preserves intermediate updates, but its agent buffer is finite: QD uses `DROP_OLDEST` by default and
increments the monitoring `Dropped` counter if a consumer falls far enough behind to overflow that buffer.

The summary contains aggregate `event` and `batch` rows plus `event-quote`, `event-trade`, and `event-summary` rows.
Their `expected_per_batch` value is the expected sample count for that row (`1` for `batch`). Event outliers are
classified against the IQR threshold of their own event type and use the corresponding type-specific sample kind in
the outliers file.

## QD monitoring statistics

The periodic `{latency-server}` and `{latency-client}` log records include subscription, storage, outgoing-buffer,
dropped-record, I/O-rate, data-lag, round-trip-time, and process-CPU statistics when those values are available. I/O
rates describe the interval since the previous report. CPU is normalized to the total capacity of all logical
processors, so one fully occupied logical processor on a 32-processor machine is approximately `3.125%`.

For a benchmark directory containing matching `<profile>-summary.csv`, `<profile>-server.log`, and
`<profile>-client.log` files, convert the monitoring records to CSV with:

```powershell
pwsh -File tools/analyze-monitoring.ps1 -RunDirectory benchmark-results/<run>
```

The script writes all intervals to `monitoring.csv` and profile/process aggregates for intervals entirely inside the
measurement phase to `monitoring-summary.csv`.
