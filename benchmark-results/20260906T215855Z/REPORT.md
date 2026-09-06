# FEED release-stack comparison

## Experiment definition

- **Objective:** Check for delivery, supersession, latency, or resource regression between selected CXX API release stacks.
- **Changed variable:** Complete release stack: CXX API, Graal Native SDK, and embedded QD dependency.
- **Controls:** Compiler, host, FEED role, shuffled 375-symbol workload, 150,000 recurring events/s, zero aggregation, warm-up, and duration.
- **Evaluation criteria:** Compare listener coverage and deficit, per-type latency, callback shape, QD drops, buffers, I/O rates, and CPU across three repetitions.
- **Limitations:** Cannot attribute differences to one stack component or compare the legacy C API, which lacks TextMessage and has one-event, non-conflating client delivery semantics.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| sdk-feed-0 | feed | optimal | 0.000 ms | 3 | 99.840% | 15132.000 | 2.841 | 10.996 (10.473–11.756) | 15.420 | 12.190 | CHECK |


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
| sdk-feed-0 | 140801.400 | 0.439 | 0.764% | 0.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| sdk-feed-0 | 142110.833 | 0.154 | 2.439% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
