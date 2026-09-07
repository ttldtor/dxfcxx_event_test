# Marker-correlated and delivery-only API overhead comparison

## Experiment definition

- **Objective:** Determine how much of the observed client CPU, RSS, and delivery behavior belongs to benchmark timestamp correlation rather than the Graal CXX or legacy API delivery path.
- **Changed variable:** Client callback work at 150,000, 375,000, and 500,000 nominal events/s: full Graal marker correlation, Graal STREAM_FEED delivery-only counters, and legacy default delivery-only counters.
- **Controls:** Host, compiler, synthetic server, 375-symbol shuffled Q/T/E/S workload, 1,500 events per publication, warm-up, duration, monitoring period, and rotating run order.
- **Evaluation criteria:** Compare observed delivery rate, callback shape, QD drops, CPU, and RSS while retaining Graal E2E latency as a separate full-client result.
- **Limitations:** The synthetic publisher approaches its own capacity at 500,000 nominal events/s; the legacy callback API delivers one event at a time; the full and delivery-only reports expose different observables; this fixed independent event mix is not a causal market-microstructure model.

## Delivery-only client results

These values describe callback delivery and callback shape without timestamp correlation or per-event sample
allocation. They are not timestamp-based E2E latency measurements. Rates are medians across repetitions.

| Scenario | Client | Contract / role | Runs | Nominal events/s | Observed events/s median (range) | Callbacks median | Maximum callback batch | CPU, one-core basis | CPU, host basis | RSS mean / maximum |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 150k-graal-delivery | graal | stream-feed | 3 | 150000.000 | 149322.128 (149187.119–149903.111) | 11607.000 | 955 | 23.054% | 0.720% | 94.459 MiB / 99.824 MiB |
| 150k-legacy-default | legacy | default | 3 | 150000.000 | 149087.127 (149033.963–149166.918) | 4476000.000 | 1 | 11.591% | 0.362% | 7.264 MiB / 7.293 MiB |
| 375k-graal-delivery | graal | stream-feed | 3 | 375000.000 | 368310.879 (366612.099–369255.338) | 27393.000 | 1057 | 54.569% | 1.705% | 68.648 MiB / 92.047 MiB |
| 375k-legacy-default | legacy | default | 3 | 375000.000 | 368825.656 (368053.487–368868.184) | 11067772.000 | 1 | 22.812% | 0.713% | 7.277 MiB / 7.309 MiB |
| 500k-graal-delivery | graal | stream-feed | 3 | 500000.000 | 478839.974 (469140.113–490445.500) | 39408.000 | 1057 | 83.047% | 2.595% | 65.286 MiB / 81.332 MiB |
| 500k-legacy-default | legacy | default | 3 | 500000.000 | 478518.676 (476458.430–489894.353) | 14378796.000 | 1 | 31.928% | 0.998% | 7.292 MiB / 7.324 MiB |


With the default legacy contract, the C API expands each base-symbol Quote, Trade, TradeETH, and Summary subscription
into the composite plus 26 regional symbols. A task can publish a configured subset of those regional record keys
while keeping its recurring event rate fixed. CPU uses both a one-core basis and a host-normalized basis; RSS is
sampled by the cross-platform `ttldtor/Process` library during the measurement interval. The Graal delivery-only
client preserves native vector callback sizes and performs only the type inspection needed for per-type counters.

## Client process resources

These measurements cover the complete client process during its measurement interval. Full Graal includes marker
correlation, latency-sample retention, window statistics, and outlier reporting; delivery-only clients count callback
delivery without retaining per-event latency samples. CPU is normalized both to one logical core and to the host.

