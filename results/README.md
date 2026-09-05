# Measurements

Everything here is produced by `scripts/measure.sh` and carries the output of
`scripts/measure-env.sh capture` beside it. Each experiment directory holds a
`summary.txt`, an `env.txt`, and the raw gateway and probe logs for every run.

## Method

- **Source.** The synthetic in-process generator, not vcan. The offered rate is
  then a knob rather than a property of a driver, and the loopback's own
  behaviour is not folded into a measurement of the pipeline. The vcan path is
  covered separately by `scripts/smoke-vcan.sh`.
- **Latency.** `now - frame.t_ingest_ns`, both `CLOCK_MONOTONIC`, taken when the
  consumer's `read()` returns. It covers everything the gateway is responsible
  for: fan-out or ring publish, queueing, encoding, and the TCP write.
- **Warm-up.** The first seconds of samples are discarded. A consumer that
  connects to a running gateway finds a backlog waiting, and those frames were
  queued before it existed — their measured latency is the age of the buffer,
  not the behaviour of the pipeline. Without this the tail is
  `queue_depth / rate` and says nothing about the system.
- **Governor.** Runs are taken with `measure-env.sh perf-on`. It is not made
  permanent: an always-performance board runs hotter and reaches its thermal
  knee sooner, which would change the very throttling behaviour worth measuring.

## What these numbers are not

- **Not cross-machine.** Both timestamps are `CLOCK_MONOTONIC` on one host.
  Across machines those clocks share no epoch and the difference is meaningless.
  The split deployment uses `chrony` and is reported separately.
- **Not a bus.** vcan has no bit timing, no arbitration and no bus-off, and the
  synthetic source has no rate ceiling at all. Offered rates here go far above
  what a 500 kbit/s CAN bus can carry (roughly 4000 frames/s), and they are
  chosen to find the pipeline's limits, not to model a vehicle.
- **Not the network.** The consumer is on the same host, so the TCP path is
  loopback. The Pi's Ethernet is USB-attached with a realistic ceiling around
  200–230 Mbit/s, and its WiFi is worse and far more variable; neither is in
  these figures.

## Reading a latency tail

A tail that is much larger than the median usually means the consumer briefly
stopped draining, not that the pipeline stalled. The gateway's own counters in
`gateway.log` say which: `would_block` is back-pressure from the socket,
`dropped` is the consumer's queue or ring cells being overwritten, and
`lingered` is batches that waited to coalesce on purpose.

## Findings

Raspberry Pi 3 B+, aarch64, GCC 14.2, governor `performance`, one consumer,
synthetic source. Full provenance in each experiment's `env.txt`.

### The median is flat; only the tail moves

| rate | p50 | p99 | p99.9 |
|---|---|---|---|
| 2 000/s | 64.6 µs | 74.1 µs | 172.6 µs |
| 20 000/s | 61.4 µs | 428.4 µs | 12.6 ms |
| 100 000/s | 68.5 µs | 77.4 ms | 110.8 ms |

Across a fiftyfold change in offered rate the median moves by 7 µs. What
degrades is the tail, and it degrades by five orders of magnitude. Both
topologies behave the same way here.

Four 500 kbit/s CAN buses produce on the order of 16 000 frames/s, so the
middle row is already well past anything this gateway would see in the role it
was built for.

### Batching is not a monotonic trade on this hardware

| linger | frames/batch | p50 | p99 | write syscalls/s |
|---|---|---|---|---|
| 0 | 1.0 | 62.0 µs | **1407.0 µs** | 20 000 |
| 100 µs | 4.1 | 123.7 µs | **376.8 µs** | 4 878 |
| 250 µs | 7.1 | 214.2 µs | 401.0 µs | 2 817 |
| 1000 µs | 22.7 | 603.9 µs | 1138.4 µs | 881 |
| 5000 µs | 103.7 | 2616.0 µs | 5104.5 µs | 193 |

