# API delivery overhead after removing benchmark correlation

This note interprets the repeated
[`20260907T123134Z`](20260907T123134Z/REPORT.md) run. It follows the
[`API-CAPACITY-CONFIRMATION.md`](API-CAPACITY-CONFIRMATION.md) result, which found no client delivery knee before the
current single-publisher limit. The purpose of this experiment is narrower: determine how much CPU and memory in
the full Graal result belongs to this benchmark's timestamp correlation and statistical reporting rather than to
the API delivery path.

## Method

The same synthetic server, 375-symbol shuffled Quote/Trade/TradeETH/Summary workload, 1,500-event publications,
loopback connection, warm-up, measurement duration, and rotating three-run order are used at nominal 150,000,
375,000, and 500,000 events/s. Three fresh client processes are compared:

- **Graal full** uses `STREAM_FEED`, correlates every publication with a `TextMessage` timestamp marker, retains
  per-event latency samples, calculates window distributions, and writes outliers.
- **Graal delivery-only** uses `STREAM_FEED` and the same combined market-event subscription, but its listener only
  inspects event types and increments counters. It retains native vector callback sizes and records CPU/RSS.
- **Legacy delivery-only** uses dxFeed C API 5.11 with its default subscription contract. Its callback receives one
  event at a time and only increments counters.

This is a delivery-path comparison, not a common latency comparison. The legacy API cannot receive the marker used
by the full Graal client. It also uses a much older native/QD stack, so a resource difference between Graal and
legacy cannot by itself identify a regression in one CXX API layer.

## Delivery

| Nominal events/s | Graal delivery-only events/s | Legacy delivery-only events/s | Graal average / maximum callback batch | Legacy callback batch |
|---:|---:|---:|---:|---:|
| 150,000 | 149,322 | 149,087 | 386 / 955 | 1 |
| 375,000 | 368,311 | 368,826 | 404 / 1,057 | 1 |
| 500,000 | 478,840 | 478,519 | 365 / 1,057 | 1 |

Both delivery-only clients track the same achieved source rate. The lower result at nominal 500,000 is common to
both clients and agrees with the previously identified publisher cadence limit; it is not evidence that either
client stopped accepting data. QD reports no dropped records. The full Graal `STREAM_FEED` runs also have 100%
marker-correlated listener coverage at every tested rate.

The callback shapes are intentionally very different. Graal delivers vectors containing hundreds of events on
average, whereas the legacy API invokes its callback once per event. Therefore callback count alone is not a useful
measure of work or throughput.

## Benchmark overhead inside the Graal client

| Nominal events/s | Full CPU, one-core basis | Delivery-only CPU | CPU reduction | Full mean / maximum RSS | Delivery-only mean / maximum RSS |
|---:|---:|---:|---:|---:|---:|
| 150,000 | 25.844% | 23.054% | 10.8% | 137.184 / 198.910 MiB | 94.459 / 99.824 MiB |
| 375,000 | 88.950% | 54.569% | 38.7% | 181.448 / 299.867 MiB | 68.648 / 92.047 MiB |
| 500,000 | 115.871% | 83.047% | 28.3% | 214.377 / 444.734 MiB | 65.286 / 81.332 MiB |

Removing correlation and sample retention materially reduces the full benchmark's cost. The reduction is
particularly clear in maximum RSS: 49.8%, 69.3%, and 81.7% at the three load points. This confirms that the large
memory footprint seen during the earlier stress run was substantially caused by retaining millions of latency
samples and outliers, not by queued undelivered events in the API.

The full client's window statistics are calculated synchronously while callbacks continue. At the two higher
rates, this work can extend a nominal five-second window and reduce the number of RSS samples. CPU is based on total
process CPU time divided by actual elapsed wall time and remains comparable; mean RSS should be read together with
the less sampling-sensitive maximum RSS.

## Remaining Graal-versus-legacy difference

| Nominal events/s | Graal delivery-only CPU | Legacy delivery-only CPU | Graal / legacy CPU ratio | Graal mean RSS | Legacy mean RSS |
|---:|---:|---:|---:|---:|---:|
| 150,000 | 23.054% | 11.591% | 1.99x | 94.459 MiB | 7.264 MiB |
| 375,000 | 54.569% | 22.812% | 2.39x | 68.648 MiB | 7.277 MiB |
| 500,000 | 83.047% | 31.928% | 2.60x | 65.286 MiB | 7.292 MiB |

The measurement harness does not explain the whole process-resource difference: after its expensive correlation
work is removed, the Graal delivery-only process still consumes more CPU and resident memory than the legacy
client. This is a real process-level observation, but it is not yet attributable to a CXX API regression. The two
paths differ in QD/native SDK generation, event conversion and ownership, callback ABI and batching, subscription
semantics, and runtime initialization. The legacy API's one-event callbacks being cheaper overall also shows that
callback count is not the dominant cost in this test.

The next controlled step should run this same delivery-only Graal client against the pinned v5.0.0, v7.0.0, and
v8.0.0 CXX API stacks. That will distinguish a release-stack change from an architectural difference between the
legacy and Graal APIs without reintroducing latency-sample retention. Only after that comparison should source
capacity be raised. If source sharding is added, each base symbol and all its regional records must stay on one
publisher shard; any regional-to-composite relationship must be explicitly modeled rather than created by
independent parallel publishers.

## Relation to the customer observation

The run supports three bounded statements:

1. The new Graal `STREAM_FEED` client delivered every correlated recurring event through the achieved source range;
   no QD drops or client delivery knee were observed.
2. Its latency tail grows gradually with rate: median event p99 is 7.874, 17.904, and 27.868 ms. There is no single
   catastrophic outlier that explains the result; the tail distribution moves as load increases.
3. The original full benchmark overstates API-only CPU and especially memory because it retains and analyzes every
   latency sample. Even after correcting for that, a process-resource difference from legacy remains and requires a
   same-client, cross-release experiment before it can be called a degradation.

This workload reproduces an aggregate event rate, not the causal sequence of a real exchange. Quote, Trade,
TradeETH, and Summary blocks are independently shuffled. A future realism study would need a symbol-local event
model in which a trade may produce zero, one, or several regional quote changes and composite state follows the
chosen regional rules.
