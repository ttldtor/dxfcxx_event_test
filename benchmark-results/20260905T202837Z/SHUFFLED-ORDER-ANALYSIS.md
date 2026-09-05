# Shuffled event-type order benchmark

This benchmark keeps the 375-symbol, 1,500-event, 10 ms `FEED` workload unchanged and uses seed `22805` to
deterministically shuffle only the four event-type blocks before every publication. It is a control for the
last-block effect observed by the fixed-order benchmark.

## Results

| Event type | Median listener deficit | Median listener coverage |
|---|---:|---:|
| Quote | 4,542 | 99.81% |
| Trade | 4,523 | 99.81% |
| TradeETH | 4,903 | 99.79% |
| Summary | 3,841 | 99.83% |

The largest and smallest per-type median deficits differ by approximately 28%. In the fixed-order experiment, the
type deliberately placed last had the largest deficit in every permutation. Grouping the fixed-order results by
block position gives median deficits of 2,724, 2,713, 3,103, and 4,539 for positions one through four respectively.
The last position therefore has approximately 67% more deficit than the first two positions.

The complete shuffled run has balanced last-block counts:

| Repetition | Quote | Trade | TradeETH | Summary |
|---|---:|---:|---:|---:|
| 1 | 2,298 | 2,289 | 2,235 | 2,231 |
| 2 | 2,284 | 2,271 | 2,217 | 2,214 |
| 3 | 2,296 | 2,286 | 2,233 | 2,229 |

All three repetitions passed. The overall median listener coverage is 99.808%, with a median deficit of 17,809.
Server write and client read rates remain close to 150,000 records/s, endpoint buffers remain zero, and both QD
endpoints report zero dropped records.

## Interpretation

The shuffled control confirms that the asymmetry is caused mainly by serialization or processing position rather
than by a special limitation of `Summary`. Once all four types occupy the last position approximately equally often,
their long-run deficits become much more similar.

Shuffling is a diagnostic control, not a mitigation. It distributes the listener deficit between event types but
does not reduce the total deficit. The shuffled run's median total deficit is actually higher than the medians of
the four fixed-order scenarios, although three repetitions are insufficient to interpret that difference as a
causal shuffle penalty.

No measurable generator overhead was introduced. Median event-preparation time is 0.192 ms with shuffling versus
0.213 ms across the fixed-order runs. This small difference is ordinary run variation and should not be interpreted
as an improvement; it shows only that shuffling four indices is below the benchmark's measurement sensitivity.

Together with the role, listener-delay, and symbol-cardinality controls, this supports a per-record-key `FEED`
supersession mechanism. A newly published state can overtake listener delivery of the previous state even when the
transport keeps reading records, QD reports no drops, and average CPU and endpoint buffers remain low.

## Scope and next step

- Only the four contiguous event-type blocks are shuffled. Individual events within a block remain in symbol order.
- The publication marker remains separate from the market-event subscription, so boundary reordering can still
  produce small `excess_events` values and `CHECK` integrity status.
- The test identifies a position-dependent listener-observation effect but does not by itself locate the exact
  publisher-side or feed-side code path where a state is superseded.

Further black-box randomization is unlikely to add much. The next useful step is targeted instrumentation or source
tracing around the QD-to-event-listener boundary to identify where an accepted `TICKER` record ceases to correspond
one-to-one with a C++ listener observation.
