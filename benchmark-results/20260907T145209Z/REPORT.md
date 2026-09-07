# Inactive subscription-cardinality API comparison

## Experiment definition

- **Objective:** Determine how a larger mostly idle symbol universe affects the Graal CXX and legacy C API delivery paths independently of active event throughput.
- **Changed variable:** Subscribed base-symbol cardinality: 375, 3,750, or 10,000 symbols, with only the first 375 receiving recurring events.
- **Controls:** Host, compiler, v8 synthetic server, 375 active base symbols, 150,000 composite Q/T/E/S events/s, shuffled publication order, warm-up, duration, monitoring period, and rotating run order.
- **Evaluation criteria:** Compare startup completion, observed delivery rate, callback shape, QD subscription and storage state, drops, buffers, CPU, and RSS for Graal STREAM_FEED and legacy default clients.
- **Limitations:** This is not an E2E latency comparison; legacy 5.11 expands market subscriptions to composite and regional records internally; initial Profile state is delivered before measurement; the full production topology and customer's active-symbol distribution remain unknown.

## Delivery-only client results

These values describe callback delivery and callback shape without timestamp correlation or per-event sample
allocation. They are not timestamp-based E2E latency measurements. Rates are medians across repetitions.

| Scenario | Client | Contract / role | Runs | Nominal events/s | Observed events/s median (range) | Callbacks median | Maximum callback batch | CPU, one-core basis | CPU, host basis | RSS mean / maximum |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 10000-symbols-graal | graal | stream-feed | 3 | 150000.000 | 149129.301 (148849.748–149141.325) | 15827.000 | 958 | 31.068% | 0.971% | 131.574 MiB / 134.379 MiB |
| 10000-symbols-legacy | legacy | default | 3 | 150000.000 | 149488.377 (149338.206–149530.194) | 4486500.000 | 1 | 12.292% | 0.384% | 36.797 MiB / 36.832 MiB |
| 375-symbols-graal | graal | stream-feed | 3 | 150000.000 | 149191.473 (149013.832–149563.543) | 12478.000 | 935 | 26.952% | 0.842% | 96.212 MiB / 101.258 MiB |
| 375-symbols-legacy | legacy | default | 3 | 150000.000 | 148994.535 (148751.205–149028.962) | 4477167.000 | 1 | 12.426% | 0.388% | 7.295 MiB / 7.324 MiB |
| 3750-symbols-graal | graal | stream-feed | 3 | 150000.000 | 149085.358 (148857.635–149425.343) | 15492.000 | 941 | 31.664% | 0.989% | 97.604 MiB / 103.195 MiB |
| 3750-symbols-legacy | legacy | default | 3 | 150000.000 | 149744.225 (149631.105–149759.306) | 4501500.000 | 1 | 12.591% | 0.393% | 17.979 MiB / 18.008 MiB |


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
| 10000-symbols-graal | graal/delivery-only | 3 | 150000.000 | 31.068% (30.051–32.401%) | 0.971% | 131.574 MiB | 134.379 MiB |
| 10000-symbols-legacy | legacy/delivery-only | 3 | 150000.000 | 12.292% (11.333–12.333%) | 0.384% | 36.797 MiB | 36.832 MiB |
| 375-symbols-graal | graal/delivery-only | 3 | 150000.000 | 26.952% (24.175–30.891%) | 0.842% | 96.212 MiB | 101.258 MiB |
| 375-symbols-legacy | legacy/delivery-only | 3 | 150000.000 | 12.426% (10.764–13.003%) | 0.388% | 7.295 MiB | 7.324 MiB |
| 3750-symbols-graal | graal/delivery-only | 3 | 150000.000 | 31.664% (29.113–32.698%) | 0.989% | 97.604 MiB | 103.195 MiB |
| 3750-symbols-legacy | legacy/delivery-only | 3 | 150000.000 | 12.591% (10.811–13.048%) | 0.393% | 17.979 MiB | 18.008 MiB |


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
| 10000-symbols-graal | 148937.000 | 0.559 | 0.998% | 269.000 | 0.000 |
| 10000-symbols-legacy | n/a | n/a | n/a | n/a | n/a |
| 375-symbols-graal | 149388.000 | 0.493 | 0.852% | 581.000 | 0.000 |
| 375-symbols-legacy | n/a | n/a | n/a | n/a | n/a |
| 3750-symbols-graal | 148879.600 | 0.555 | 0.966% | 69.000 | 0.000 |
| 3750-symbols-legacy | n/a | n/a | n/a | n/a | n/a |


## Server monitoring

The table shows medians across repetitions. Lag is in milliseconds; dropped is the largest per-run sum and buffer is the largest per-run high-water mark.

| Scenario | Write records/s | Write lag | CPU | Maximum buffer | Maximum dropped |
|---|---:|---:|---:|---:|---:|
| 10000-symbols-graal | 148937.000 | 0.128 | 3.344% | 0.000 | 0.000 |
| 10000-symbols-legacy | 149422.600 | 0.192 | 3.440% | 0.000 | 0.000 |
| 375-symbols-graal | 149411.600 | 0.134 | 3.392% | 1224.000 | 0.000 |
| 375-symbols-legacy | 148968.000 | 0.184 | 3.346% | 0.000 | 0.000 |
| 3750-symbols-graal | 148865.800 | 0.127 | 3.358% | 0.000 | 0.000 |
| 3750-symbols-legacy | 149640.200 | 0.177 | 3.424% | 0.000 | 0.000 |


`Dropped = 0` rules out drops counted by the corresponding QD endpoint, but it does not rule out normal FEED conflation.
The current measurements cannot locate TICKER supersession on the publisher or feed side. A monitoring value of
`n/a` means that the endpoint log emitted no parseable sample; it must not be interpreted as zero. When both rates
are available, similar server write and client read rates make transport loss unlikely, but they are interval
averages rather than a record-by-record audit.
Low average CPU, buffer, and network utilization also do not exclude conflation: a short burst only has to overtake
listener processing for the same record and symbol before the next monitoring sample.
