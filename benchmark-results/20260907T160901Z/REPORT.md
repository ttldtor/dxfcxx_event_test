# Customer source-time methodology control

## Experiment definition

- **Objective:** Quantify how a single global strictly increasing Trade timestamp changes sample count and reported latency relative to all observations and an instrument-aware filter.
- **Changed variable:** Endpoint role and native callback batch limit: FEED optimal, FEED one event, and STREAM_FEED optimal.
- **Controls:** Host, CXX API v8.0.0, synthetic server, 375 symbols, 150,000 Q/T/E/S events/s, Trade timestamps, shuffled publication order, warm-up, duration, and monitoring period.
- **Evaluation criteria:** Report acceptance ratios and latency distributions for all Trade/TradeETH observations, strict monotonicity per event-kind and symbol, and the customer's single global strict timestamp filter.
- **Limitations:** The source is synthetic loopback rather than recorded OPRA traffic; it does not reproduce production network or server stalls; the customer's eligibility-time gates and 10-second terminal histogram bin are not modeled because the runner already defines the measurement interval and preserves uncapped latency statistics.

## Source-time measurement methods

Each method is applied to the same Trade and TradeETH callbacks at millisecond timestamp resolution. Values are
medians across repetitions. `customer-global-strict` uses one last-seen timestamp across all event types and symbols;
`per-series-strict` keeps independent state for each event-kind and symbol pair.

| Scenario | Method | Runs | Observations | Accepted | Acceptance | p50 | p99 | p99.9 | Maximum |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| feed-batch-1 | all-trade-events | 3 | 520633 | 520633 | 100.0000% | 7.000 ms | 16.000 ms | 22.000 ms | 48.000 ms |
| feed-batch-1 | customer-global-strict | 3 | 520633 | 2925 | 0.5618% | 3.000 ms | 12.000 ms | 18.000 ms | 21.000 ms |
| feed-batch-1 | per-series-strict | 3 | 520633 | 520633 | 100.0000% | 7.000 ms | 16.000 ms | 22.000 ms | 48.000 ms |
| feed-optimal | all-trade-events | 3 | 2252477 | 2252477 | 100.0000% | 3.000 ms | 9.000 ms | 16.000 ms | 19.000 ms |
| feed-optimal | customer-global-strict | 3 | 2252477 | 3005 | 0.1334% | 2.000 ms | 7.850 ms | 14.003 ms | 18.000 ms |
| feed-optimal | per-series-strict | 3 | 2252477 | 2252477 | 100.0000% | 3.000 ms | 9.000 ms | 16.000 ms | 19.000 ms |
| stream-control | all-trade-events | 3 | 2256000 | 2256000 | 100.0000% | 3.000 ms | 9.000 ms | 16.000 ms | 19.000 ms |
| stream-control | customer-global-strict | 3 | 2256000 | 3008 | 0.1333% | 3.000 ms | 8.000 ms | 14.998 ms | 16.000 ms |
| stream-control | per-series-strict | 3 | 2256000 | 2256000 | 100.0000% | 3.000 ms | 9.000 ms | 16.000 ms | 19.000 ms |


The global strict filter is order-dependent and can retain at most one observation for a group of events carrying
the same millisecond source timestamp. Its latency distribution therefore describes the surviving timestamp maxima,
not the full Trade/TradeETH population. Compare its acceptance ratio with marker-correlated listener coverage before
using its percentile values to assess API behavior.

## Client process resources

These measurements cover the complete client process during its measurement interval. Full Graal includes marker
correlation, latency-sample retention, window statistics, and outlier reporting; delivery-only clients count callback
delivery without retaining per-event latency samples. CPU is normalized both to one logical core and to the host.

| Scenario | Client workload | Runs | Nominal events/s | CPU, one-core median (range) | CPU, host median | RSS mean median | RSS maximum median |
|---|---|---:|---:|---:|---:|---:|---:|
| feed-batch-1 | graal/full | 3 | 150000.000 | 124.581% (124.492–125.211%) | 3.893% | 109.863 MiB | 120.621 MiB |
| feed-optimal | graal/full | 3 | 150000.000 | 37.766% (37.162–39.750%) | 1.180% | 142.485 MiB | 199.438 MiB |
| stream-control | graal/full | 3 | 150000.000 | 34.997% (34.045–35.905%) | 1.094% | 154.341 MiB | 200.109 MiB |


RSS means are arithmetic means of periodic samples; maximum RSS is less sensitive to different sample counts. The
full client calculates each window's distributions synchronously, so at high load that benchmark work can extend a
nominal window while listener callbacks continue. Use QD read/write rates and exact STREAM_FEED integrity alongside
these process measurements.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| feed-batch-1 | feed | 1 | 0.000 ms | 3 | 23.256% | 3436218.000 | 7.566 | 16.403 (16.267–16.873) | 23.348 | 22.201 | OK |
| feed-optimal | feed | optimal | 0.000 ms | 3 | 99.903% | 4377.000 | 3.023 | 9.540 (9.287–9.654) | 15.123 | 11.125 | OK |
| stream-control | stream-feed | optimal | 0.000 ms | 3 | 100.000% | 0.000 | 3.175 | 9.133 (9.021–9.694) | 15.640 | 10.128 | OK |


`Listener coverage median` is recurring events observed by the C++ listener divided by events expected for the
correlated publications. `Listener deficit` is the corresponding observation gap. It is not a transport-loss or
QD-drop counter. In `FEED` mode, the gap may contain TICKER states superseded before listener delivery; endpoint
buffering, `Dropped` records, incomplete publication correlation, and measurement boundaries must also be checked.
Events without a delivered timestamp marker are reported separately as `uncorrelated_events` and excluded from the
listener coverage. Because FEED does not preserve publication boundaries, listener deficit and per-marker excess
are observations rather than integrity failures; STREAM_FEED still requires exact correlated delivery.

Generated files: `latency-runs.csv`, `latency-comparison.csv`, `source-time-method-runs.csv`, `source-time-method-comparison.csv`, `client-resource-runs.csv`, `client-resource-comparison.csv`, `time-series-runs.csv`, `time-series-comparison.csv`, `snapshot-overlap-runs.csv`, `snapshot-overlap-comparison.csv`, `delivery-runs.csv`, `delivery-comparison.csv`, `monitoring.csv`, `monitoring-summary.csv`, and `monitoring-comparison.csv`.

## Client monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Read records/s | Read lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| feed-batch-1 | 148779.250 | 0.335 | 3.930% | 0.000 | 0.000 |
| feed-optimal | 149252.000 | 0.390 | 1.182% | 0.000 | 0.000 |
| stream-control | 149289.000 | 0.433 | 1.067% | 535.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| feed-batch-1 | 148712.800 | 0.129 | 3.397% | 0.000 | 0.000 |
| feed-optimal | 149138.600 | 0.135 | 3.454% | 0.000 | 0.000 |
| stream-control | 149372.800 | 0.114 | 3.320% | 1234.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
