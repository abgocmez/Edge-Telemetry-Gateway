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

## A correction, and what it cost

Every tail figure below was measured twice. The first set was wrong, and the
reason is worth more than the numbers.

`etg-probe` printed a progress line once a second, and to get the median for it
it called `Samples::compute`, which copies and sorts everything retained so far.
That call sat in the read loop being measured, and the retained set grew for the
whole run. So the instrument stalled itself, for longer the longer it ran, and
every stall landed in the tail it was reporting. At 20 000 frames/s a twelve
second run sorted 240 000 samples once a second and reported max 21 ms; a
forty-five second run sorted 900 000 and reported 124 ms, from the same pipeline
under the same load.

It survived this long because nothing ran long enough to make it obvious. It
surfaced when the thermal trace used 45-second blocks and returned p99 of 86 ms
where the scheduling experiment had 2.4 ms at the same rate on the same board.
Before changing anything it was isolated by elimination: not temperature (the
first block was cool and already showed it), not the governor (pinning
`performance` changed nothing), not scheduling, not paging, and not a backlog --
delivery held at exactly 20 000/s throughout and the median frame was never
late. Duration was the only variable that moved it.

With the fix, at 20 000/s over 45 seconds: p99 73 292 µs to 228 µs, p99.9
109 057 µs to 278 µs. Twelve-second and forty-five-second runs now agree, which
is the property that was missing.

Medians were never affected. Two conclusions drawn from the bad tails were, and
both are retracted where they appear below: that batching improves the tail on
this hardware, and that `SCHED_FIFO` cannot help the extreme tail.

## Findings

Raspberry Pi 3 B+, aarch64, GCC 14.2, governor `ondemand`, one consumer,
synthetic source. Full provenance in each experiment's `env.txt`.

### The median is flat until the pipeline runs out of room

| rate | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| 2 000/s | 114.8 µs | 125.3 µs | 144.3 µs | 214.5 µs | 4 608.8 µs |
| 20 000/s | 65.9 µs | 72.2 µs | 118.2 µs | 163.0 µs | 2 835.3 µs |
| 100 000/s | 61.7 µs | 79.6 µs | 1 690.9 µs | 4 586.1 µs | 11 310.9 µs |

Both topologies behave the same way here, within noise.

The 2 000/s row is slower than the 20 000/s row at every percentile, which looks
backwards and is not. The governor is `ondemand`: at 2 000 frames/s the board is
nearly idle, so it drops the clock and lets cores enter idle states, and every
wake-up pays to come back. Ten times the work arrives *sooner* because the
machine is already awake. The same effect shows up again under contention in the
scheduling experiment, where a loaded pipeline beats an idle one.

Only at 100 000/s does a real tail appear, and there it is queueing: the board
is close enough to its service capacity that a burst builds a backlog and the
frames behind it wait for the drain.

Four 500 kbit/s CAN buses produce on the order of 16 000 frames/s, so the middle
row is already past anything this gateway would see in the role it was built
for.

### Batching trades latency for syscalls, monotonically

| linger | frames/batch | p50 | p99 | write syscalls/s |
|---|---|---|---|---|
| 0 | 1.1 | 72.5 µs | 121.8 µs | 18 182 |
| 100 µs | 3.2 | 133.6 µs | 233.6 µs | 6 250 |
| 250 µs | 6.2 | 229.5 µs | 368.7 µs | 3 226 |
| 500 µs | 11.5 | 340.3 µs | 677.2 µs | 1 739 |
| 1000 µs | 21.8 | 593.7 µs | 1 143.6 µs | 917 |
| 2500 µs | 52.9 | 1 365.7 µs | 2 652.0 µs | 378 |
| 5000 µs | 104.0 | 2 678.0 µs | 5 177.6 µs | 192 |

Waiting to coalesce buys syscalls and costs latency, in a straight line, at both
percentiles. A hundredfold reduction in write syscalls costs about twenty times
the p50.

**This retracts a previous finding.** An earlier version of this file reported
that on the Pi the curve turned over -- that a 100 µs linger made p99 3.7 times
*better* than no linger, and that the optimum was 100-250 µs rather than zero.
That was the probe's own stall: at linger 0 the probe sees 20 000 individual
frames a second and accumulates samples fastest, so the periodic sort that
polluted the tail was largest exactly where the claim needed it to be. There is
no turnover. Zero is the lowest-latency setting on this board as it is on any
other, and the reason to raise it is syscall rate, not tail latency.

### Under overload, more loss means lower latency

| offered | delivered | lost | loss | p99 |
|---|---|---|---|---|
| 5 000/s | 53 233 | 0 | 0.00% | 2 297 ms |
| 20 000/s | 107 658 | 109 667 | 50.46% | 1 784 ms |
| 50 000/s | 173 597 | 391 894 | 69.30% | 1 213 ms |
| 100 000/s | 236 245 | 922 444 | 79.61% | 966 ms |
| 200 000/s | 392 498 | 1 960 773 | 83.32% | **697 ms** |

The consumer stalls 300 µs per batch throughout, so this is what the pipeline
does when a consumer is definitively too slow, not how fast it can go.

Loss rises with load, as expected. Latency does not: it falls monotonically as
loss rises. That is drop-oldest working as designed -- the more aggressively
stale frames are discarded, the younger the survivors are when they arrive. A
pipeline that buffered instead of dropping would deliver everything, eventually,
and every frame would be worthless by then.

