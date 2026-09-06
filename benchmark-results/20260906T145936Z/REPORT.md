# Graal CXX API and legacy C API delivery comparison

## Experiment definition

- **Objective:** Compare observable listener delivery rate and callback shape while keeping the synthetic composite-event workload constant.
- **Changed variable:** Client implementation and its normal delivery semantics: Graal CXX STREAM_FEED versus the default legacy C API contract.
- **Controls:** Host, compiler, synthetic server, shuffled four-type workload, 150,000 recurring composite events/s, warm-up, duration, and monitoring period.
- **Evaluation criteria:** Record per-run recurring event rate, callback count, maximum callback batch size, per-type counts, QD monitoring, and Graal-client E2E latency.
- **Limitations:** Legacy events have no common benchmark publish timestamp, regional subscriptions do not create regional publications, and callback delivery rate is not an E2E latency measurement.

## Legacy C API delivery

The legacy C API has no benchmark marker event, so these values describe callback delivery and callback shape; they
are not timestamp-based E2E latency measurements. Rates are medians across repetitions.

| Scenario | Contract | Runs | Nominal events/s | Observed events/s median (range) | Callbacks median | Maximum `data_count` |
|---|---|---:|---:|---:|---:|---:|
| legacy-default | default | 3 | 150000.000 | 148911.852 (148885.802–148912.822) | 8941597.000 | 1 |


The synthetic server publishes only composite symbols. With the default legacy contract, the C API expands each
Quote, Trade, TradeETH, and Summary subscription into the composite plus 26 regional symbols. Therefore subscription
cardinality is intentionally much larger than the recurring composite event rate shown here.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| graal-stream | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 2.943 | 11.199 (9.264–11.316) | 18.055 | 12.461 | OK |


`Listener coverage median` is recurring events observed by the C++ listener divided by events expected for the
correlated publications. `Listener deficit` is the corresponding observation gap. It is not a transport-loss or
QD-drop counter. In `FEED` mode, the gap may contain TICKER states superseded before listener delivery; endpoint
buffering, `Dropped` records, incomplete publication correlation, and measurement boundaries must also be checked.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
listener coverage.

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `delivery-runs.csv`, `delivery-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| graal-stream | 149232.750 | 0.485 | 1.133% | 0.000 | 0.000 |
| legacy-default | n/a | n/a | n/a | n/a | n/a |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| graal-stream | 149328.200 | 0.121 | 3.370% | 822.000 | 0.000 |
| legacy-default | 148916.200 | 0.175 | 3.376% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
