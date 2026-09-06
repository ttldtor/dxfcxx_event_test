# TimeAndSale HISTORY cardinality and depth scaling

## Experiment definition

- **Objective:** Measure how a completed bounded TimeAndSale snapshot affects steady-state ticker latency, live TimeAndSale latency, and client memory.
- **Changed variable:** Subscribed symbol cardinality at depth 200, followed by retained history depth at 375 symbols.
- **Controls:** CXX API v8 stack, FEED role, Q/T/E/S/N quantities, 10 ms cadence, 10.5 s prefill, zero aggregation, warm-up, duration, and loopback transport.
- **Evaluation criteria:** Complete every requested snapshot without duplicate indices or premature live events; compare snapshot size and duration, ticker and TimeAndSale latency, QD drops, CPU, and RSS.
- **Limitations:** The symbol sweep also changes the common Q/T/E/S subscription universe; synthetic loopback traffic and bounded in-process history do not reproduce production multiplexer retention or network conditions.

## TimeAndSale snapshot and live cutover

The client adds a separate HISTORY subscription after the configured prefill. Snapshot and live values are medians
across repetitions; the event range is the minimum and maximum complete snapshot size.

| Scenario | Runs | Completed symbols | Snapshot events median (range) | Snapshot callbacks | Snapshot duration | SNIP | First live vs global completion | Live p99 | CPU, one-core basis | RSS mean / maximum | Integrity |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| tns-symbols-1500-h200 | 3 | 1500 | 301128 (300956–301306) | 341 | 580.915 ms | 1500 | -108.350 ms | 6290.300 us | 37.278% | 354.533 MiB / 406.266 MiB | OK |
| tns-symbols-375-h100 | 3 | 375 | 37777 (37705–37917) | 44 | 81.460 ms | 375 | -21.148 ms | 16826.800 us | 35.313% | 164.957 MiB / 230.582 MiB | OK |
| tns-symbols-375-h1000 | 3 | 375 | 374981 (374974–374982) | 424 | 823.662 ms | 375 | -240.756 ms | 10401.400 us | 36.380% | 177.962 MiB / 241.223 MiB | OK |
| tns-symbols-375-h200 | 3 | 375 | 75577 (75472–75652) | 86 | 155.363 ms | 375 | -28.785 ms | 16765.000 us | 36.956% | 166.408 MiB / 229.297 MiB | OK |
| tns-symbols-750-h200 | 3 | 750 | 150311 (150221–150382) | 172 | 308.890 ms | 750 | -39.559 ms | 16477.100 us | 36.633% | 216.754 MiB / 285.805 MiB | OK |


Integrity requires every requested symbol to complete with `SNAPSHOT_END`, no duplicate indices, no live events
before that symbol's snapshot completion, no clock anomalies, and at least one measured live TimeAndSale event.
`First live vs global completion` is negative when symbols that completed early start receiving live updates while
snapshots for other symbols are still in progress; this is valid per-symbol snapshot-to-live overlap. `SNAPSHOT_SNIP`
is reported separately because it is an expected bounded-history condition, not an integrity failure.
CPU and RSS are sampled in the Graal client during the configured measurement interval, after the initial snapshot.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| tns-symbols-1500-h200 | feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 2.796 | 6.840 (6.402–6.989) | 29.593 | 7.753 | OK |
| tns-symbols-375-h100 | feed | optimal | 0.000 ms | 3 | 99.870% | 6181.000 | 2.664 | 16.319 (16.158–16.691) | 25.990 | 17.221 | OK |
| tns-symbols-375-h1000 | feed | optimal | 0.000 ms | 3 | 99.827% | 8360.000 | 2.484 | 10.351 (8.726–11.476) | 21.482 | 13.437 | OK |
| tns-symbols-375-h200 | feed | optimal | 0.000 ms | 3 | 99.900% | 4816.000 | 2.528 | 16.377 (16.332–16.802) | 23.008 | 16.871 | OK |
| tns-symbols-750-h200 | feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 2.795 | 16.107 (9.838–16.671) | 36.183 | 16.761 | OK |


`Listener coverage median` is recurring events observed by the C++ listener divided by events expected for the
correlated publications. `Listener deficit` is the corresponding observation gap. It is not a transport-loss or
QD-drop counter. In `FEED` mode, the gap may contain TICKER states superseded before listener delivery; endpoint
buffering, `Dropped` records, incomplete publication correlation, and measurement boundaries must also be checked.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
listener coverage. Because FEED does not preserve publication boundaries, listener deficit and per-marker excess
are observations rather than integrity failures; STREAM_FEED still requires exact correlated delivery.

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `time-series-runs.csv`, `time-series-comparison.csv`, `delivery-runs.csv`, `delivery-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| tns-symbols-1500-h200 | 187268.800 | 0.337 | 1.148% | 85.000 | 0.000 |
| tns-symbols-375-h100 | 185755.800 | 0.416 | 1.110% | 132.000 | 0.000 |
| tns-symbols-375-h1000 | 187299.400 | 0.450 | 1.114% | 0.000 | 0.000 |
| tns-symbols-375-h200 | 185824.000 | 0.429 | 1.108% | 0.000 | 0.000 |
| tns-symbols-750-h200 | 185650.200 | 0.405 | 1.064% | 0.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| tns-symbols-1500-h200 | 187268.800 | 0.085 | 3.400% | 1.000 | 0.000 |
| tns-symbols-375-h100 | 185753.600 | 0.080 | 3.356% | 0.000 | 0.000 |
| tns-symbols-375-h1000 | 187299.400 | 0.080 | 3.395% | 0.000 | 0.000 |
| tns-symbols-375-h200 | 185813.600 | 0.079 | 3.364% | 0.000 | 0.000 |
| tns-symbols-750-h200 | 185648.400 | 0.081 | 3.370% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