The 5 000/s row is the one to read carefully: zero loss and 2.3 seconds of
latency. Nothing was dropped because an 8192-deep buffer at that rate holds 1.6
seconds of traffic, so the delay is the buffer, not the pipeline.

### The topologies differ when there is more than one bus

| topology | sources | sent | dropped | delivered |
|---|---|---|---|---|
| queue | 1 | 156 204 | 185 696 | 45.7% |
| ring | 1 | 167 990 | 147 182 | 53.3% |
| queue | 4 | 388 480 | 1 004 600 | **27.9%** |
| ring | 4 | 659 082 | 621 949 | **51.4%** |

With four sources the ring delivers nearly twice as much of the same offered
load. With one it is ahead by rather less, and that margin is the least
trustworthy number in this file: a single run of a saturated pipeline, where
what gets dropped depends on scheduling accidents. The four-source gap is large
enough and mechanical enough to stand on.

Topology A is not slow by accident. Order in a queue is push order, so it cannot
spread ingest across threads without handing a consumer sequences out of order --
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
wired Ethernet, 35 seconds, 175 probes sent and 175 returned:

| | idle side-channel | in-band probe |
|---|---|---|
| RTT p50 | 518 µs | **496.1 µs** |
| RTT p99 | 2 294 µs | **655.7 µs** |
| RTT max | 12 219 µs | **6 135.0 µs** |

The two mechanisms share no code — one is a standalone Python TCP echo
(`link-ceiling.py`), the other a C++ record travelling through the whole egress
path, the same batching and the same socket as the frames around it — and their
medians agree to within 4%. That agreement is the cross-check that neither is
measuring something else.

**One-way p50 is therefore about 248 µs**, halved from the round trip, assuming
a symmetric path and including the consumer's turnaround. The gateway's own
contribution — 60–70 µs, measured locally in a single clock domain — is a small
part of it. The two halves are measured separately and added, which is
defensible; subtracting two clocks that disagree by 1.19 seconds is not.

### What the tails do not say

An earlier version of this section reported the in-band p99 as 25 460 µs against
the side-channel's 2 294 µs and made an argument out of the elevenfold gap: that
an idle probe measures the network while an in-band one measures what a frame
really experiences. The gap was the probe stalling itself (see the correction at
the top of this file), and 38 probes were "dropped by the consumer" for the same
reason. With the instrument fixed, none are dropped and the in-band tail is
*lower* than the side-channel's.

The argument was appealing and it fit. It is worth recording that it was wrong,
because the number that supported it was produced by the thing being measured.

The remaining difference runs the other way and is not interesting: the
side-channel's responder is a Python script and the in-band one is the C++
consumer, so their tails describe two different responders rather than two
different paths. The medians agree because the network dominates both; the tails
differ because the software does not.

## Does SCHED_FIFO help a pipeline the way it helped a periodic loop?

The supervisor gained roughly 570x lower p99 tick jitter from SCHED_FIFO,
`mlockall` and an isolated core. That was a single-threaded loop whose whole job
was to wake on a 10 ms timer, so being scheduled late *was* the error. This
pipeline is a different shape, and the question is worth measuring rather than
assuming in either direction.

Four arms at 20 000 frames/s on the ring, `./scripts/measure.sh sched`. The three
loaded arms run one busy loop per core at ordinary priority; without contention
every policy looks identical and the experiment answers nothing. Microseconds:

| arm | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| idle, `SCHED_OTHER` | 133.7 | 187.7 | 229.5 | 290.2 | 3 134.3 |
| loaded, `SCHED_OTHER` | 206.6 | 420.9 | **5 087.4** | **8 671.9** | 12 126.5 |
| loaded, `SCHED_FIFO` 20 | 124.2 | 176.8 | **226.2** | **245.3** | 2 855.8 |
| loaded, `SCHED_FIFO` + `mlockall` | 124.2 | 177.2 | 226.8 | 248.9 | 3 788.0 |

No frames were lost in any arm.

Contention costs 22x at p99 and 30x at p99.9. `SCHED_FIFO` gives all of it back,
across the whole distribution rather than just the middle of it: every
percentile under contention at real-time priority is at or below the idle
figure at ordinary priority.

Beating the idle arm is not a paradox. The governor is `ondemand`, so an idle
board drops its clock and sleeps its cores, and every wake-up pays to come back.
The busy loops hold the frequency up. It is the same effect that makes 2 000
frames/s slower than 20 000 in the rate sweep above.

`mlockall` adds nothing measurable on top. That is worth stating rather than
quietly dropping: the supervisor used it, this asks whether it matters here, and
the answer on this workload is no. It is a real answer, not a null result --
locking memory protects against faults this pipeline does not take, because it
allocates its ring once at startup and then reuses it.

**This replaces an earlier conclusion.** A previous version of this file reported
that `SCHED_FIFO` recovered p50 through p99 but left p99.9 and max untouched at
about 18 ms and 24 ms, and reasoned at length about what could produce a tail
that priority could not reach. Nothing did: that tail was the probe sorting its
own sample buffer inside the loop it was measuring, and no scheduling policy
makes a sort faster. Once the instrument stopped stalling, the tail it was
protecting turned out not to exist.

The lesson is the one worth keeping. The wrong conclusion was reached carefully
-- three repetitions, a stated hypothesis, an experiment built to test it, and
`mlockall` correctly refuted. Every step was sound and the answer was still
wrong, because the instrument was inside the measurement. The rate sweep had
been sitting in this same file the whole time showing a tail that scaled with
sample count, and it was read as evidence of queueing.

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
  statement about these runs; the die was at 53.7 °C.
