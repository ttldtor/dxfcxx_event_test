# Transient ticker interference during TimeAndSale HISTORY delivery

## Experiment definition

- **Objective:** Determine whether adding a TimeAndSale HISTORY subscription temporarily increases ticker listener latency, delivery deficit, CPU, or RSS.
- **Changed variable:** Retained TimeAndSale history depth delivered during an already-running ticker measurement.
- **Controls:** CXX API v8 stack, 375 subscribed and active symbols, fixed Q/T/E/S/N quantities, 10 ms cadence, FEED role, zero aggregation, loopback transport, warm-up, measurement phases, and publication order seed.
- **Evaluation criteria:** Complete every snapshot with QD Dropped zero and compare marker-correlated ticker latency, listener coverage, CPU, and RSS before, during, and after snapshot delivery.
- **Limitations:** The synthetic in-process server and loopback transport do not reproduce production multiplexer retention or network conditions; the snapshot phase is short and resource sampling can contain few observations; phase correlation counters can straddle exact boundaries.

## TimeAndSale snapshot and live cutover

The client adds a separate HISTORY subscription after the configured prefill. Snapshot and live values are medians
across repetitions; the event range is the minimum and maximum complete snapshot size.

| Scenario | Runs | Completed symbols | Snapshot events median (range) | Snapshot callbacks | Snapshot duration | SNIP | First live vs global completion | Live p99 | CPU, one-core basis | RSS mean / maximum | Integrity |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| tns-overlap-h100 | 3 | 375 | 37891 (37680–38011) | 53 | 407.528 ms | 375 | -10.040 ms | 18534.300 us | 42.966% | 241.265 MiB / 415.074 MiB | OK |
| tns-overlap-h1000 | 3 | 375 | 374975 (374974–374980) | 424 | 1532.120 ms | 375 | -274.714 ms | 14344.000 us | 41.511% | 244.931 MiB / 472.871 MiB | OK |
| tns-overlap-h200 | 3 | 375 | 75834 (75691–76382) | 96 | 502.112 ms | 375 | -47.758 ms | 15491.500 us | 41.585% | 248.963 MiB / 477.445 MiB | OK |


Integrity requires every requested symbol to complete with `SNAPSHOT_END` or `SNAPSHOT_SNIP`, no duplicate indices,
no live events before that symbol's snapshot completion, no clock anomalies, and at least one measured live
TimeAndSale event.
`First live vs global completion` is negative when symbols that completed early start receiving live updates while
snapshots for other symbols are still in progress; this is valid per-symbol snapshot-to-live overlap. `SNAPSHOT_SNIP`
is reported separately because it is an expected bounded-history condition, not an integrity failure.
CPU and RSS are sampled in the Graal client during the configured measurement interval, after the initial snapshot.

## Ticker latency around TimeAndSale snapshot delivery

The client starts ticker measurement first, adds the TimeAndSale HISTORY subscription after the configured delay,
and partitions ticker observations into `before`, `during`, and `after` phases. Phase boundaries are detected by the
client at subscription and global snapshot completion. Values are medians across repetitions.

