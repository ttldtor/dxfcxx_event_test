# Active regional-symbol fan-out comparison

## Experiment definition

- **Objective:** Determine whether active regional record keys reduce listener delivery throughput or increase latency while aggregate event rate remains fixed.
- **Changed variable:** Zero, one, four, or twenty-six active regional sources for both Graal STREAM_FEED and default legacy C API clients.
- **Controls:** Host, compiler, four event types, 375 base instruments, 1,500 recurring events per 10 ms, shuffled event-type order, warm-up, duration, and monitoring period.
- **Evaluation criteria:** Compare Graal listener coverage and latency, legacy delivery rate and callback shape, server QD metrics, and client CPU and RSS where available.
- **Limitations:** The regional source selected for each base instrument is synthetic; the legacy API always expands base subscriptions to all 26 regional sources; there is no TimeAndSale or historical snapshot traffic; legacy delivery has no common E2E timestamp marker.

## Legacy C API delivery

The legacy C API has no benchmark marker event, so these values describe callback delivery and callback shape; they
are not timestamp-based E2E latency measurements. Rates are medians across repetitions.

| Scenario | Contract | Runs | Nominal events/s | Observed events/s median (range) | Callbacks median | Maximum `data_count` | CPU, one-core basis | CPU, host basis | RSS mean / maximum |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| legacy-composite | default | 3 | 150000.000 | 148742.201 (148710.983–149201.394) | 8925000.000 | 1 | 10.443% | 0.326% | 7.238 MiB / 7.238 MiB |
| legacy-region-1 | default | 3 | 150000.000 | 148800.236 (148570.368–148914.952) | 8932500.000 | 1 | 10.825% | 0.338% | 7.234 MiB / 7.234 MiB |
| legacy-region-26 | default | 3 | 150000.000 | 148762.095 (148698.322–148834.160) | 8932500.000 | 1 | 11.474% | 0.359% | 7.246 MiB / 7.246 MiB |
| legacy-region-4 | default | 3 | 150000.000 | 148803.494 (148505.219–149402.329) | 8937000.000 | 1 | 9.522% | 0.298% | 7.262 MiB / 7.262 MiB |


With the default legacy contract, the C API expands each base-symbol Quote, Trade, TradeETH, and Summary subscription
into the composite plus 26 regional symbols. A task can publish a configured subset of those regional record keys
while keeping its recurring event rate fixed. CPU uses both a one-core basis and a host-normalized basis; RSS is
sampled by the cross-platform `ttldtor/Process` library during the measurement interval.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| graal-composite | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 2.502 | 9.267 (9.077–9.846) | 17.008 | 9.975 | OK |
| graal-region-1 | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.132 | 12.246 (11.058–13.544) | 21.425 | 13.712 | OK |
| graal-region-26 | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.741 | 14.100 (13.068–14.828) | 23.053 | 15.788 | OK |
| graal-region-4 | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.570 | 12.309 (10.920–12.828) | 19.586 | 14.081 | OK |


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
| graal-composite | 149874.500 | 0.413 | 0.975% | 560.000 | 0.000 |
| graal-region-1 | 149341.250 | 0.503 | 1.137% | 382.000 | 0.000 |
| graal-region-26 | 148625.750 | 0.701 | 1.342% | 492.000 | 0.000 |
| graal-region-4 | 149341.750 | 0.619 | 1.286% | 277.000 | 0.000 |
| legacy-composite | n/a | n/a | n/a | n/a | n/a |
| legacy-region-1 | n/a | n/a | n/a | n/a | n/a |
| legacy-region-26 | n/a | n/a | n/a | n/a | n/a |
| legacy-region-4 | n/a | n/a | n/a | n/a | n/a |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| graal-composite | 149895.600 | 0.084 | 3.312% | 928.000 | 0.000 |
| graal-region-1 | 149351.800 | 0.116 | 3.350% | 901.000 | 0.000 |
| graal-region-26 | 148664.400 | 0.122 | 3.344% | 658.000 | 0.000 |
| graal-region-4 | 149169.200 | 0.123 | 3.300% | 1216.000 | 0.000 |
| legacy-composite | 148895.400 | 0.178 | 3.340% | 0.000 | 0.000 |
| legacy-region-1 | 148862.400 | 0.184 | 3.372% | 0.000 | 0.000 |
| legacy-region-26 | 148817.800 | 0.195 | 3.386% | 0.000 | 0.000 |
| legacy-region-4 | 148772.400 | 0.194 | 3.380% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