The expected shape is that waiting to coalesce buys throughput and costs tail
latency. On the development machine it does exactly that, monotonically: p99
rises from 107 µs at linger 0 through 246 µs, 378 µs and upward.

On the Pi the curve turns over. A linger of 100 µs makes the tail **3.7x
better**, not worse. Twenty thousand write syscalls a second is itself the
dominant source of tail latency on four 1.4 GHz cores, and cutting it fourfold
more than pays for the delay introduced. The optimum here is somewhere around
100–250 µs; on x86 it is zero.

This is the clearest argument in the project for measuring on the target rather
than on the machine the code was written on. The same sweep on the development
machine would have supported the opposite recommendation.

### Under overload, more loss means lower latency

| offered | delivered | lost | loss | p99 |
|---|---|---|---|---|
| 5 000/s | 48 760 | 0 | 0.00% | 1227 ms |
| 20 000/s | 108 179 | 62 185 | 36.50% | 2223 ms |
| 50 000/s | 130 612 | 349 764 | 72.81% | 1367 ms |
| 200 000/s | 359 964 | 1 634 792 | 81.95% | **635 ms** |

Loss rises with load, as expected. Latency does not: it peaks at 20 000/s and
then *falls* as loss increases. That is drop-oldest working as designed — the
more aggressively stale frames are discarded, the younger the surviving ones
are when they arrive. A pipeline that buffered instead of dropping would
deliver everything, eventually, and every frame would be worthless by then.

The 5 000/s row is the one to read carefully: zero loss and 1.2 seconds of
latency. Nothing was dropped because an 8192-deep buffer at that rate holds
1.6 seconds of traffic, so the delay is the buffer, not the pipeline.

### The topologies differ only when there is more than one bus

| topology | sources | sent | dropped | delivered |
|---|---|---|---|---|
| queue | 1 | 147 733 | 144 334 | 50.6% |
| ring | 1 | 145 212 | 129 886 | 52.8% |
| queue | 4 | 265 645 | 927 012 | **22.3%** |
| ring | 4 | 468 905 | 621 642 | **43.0%** |

With one source the two are indistinguishable, which is the honest result: the
ring's machinery buys nothing when there is one producer and no contention.
With four, the ring delivers nearly twice as much of the same offered load.

Topology A is not slow by accident. Order in a queue is push order, so it cannot
spread ingest across threads without handing a consumer sequences out of order —
its single fan-out thread is forced by the ordering guarantee, and it then has
to copy every frame once per consumer. The ring takes its order from the cell
position and needs neither.

## Caveat that applies to every number above

`throttled_meaning=soft-temp-limit-occurred` appears in the provenance of these
runs. The board was clean (`0x0`) before them and reached its soft temperature
limit during them — 49 °C idle, 57 °C after — at which point a Pi 3 B+ drops
from 1400 MHz to 1200 MHz.

So an unknown fraction of this data was taken on a thermally capped CPU. The
direction of every finding above survives it, because the comparisons are
between runs on the same board minutes apart, but the absolute figures are not
a clean 1400 MHz measurement.

This is exactly why the environment is captured beside the numbers rather than
assumed. Fixing it properly means a heatsink, and quantifying it means a
sustained run with a temperature trace alongside throughput — which is its own
experiment, not a footnote to this one.

## Split deployment: gateway on the Pi, consumer on another machine

Gateway on the Pi (192.168.1.103, wired), probe on the development machine
(192.168.1.102), 20 000 frames/s, ring topology, 100 µs linger.

### The link first, because otherwise the gateway gets the credit or the blame

`scripts/link-ceiling.py`, 48 MB each way plus 500 round trips:

| | |
|---|---|
| upload | 94.1 Mbit/s |
| download | 93.8 Mbit/s |
| RTT p50 | 518 µs |
| RTT p99 | 2294 µs |
| RTT max | 12219 µs |