| Scenario | Phase | Runs | Duration | Listener coverage | Event p50 | Event p99 (range) | Event p99.9 | Event maximum | CPU, one-core basis | RSS mean / maximum |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| tns-overlap-h100 | after | 3 | 29591.600 ms | 99.068% | 3354.000 us | 14615.100 (8904.900–19188.600) us | 29292.600 us | 38698.500 us | 41.102% | 274.583 / 415.074 MiB |
| tns-overlap-h100 | before | 3 | 10069.000 ms | 99.800% | 2770.200 us | 5737.700 (4276.300–7652.700) us | 12524.300 us | 14491.800 us | 30.789% | 145.725 / 192.965 MiB |
| tns-overlap-h100 | during | 3 | 407.528 ms | 96.667% | 3497.500 us | 16020.000 (13422.400–18713.400) us | 19147.800 us | 19147.800 us | 26.087% | 106.389 / 110.398 MiB |
| tns-overlap-h1000 | after | 3 | 28506.800 ms | 99.218% | 3228.900 us | 8093.900 (7262.400–11972.700) us | 23675.200 us | 31471.700 us | 40.416% | 286.447 / 472.871 MiB |
| tns-overlap-h1000 | before | 3 | 10084.500 ms | 99.900% | 2778.700 us | 5528.400 (4449.500–6401.800) us | 14000.000 us | 15076.900 us | 33.087% | 149.185 / 199.520 MiB |
| tns-overlap-h1000 | during | 3 | 1532.120 ms | 95.238% | 4192.400 us | 18703.500 (17614.000–19114.700) us | 24652.200 us | 24652.200 us | 42.582% | 117.361 / 159.453 MiB |
| tns-overlap-h200 | after | 3 | 29452.800 ms | 99.384% | 3157.800 us | 11552.800 (7012.900–14255.000) us | 27414.300 us | 32461.800 us | 40.172% | 285.572 / 477.445 MiB |
| tns-overlap-h200 | before | 3 | 10085.300 ms | 99.900% | 2860.700 us | 6177.200 (4339.900–6606.900) us | 12957.800 us | 16861.200 us | 30.986% | 148.002 / 197.691 MiB |
| tns-overlap-h200 | during | 3 | 502.112 ms | 95.556% | 3720.200 us | 15370.100 (14149.100–19400.200) us | 19665.300 us | 20065.700 us | 21.042% | 110.224 / 115.203 MiB |


Phase latency is based on marker-correlated Quote/Trade/TradeETH/Summary listener observations. Resource sampling is
performed separately in each phase; 100% CPU means one fully occupied logical core. A short `during` phase can have
few samples, so its range and the raw per-run CSV must be considered alongside the median. Phase delivery counters
can straddle a boundary when market events and their timestamp marker complete on opposite sides of it; whole-run
integrity and QD `Dropped` remain the authoritative loss checks.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| tns-overlap-h100 | feed | optimal | 0.000 ms | 3 | 99.500% | 31380.000 | 3.188 | 12.390 (9.178–15.827) | 28.974 | 13.326 | OK |
| tns-overlap-h1000 | feed | optimal | 0.000 ms | 3 | 99.510% | 29342.000 | 3.111 | 11.430 (11.028–11.500) | 23.365 | 13.127 | OK |
| tns-overlap-h200 | feed | optimal | 0.000 ms | 3 | 99.626% | 23165.000 | 3.043 | 11.099 (7.341–12.537) | 25.734 | 12.799 | OK |


`Listener coverage median` is recurring events observed by the C++ listener divided by events expected for the
correlated publications. `Listener deficit` is the corresponding observation gap. It is not a transport-loss or
QD-drop counter. In `FEED` mode, the gap may contain TICKER states superseded before listener delivery; endpoint
buffering, `Dropped` records, incomplete publication correlation, and measurement boundaries must also be checked.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
listener coverage. Because FEED does not preserve publication boundaries, listener deficit and per-marker excess
are observations rather than integrity failures; STREAM_FEED still requires exact correlated delivery.

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `time-series-runs.csv`, `time-series-comparison.csv`, `snapshot-overlap-runs.csv`, `snapshot-overlap-comparison.csv`, `delivery-runs.csv`, `delivery-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| tns-overlap-h100 | 177676.571 | 0.455 | 1.289% | 132.000 | 0.000 |
| tns-overlap-h1000 | 183571.857 | 0.765 | 1.279% | 32.000 | 0.000 |
| tns-overlap-h200 | 178426.571 | 0.442 | 1.259% | 0.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| tns-overlap-h100 | 177676.429 | 0.129 | 3.441% | 0.000 | 0.000 |
| tns-overlap-h1000 | 183559.000 | 0.126 | 3.449% | 1.000 | 0.000 |
| tns-overlap-h200 | 178436.714 | 0.129 | 3.494% | 238.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
