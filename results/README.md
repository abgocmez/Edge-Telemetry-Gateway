# Measurements

Everything here is produced by `scripts/measure.sh` and carries the output of
`scripts/measure-env.sh capture` beside it. Each experiment directory holds a
`summary.txt`, an `env.txt`, and the raw gateway and probe logs for every run.

`ADMIN-20260906/` holds the numbers quoted below. `ADMIN-20260905/` is kept
deliberately: it is the same experiments measured with the instrument bug
described in the next section, and the two directories side by side are the
evidence for that correction rather than a claim about it.

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
- **Governor.** These runs use the board's default `ondemand`, recorded in every
  `env.txt`. `measure-env.sh perf-on` exists and pins `performance`, but leaving
  it off is the more honest default here and two findings below turn on it: a
  nearly idle board drops its clock and sleeps its cores, which is why 2 000
  frames/s is slower than 20 000 and why a contended pipeline beats an idle one.
  Pinning `performance` would erase both effects and also run the board hotter,
  changing the thermal behaviour the last section measures.

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

Ring topology; the queue is within noise of it at every rate.

| rate | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| 2 000/s | 114.2 µs | 126.0 µs | 151.5 µs | 191.0 µs | 254.5 µs |
| 20 000/s | 66.2 µs | 79.8 µs | 121.0 µs | 189.2 µs | 2 410.1 µs |
| 100 000/s | 70.1 µs | 86.0 µs | 101.1 µs | 1 283.7 µs | 3 635.3 µs |

The 2 000/s row is slower than the 20 000/s row through p90, which looks
backwards and is not. The governor is `ondemand`: at 2 000 frames/s the board is
nearly idle, so it drops the clock and lets cores enter idle states, and every
wake-up pays to come back. Ten times the work arrives *sooner* because the
machine is already awake. The same effect shows up again in the scheduling
experiment, where a loaded pipeline beats an idle one.

It also has the flattest tail of the three, because at that rate nothing ever
queues. What the higher rates buy in wake-up latency they pay back at p99.9.

Four 500 kbit/s CAN buses produce on the order of 16 000 frames/s, so the middle
row is already past anything this gateway would see in the role it was built
for.

### Batching trades latency for syscalls, monotonically

| linger | frames/batch | p50 | p99 | write syscalls/s |
|---|---|---|---|---|
| 0 | 1.1 | 72.7 µs | 118.3 µs | 18 182 |
| 100 µs | 3.1 | 132.8 µs | 233.9 µs | 6 452 |
| 250 µs | 6.2 | 229.6 µs | 368.7 µs | 3 226 |
| 500 µs | 11.4 | 338.0 µs | 607.2 µs | 1 754 |
| 1000 µs | 21.8 | 593.3 µs | 1 106.2 µs | 917 |
| 2500 µs | 52.9 | 1 370.2 µs | 2 653.2 µs | 378 |
| 5000 µs | 104.1 | 2 677.4 µs | 5 176.7 µs | 192 |

Waiting to coalesce buys syscalls and costs latency, in a straight line, at both
percentiles. A hundredfold reduction in write syscalls costs about twenty times
the p50. This sweep reproduces to within 2% across runs, which is more than can
be said for the two below.

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
| 5 000/s | 53 300 | 0 | 0.00% | 2 334 ms |
| 20 000/s | 131 276 | 48 723 | 27.07% | 3 777 ms |
| 50 000/s | 172 493 | 399 134 | 69.82% | 1 174 ms |
| 100 000/s | 231 670 | 962 425 | 80.60% | 971 ms |
| 200 000/s | 371 494 | 1 914 674 | 83.75% | **711 ms** |

The consumer stalls 300 µs per batch throughout, so this is what the pipeline
does when a consumer is definitively too slow, not how fast it can go.

Loss rises with load, as expected. Latency does not: past the peak it falls as
loss rises. That is drop-oldest working as designed -- the more aggressively
stale frames are discarded, the younger the survivors are when they arrive. A
pipeline that buffered instead of dropping would deliver everything, eventually,
and every frame would be worthless by then.

Where the peak sits moves between runs: 20 000/s here, 5 000/s in the previous
run, and the loss at 20 000/s was 27% here against 50% there. The high-load rows
are stable and the trend across them is the finding; the exact turnover point is
not.

The 5 000/s row is the one to read carefully: zero loss and 2.3 seconds of
latency. Nothing was dropped because an 8192-deep buffer at that rate holds 1.6
seconds of traffic, so the delay is the buffer, not the pipeline.

### The topologies differ when there is more than one bus

| topology | sources | sent | dropped | delivered |
|---|---|---|---|---|
| queue | 1 | 261 856 | 80 266 | 76.5% |
| ring | 1 | 168 220 | 147 000 | 53.4% |
| queue | 4 | 353 210 | 1 039 927 | **25.4%** |
| ring | 4 | 579 507 | 709 676 | **45.0%** |

