# Graal delivery-only release-stack control

## Experiment definition

- **Objective:** Determine whether process delivery cost changed between pinned CXX API, Graal Native SDK, and QD release stacks without benchmark timestamp correlation.
- **Changed variable:** The separately configured v5.0.0, v7.0.0, or v8.0.0 release stack used by both the synthetic server and the Graal delivery-only client.
- **Controls:** Host, compiler, client callback code, STREAM_FEED role, 375-symbol shuffled Q/T/E/S workload, 1,500 events per publication, warm-up, duration, monitoring period, and rotating run order.
- **Evaluation criteria:** Compare achieved delivery rate, callback shape, QD drops, CPU, and RSS at the confirmed baseline, pre-knee, and publisher-knee points.
- **Limitations:** Each stack also changes the server-side publisher and embedded QD version; the 500,000-events/s point approaches publisher capacity; the fixed independent event mix is not a causal market-microstructure model.

## Delivery-only client results

These values describe callback delivery and callback shape without timestamp correlation or per-event sample
allocation. They are not timestamp-based E2E latency measurements. Rates are medians across repetitions.

| Scenario | Client | Contract / role | Runs | Nominal events/s | Observed events/s median (range) | Callbacks median | Maximum callback batch | CPU, one-core basis | CPU, host basis | RSS mean / maximum |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 150k-graal-delivery | graal | stream-feed | 3 | 150000.000 | 149392.747 (149353.039–149600.469) | 12227.000 | 954 | 26.038% | 0.814% | 95.727 MiB / 100.438 MiB |
| 375k-graal-delivery | graal | stream-feed | 3 | 375000.000 | 368443.838 (367854.719–368519.956) | 31277.000 | 1044 | 64.632% | 2.020% | 68.183 MiB / 91.934 MiB |
| 500k-graal-delivery | graal | stream-feed | 3 | 500000.000 | 480284.516 (479159.279–485019.767) | 40992.000 | 1057 | 83.594% | 2.612% | 66.331 MiB / 84.016 MiB |


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
| 150k-graal-delivery | graal/delivery-only | 3 | 150000.000 | 26.038% (25.295–27.342%) | 0.814% | 95.727 MiB | 100.438 MiB |
| 375k-graal-delivery | graal/delivery-only | 3 | 375000.000 | 64.632% (62.480–70.362%) | 2.020% | 68.183 MiB | 91.934 MiB |
| 500k-graal-delivery | graal/delivery-only | 3 | 500000.000 | 83.594% (82.759–84.830%) | 2.612% | 66.331 MiB | 84.016 MiB |


RSS means are arithmetic means of periodic samples; maximum RSS is less sensitive to different sample counts. The
full client calculates each window's distributions synchronously, so at high load that benchmark work can extend a
nominal window while listener callbacks continue. Use QD read/write rates and exact STREAM_FEED integrity alongside
these process measurements.

## Results

Run-level values are aggregated using the median; the range shows the minimum and maximum across independent repetitions. Latencies are in milliseconds.

| Scenario | Role | Batch limit | Aggregation | Runs | Listener coverage median | Listener deficit median | Event p50 median | Event p99 median (range) | Event p99.9 median | Batch p99 median | Integrity |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|


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
| 150k-graal-delivery | 149447.400 | 0.440 | 0.810% | 318.000 | 0.000 |
| 375k-graal-delivery | 368525.800 | 0.544 | 2.012% | 589.000 | 0.000 |
| 500k-graal-delivery | 478148.600 | 0.518 | 2.640% | 297.000 | 0.000 |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 150k-graal-delivery | 149447.400 | 0.132 | 3.424% | 174.000 | 0.000 |
| 375k-graal-delivery | 368521.600 | 0.129 | 3.806% | 1359.000 | 0.000 |
| 500k-graal-delivery | 478123.400 | 0.127 | 3.992% | 1350.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