| Scenario | Client workload | Runs | Nominal events/s | CPU, one-core median (range) | CPU, host median | RSS mean median | RSS maximum median |
|---|---|---:|---:|---:|---:|---:|---:|
| 150k-graal-delivery | graal/delivery-only | 3 | 150000.000 | 23.054% (18.835–27.634%) | 0.720% | 94.459 MiB | 99.824 MiB |
| 150k-graal-full | graal/full | 3 | 150000.000 | 25.844% (24.549–35.212%) | 0.808% | 137.184 MiB | 198.910 MiB |
| 150k-legacy-default | legacy/delivery-only | 3 | 150000.000 | 11.591% (10.311–12.602%) | 0.362% | 7.264 MiB | 7.293 MiB |
| 375k-graal-delivery | graal/delivery-only | 3 | 375000.000 | 54.569% (50.487–70.352%) | 1.705% | 68.648 MiB | 92.047 MiB |
| 375k-graal-full | graal/full | 3 | 375000.000 | 88.950% (73.625–92.283%) | 2.780% | 181.448 MiB | 299.867 MiB |
| 375k-legacy-default | legacy/delivery-only | 3 | 375000.000 | 22.812% (20.982–24.837%) | 0.713% | 7.277 MiB | 7.309 MiB |
| 500k-graal-delivery | graal/delivery-only | 3 | 500000.000 | 83.047% (66.794–86.237%) | 2.595% | 65.286 MiB | 81.332 MiB |
| 500k-graal-full | graal/full | 3 | 500000.000 | 115.871% (95.392–118.481%) | 3.621% | 214.377 MiB | 444.734 MiB |
| 500k-legacy-default | legacy/delivery-only | 3 | 500000.000 | 31.928% (24.550–33.399%) | 0.998% | 7.292 MiB | 7.324 MiB |


RSS means are arithmetic means of periodic samples; maximum RSS is less sensitive to different sample counts. The
full client calculates each window's distributions synchronously, so at high load that benchmark work can extend a
nominal window while listener callbacks continue. Use QD read/write rates and exact STREAM_FEED integrity alongside
these process measurements.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 150k-graal-full | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 2.501 | 7.874 (7.669–8.948) | 13.342 | 8.755 | OK |
| 375k-graal-full | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.070 | 17.904 (15.443–21.522) | 38.358 | 19.092 | OK |
| 500k-graal-full | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.078 | 27.868 (25.382–31.053) | 54.345 | 28.104 | OK |


`Listener coverage median` is recurring events observed by the C++ listener divided by events expected for the
correlated publications. `Listener deficit` is the corresponding observation gap. It is not a transport-loss or
QD-drop counter. In `FEED` mode, the gap may contain TICKER states superseded before listener delivery; endpoint
buffering, `Dropped` records, incomplete publication correlation, and measurement boundaries must also be checked.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
listener coverage. Because FEED does not preserve publication boundaries, listener deficit and per-marker excess
are observations rather than integrity failures; STREAM_FEED still requires exact correlated delivery.

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `client-resource-runs.csv`, `client-resource-comparison.csv`, `time-series-runs.csv`, `time-series-comparison.csv`, `snapshot-overlap-runs.csv`, `snapshot-overlap-comparison.csv`, `delivery-runs.csv`, `delivery-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-graal-delivery | 149220.200 | 0.425 | 0.728% | 297.000 | 0.000 |
| 150k-graal-full | 149897.000 | 0.343 | 0.800% | 0.000 | 0.000 |
| 150k-legacy-default | n/a | n/a | n/a | n/a | n/a |
| 375k-graal-delivery | 369059.600 | 0.424 | 1.648% | 291.000 | 0.000 |
| 375k-graal-full | 366854.500 | 0.495 | 2.796% | 370.000 | 0.000 |
| 375k-legacy-default | n/a | n/a | n/a | n/a | n/a |
| 500k-graal-delivery | 479660.200 | 0.500 | 2.614% | 604.000 | 0.000 |
| 500k-graal-full | 476865.000 | 0.480 | 3.574% | 305.000 | 0.000 |
| 500k-legacy-default | n/a | n/a | n/a | n/a | n/a |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-graal-delivery | 149220.200 | 0.100 | 3.300% | 298.000 | 0.000 |
| 150k-graal-full | 149889.400 | 0.085 | 3.316% | 0.000 | 0.000 |
| 150k-legacy-default | 149057.400 | 0.185 | 3.370% | 0.000 | 0.000 |
| 375k-graal-delivery | 369059.600 | 0.087 | 3.608% | 873.000 | 0.000 |
| 375k-graal-full | 367111.800 | 0.129 | 3.740% | 1418.000 | 0.000 |
| 375k-legacy-default | 368732.800 | 0.166 | 3.714% | 0.000 | 0.000 |
| 500k-graal-delivery | 479660.400 | 0.127 | 4.002% | 772.000 | 0.000 |
| 500k-graal-full | 478496.167 | 0.127 | 3.978% | 1167.000 | 0.000 |
| 500k-legacy-default | 480350.200 | 0.174 | 4.016% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