With four sources the ring delivers about 1.8 times as much of the same offered
load, and it did so in both runs -- 45.0% against 25.4% here, 51.4% against
27.9% before.

The one-source rows are worth nothing, and this run demonstrates why rather than
merely asserting it. The previous version of this file flagged them as the least
trustworthy numbers in the file: a single run of a saturated pipeline where what
survives depends on scheduling accidents. This run reversed them outright, from
ring ahead 53.3% to 45.7% into queue ahead 76.5% to 53.4%. Neither ordering means
anything. The four-source gap is large, mechanical and reproducible; the
one-source gap is a coin.

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
| idle, `SCHED_OTHER` | 133.2 | 187.9 | 236.2 | 446.4 | 1 540.3 |
| loaded, `SCHED_OTHER` | 247.2 | 2 518.9 | **4 638.2** | **5 948.8** | 9 346.5 |
| loaded, `SCHED_FIFO` 20 | 124.2 | 177.2 | **226.6** | **240.5** | 297.2 |
| loaded, `SCHED_FIFO` + `mlockall` | 124.1 | 177.4 | 226.5 | 237.1 | 400.8 |

No frames were lost in any arm.

Contention costs 20x at p99 and 13x at p99.9. `SCHED_FIFO` gives all of it back,
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
  statement about these runs; the die was at 60.7 °C.

## What a consumer costs when it dies, and what it costs everyone else

The last item on this project's own measurement list, and two questions rather
than one. `./scripts/measure.sh recovery` SIGKILLs a consumer ten times with a
two-second outage between each, while a second consumer on another port is never
touched. SIGKILL rather than SIGTERM: a consumer that was given the chance to
shut down tidily is not the case worth measuring.

| | |
|---|---|
| connect to first frame | **p50 140.7 µs**, min 119.3 µs, max 1 623.6 µs, n=10 |
| victim, per the gateway | connects=10, disconnects=10, markers sent=9 |
| witness, untouched | lost=0, silent jumps=0, connections=1, markers sent=0 |
| frames dropped for the victim | 306 267, against 476 782 delivered |

Ten kills produce ten reconnects and nine markers, which is right rather than
off by one: the first connection has no earlier connection to have missed
anything.

**There is no recovery curve.** A returning consumer's first frame arrives
140.7 µs after its connection is established, against a steady-state p50 of
132 µs — it is at full speed immediately. That is worth stating because the
intuitive model is wrong: the cost of a consumer dying is entirely the frames
that passed while it was absent, not a slow ramp once it is back. Timed from the
connection being up, not from process start, since exec and dynamic linking
belong to whatever restarts the consumer.

The witness line is the one that matters. Across ten kills of its neighbour it
saw zero markers, zero silent jumps and never reconnected. One consumer dying is
invisible to the others, which is the whole reason for a per-consumer egress
thread and an independent cursor rather than a shared position. A design that
shared one would have shown it here.

The victim's 306 267 dropped frames are not a fault. Two seconds of absence at
20 000 frames/s is 40 000 frames, ten times over, and the gateway's contract is
drop-oldest and say so — which is what the nine markers are. The alternative,
holding them, is how a dead consumer takes the live ones down with it.

## Does the board getting hot change any of this?

Every figure above comes from a run of twelve to forty-five seconds. The Pi
reports `throttled=0x80000` -- `soft-temp-limit-occurred` -- latched since boot,
so the firmware has dropped the clock at some point, and a short measurement may
simply never have been running when it happened. A gateway in a cabinet runs for
weeks. `./scripts/thermal.sh` runs thirty 45-second blocks back to back at
20 000 frames/s, 23 minutes in total, and reads the die temperature, ARM clock
and throttle word immediately before each one.

| | across 30 blocks |
|---|---|
| die temperature | 58.0 – 63.4 °C |
| ARM clock | 1100 – 1300 MHz |
| p50 | 131.9 – 136.2 µs |
| p90 | 187.4 – 188.3 µs |
| p99 | 228.5 – 243.5 µs |
| p99.9 | 273.5 – 425.6 µs (28 blocks), 811.5 and 1074.7 µs in two |

Nothing moves. The clock swings by 18% across the run and p50 answers with 1.3%,
p90 with 0.5%. The first block was actively throttling -- `0x80008`, the
current-state bit, at 63.4 °C -- and its numbers are indistinguishable from the
other twenty-nine.

That is not the board coping; it is the workload not being clock-bound. At
20 000 frames/s this pipeline spends its time waiting to be woken and making
syscalls, not computing, and neither cost scales with core frequency. It would
be a different answer at 100 000/s, where the rate sweep shows the pipeline
actually running out of room.

So the short runs elsewhere in this file are not flattered by a cool board. They
would be, at a rate high enough to be CPU-bound; they are not at this one.

This trace is also where the measurement bug surfaced, and the clearest
before-and-after of it. The same script on the same board reported p99 of 86 ms
and p99.9 of 123 ms with the old probe, from the same pipeline, because 45
seconds was long enough for the instrument's own sorting to dominate everything
it reported.
