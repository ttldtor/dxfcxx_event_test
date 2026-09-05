# Event notification batch-limit benchmark

This benchmark keeps the shuffled 375-symbol, 1,500-event, 10 ms workload fixed and changes only the maximum number
of events passed in one native subscription notification. Five `FEED` limits are compared with an `optimal`
`STREAM_FEED` control. Each scenario has three independent one-minute measurements.

## Results

| Scenario | Listener coverage median | Listener deficit median | Market callbacks median | Callback size p50 | Callback size p99 | Callback duration p50 | Client read records/s | QD dropped |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| FEED optimal | 99.839% | 14,821 | 22,041 | 297 | 876 | 132.2 us | 149,672 | 0 |
| FEED limit 1 | 26.121% | 6,605,927 | 2,337,494 | 1 | 1 | 1.1 us | 149,439 | 0 |
| FEED limit 375 | 99.871% | 12,375 | 30,129 | 297 | 375 | 99.3 us | 149,847 | 0 |
| FEED limit 1,500 | 99.914% | 8,491 | 21,194 | 342.5 | 876 | 122.2 us | 149,934 | 0 |
| FEED maximum | 99.868% | 12,770 | 19,420 | 543 | 871 | 124.8 us | 149,690 | 0 |
| STREAM_FEED optimal | 100.000% | 0 | 19,294 | 570 | 898 | 136.4 us | n/a | 0 |

Values are medians of three repetitions. `Market callbacks` excludes the separate control-marker subscription.
`STREAM_FEED` client monitoring remains unavailable in the current QD log format; its exact listener accounting is
complete and the server writes approximately 150,036 records/s.

## Interpretation

The limit-one run is the decisive localization result. It forces one native callback per listener-visible event.
The listener can process only approximately 2.1–3.3 million events during each one-minute measurement, while the
connector continues reading approximately 149,439 records/s and the server writes at the same rate. Client and
server endpoint buffers remain zero and neither endpoint reports dropped records.

The missing listener observations therefore do not originate in the network or in a transport receive backlog.
They are intermediate `TICKER` states superseded on the client after connector input has been read but before the
native notification callback can expose every state to the C++ listener. The callback limit does not create a QD
drop; it reduces notification throughput, which makes normal `FEED` conflation much more visible.

The non-stress limits do not show a monotonic coverage relationship. Limits 375, 1,500, maximum, and the default all
retain between 99.839% and 99.914% in their median runs. The 1,500 result has the smallest median deficit, but the
differences are comparable to run-to-run variation and do not establish it as a mitigation. The maximum setting is
effectively bounded by this workload: observed callbacks never exceed the 1,500-event publication size.

The C++ wrapper maps every element of the native `dxfg_event_type_list` into one C++ event before synchronously
invoking the application handler. Consequently, the recorded callback sizes bracket the output of Graal Native SDK
and the input of the user's listener. Together with the connector counters, the limit-one result places the
supersession before that native callback rather than inside the user's event loop.

`STREAM_FEED` again delivers every recurring update. This confirms that the observed difference is the endpoint
contract: STREAM queues updates, whereas FEED retains current TICKER state and may replace an intermediate value
before notification.

## Scope

- This test localizes the behavior to client-side FEED processing between connector read and native listener
  notification. It does not identify the exact QD class or queue transition responsible for scheduling callbacks.
- Limit one is deliberately pathological and is not a recommended production setting.
- Callback duration measures entry-to-return time for the benchmark's complete market-event handler. It includes an
  optional configured listener delay, although this suite uses zero delay.
- Exact record-by-record tracing inside the prebuilt Graal native image would require an instrumented SDK build.

Further black-box batch-limit testing is unlikely to improve localization. The next investigation should trace the
QD FEED notification path in source or use an instrumented Graal Native SDK build to name the precise retention and
notification components.