94 Mbit/s is line rate for 100BASE-TX, and **the cap is the development
machine's adapter**, which negotiated 100 Mbps on a gigabit card — not the Pi's
USB-attached Ethernet, whose ceiling is roughly twice that. At 56 bytes per
frame on the wire the link carries about 210 000 frames/s, well above anything
these experiments offer it.

The RTT half is the more important number: **half the median round trip is
259 µs**, and the gateway's own latency on the Pi is 60–70 µs. The network path
is four times larger than the thing being measured.

### What the split run shows

| | |
|---|---|
| delivered | 608 172 frames in 30.0 s = 20 272/s |
| loss | 0 |
| silent jumps | 0 |
| protocol errors | 0 |
| mean batch | 4.1 frames |

Zero loss across a real network to a consumer on another machine, sustained for
thirty seconds. That is the distributed claim, and it is clock-independent.

### Cross-machine one-way latency is not measurable on this pair

Two independent reasons, both quantified:

**The consumer's clock is not disciplined.** `chronyc sources` on the
development machine shows `Reach 0` against every server, `Reference ID
00000000`, stratum 0 and a reference time of 1 January 1970 — chrony has never
completed a single exchange there, and the clock comes from the Windows host
through the hypervisor. The Pi's chrony is genuinely synchronised: four sources,
reach 377, offsets of a few milliseconds. Measured directly, the two
`CLOCK_REALTIME` clocks differ by **1.19 seconds**.

**Even a perfect clock would not help.** Half the median round trip is 259 µs
against a 60–70 µs signal.

Attempting it anyway is instructive. Using `CLOCK_MONOTONIC` across the pair
reported a minimum of **−741 seconds**, which is the difference between two boot
times and nothing else. The probe reports the minimum rather than clamping it
for exactly this reason, and it worked: the mistake was unmissable rather than
plausible. Switching to `CLOCK_REALTIME` with `--cross` narrowed it to −1.05
seconds, which is the real clock offset and still not a latency.

**So the honest decomposition is to measure the two halves separately**: the
gateway's own latency on one machine in a single `CLOCK_MONOTONIC` domain, and
the network's contribution as half the round trip from `link-ceiling.py`. Adding
them is defensible; subtracting two undisciplined clocks is not.

This is the outcome the scoping session argued about. `chrony` was chosen over
RTT/2 for cross-machine work; the measurement says RTT/2 was the right call for
this pair, and the reason has nothing to do with chrony itself.

## Cross-machine latency, measured the way that works

The one-way attempt failed for reasons recorded above: one host's clock was
never disciplined, and the network path is four times the signal anyway. Wire
version 3 adds an in-band round-trip probe instead. The gateway emits one
periodically, the consumer returns it unchanged, and the gateway times it with
its own clock from send to return — there is no second clock to disagree with.

Gateway on the Pi, consumer on the development machine, 20 000 frames/s over
wired Ethernet, 30 seconds:

| | idle side-channel | in-band probe |
|---|---|---|
| RTT p50 | 518 µs | **524.6 µs** |
| RTT p99 | 2294 µs | **25 460 µs** |
| RTT max | 12 219 µs | **72 182 µs** |

The two mechanisms share no code — one is a standalone Python TCP echo
(`link-ceiling.py`), the other a C++ record travelling through the whole egress
path — and their medians agree to within six microseconds. That agreement is
the cross-check that neither is measuring something else.

Their tails differ by a factor of eleven, and that difference is the finding. A
probe on an idle socket measures the network. A probe queued behind real traffic,
through the same batching and the same socket buffer, measures what a frame
actually experiences. Reporting the first as if it were the second would
understate the tail by an order of magnitude.

The run itself: 608 174 frames delivered in 30 seconds at 20 272/s, zero loss,
zero silent jumps, zero protocol errors, and 1414 of 1452 probes returned. The
38 that did not were dropped by the consumer rather than waited on — answering a
measurement probe must not block a consumer's read loop, because that would
distort the thing being measured.

