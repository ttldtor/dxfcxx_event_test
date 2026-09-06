# TimeAndSale snapshot-to-live benchmark

## Experiment definition

- **Objective:** Measure initial HISTORY snapshot delivery and the transition to live TimeAndSale while ticker-like events continue.
- **Changed variable:** Initial TimeAndSale snapshot followed by a steady live TimeAndSale stream.
- **Controls:** Compiler, host, FEED role, 300 shared symbols, nominal 150,000 recurring events/s, zero aggregation, warm-up, duration, and monitoring period.
- **Evaluation criteria:** Verify snapshot completion without duplicates or premature live events, then compare live TimeAndSale latency, ticker latency, QD drops, CPU, and RSS across repetitions.
- **Limitations:** Synthetic loopback traffic and in-process bounded history do not reproduce production multiplexer retention, network conditions, or legacy C API delivery.

## TimeAndSale snapshot and live cutover

The client adds a separate HISTORY subscription after the configured prefill. Snapshot and live values are medians
across repetitions; the event range is the minimum and maximum complete snapshot size.

| Scenario | Runs | Completed symbols | Snapshot events median (range) | Snapshot callbacks | Snapshot duration | SNIP | First live after snapshot | Live p99 | Integrity |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| time-series-snapshot | 3 | 300 | 63080 (62928–63267) | 72 | 165.213 ms | 0 | -41.849 ms | 15419.000 us | OK |


Integrity requires every requested symbol to complete with `SNAPSHOT_END`, no duplicate indices, no live events
before snapshot completion, no clock anomalies, and at least one measured live TimeAndSale event. `SNAPSHOT_SNIP`
is reported separately because it is an expected bounded-history condition, not an integrity failure.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| time-series-snapshot | feed | optimal | 0.000 ms | 3 | 99.865% | 10107.000 | 2.480 | 14.563 (13.794–14.828) | 22.978 | 15.895 | CHECK |


`Listener coverage median` is recurring events observed by the C++ listener divided by events expected for the
correlated publications. `Listener deficit` is the corresponding observation gap. It is not a transport-loss or
QD-drop counter. In `FEED` mode, the gap may contain TICKER states superseded before listener delivery; endpoint
buffering, `Dropped` records, incomplete publication correlation, and measurement boundaries must also be checked.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
listener coverage.

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `time-series-runs.csv`, `time-series-comparison.csv`, `delivery-runs.csv`, `delivery-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| time-series-snapshot | 144347.667 | 0.517 | 0.806% | 0.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| time-series-snapshot | 144349.000 | 0.116 | 2.469% | 35.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