**One-way p50 is therefore about 262 µs**, halved from the round trip, assuming
a symmetric path and including the consumer's turnaround. The gateway's own
contribution — 60–70 µs, measured locally in a single clock domain — is a small
part of it. The two halves are measured separately and added, which is
defensible; subtracting two clocks that disagree by 1.19 seconds is not.

## Does SCHED_FIFO help a pipeline the way it helped a periodic loop?

The supervisor gained roughly 570x lower p99 tick jitter from SCHED_FIFO,
`mlockall` and an isolated core. That was a single-threaded loop whose whole job
was to wake on a 10 ms timer, so being scheduled late *was* the error. This
pipeline is a different shape, and the question is worth measuring rather than
assuming in either direction.

Three arms at 20 000 frames/s on the ring, `./scripts/measure.sh sched`. The two
loaded arms run one busy loop per core at ordinary priority; without contention
every policy looks identical and the experiment answers nothing. Median of three
runs per percentile, microseconds:

| arm | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| idle, `SCHED_OTHER` | 126.4 | 213.3 | 1 800.8 | 17 227.6 | 21 926.8 |
| loaded, `SCHED_OTHER` | 227.0 | 2 397.4 | **25 608.5** | 49 548.2 | 55 234.2 |
| loaded, `SCHED_FIFO` 20 | 124.1 | 177.1 | **2 427.2** | 17 992.5 | 24 000.5 |

No frames were lost in any arm of any run.

Contention costs 14x at p99. `SCHED_FIFO` gives back 10.6x of it, and restores
p50 and p90 completely — a loaded pipeline at real-time priority has a *lower*
p90 than an idle one at ordinary priority, 177 µs against 213 µs. That inversion
is not noise and not a win: the governor is `ondemand`, so on an idle board it
drops the clock and lets cores enter idle states, and every wake-up then pays to
come back. The busy loops hold the frequency up. Load helps the median exactly
because the board is otherwise busy standing down.

### What it does not fix

p99.9 and max barely move: 17 993 µs and 24 001 µs under `SCHED_FIFO` against
17 228 µs and 21 927 µs idle. Whatever produces those is not CPU contention,
because removing the contention does not remove them and neither does
out-prioritising it. The obvious suspect is that nothing here calls `mlockall`,
which the supervisor did — an RT thread that takes a page fault waits for the
kernel regardless of its priority. That is the next thing to test, and until it
is tested it stays a hypothesis rather than an explanation.

So the honest summary is narrower than the supervisor's headline: `SCHED_FIFO`
protects the *body* of the distribution under contention and does nothing for
its extreme tail.

### The more useful finding is the spread

Across the three runs, the real-time arm reproduces almost exactly — p50 within
0.1 µs, p90 within 1.0 µs, p99 within 140 µs. The contended ordinary arm does
not: its p90 came out 327.8, 2 397.4 and 3 418.2 µs, a tenfold spread across
identical runs, because which thread the scheduler starves is a different
accident each time.

For a failsafe runtime that is arguably the point. A tail that is ten times
lower is worth having; a tail that is the *same number twice* is what makes a
deadline something you can argue for.

### Reading these numbers

- Four busy loops were started, one per `nproc`, but `isolcpus=3` keeps core 3
  out of the scheduler's reach and nothing here sets affinity. So four hogs and
  the whole pipeline actually contended for three cores, and one core sat idle
  throughout. The contention is therefore heavier than "one loop per core"
  suggests.
- The probe runs at the same priority as the gateway in every arm, so these
  figures include the measuring instrument's own scheduling delay and bound the
  pipeline from above rather than isolating it.
- Nothing is pinned. One isolated core suits a single-threaded periodic loop;
  confining several ingest and egress threads to one core would serialise work
  meant to overlap.
- `throttled=0x80000` (`soft-temp-limit-occurred`) is latched since boot, not a
  statement about these runs; the die was at 54.8 °C.
