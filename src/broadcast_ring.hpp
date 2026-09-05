#pragma once

#include <sched.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <vector>

#include "frame.hpp"

namespace etg {

// A bounded multi-producer broadcast ring.
//
// M producers write; N readers each hold their own cursor and every reader sees
// every frame. Readers never block writers, by construction: a producer
// overwrites the oldest cell rather than waiting for anyone, and a reader that
// has fallen too far behind discovers it after the fact from the sequence
// number and reports the gap.
//
// This is the point of the whole design. "One slow consumer cannot stall the
// pipeline" stops being a policy someone has to remember to enforce and becomes
// a property of the data structure: there is no code path in which a producer
// waits for a reader, because there is no such code path to write.
//
// ---------------------------------------------------------------------------
// Cell protocol
//
// Each cell holds a sequence word encoding both the position it holds and
// whether a write is in progress:
//
//     seq == 0            never written
//     seq == 2*pos + 1    a producer is writing position pos right now
//     seq == 2*pos + 2    position pos is complete and readable
//
// so pos == (seq - 1) / 2 for either phase, using integer division.
//
// This is the seqlock from the supervisor project, generalised: there it
// protected one shared-memory cell with a single writer, here it protects
// `capacity` cells with M writers. The primitive is the same; what is new is
// that two producers can now target the same cell, which a seqlock does not
// handle on its own. See "The producer collision" below.
//
// ---------------------------------------------------------------------------
// Ordering, and why each one
//
// Producer:
//   ticket   fetch_add(relaxed)  - the counter orders nothing but itself; the
//                                  cell's own CAS provides all the ordering that
//                                  matters, so paying for more here is paying
//                                  for a guarantee nothing consumes.
//   claim    CAS(acquire)        - one-way barrier. Without it the payload
//                                  stores could be hoisted above the claim, and
//                                  a reader would see a cell still marked
//                                  complete-for-the-previous-lap while its bytes
//                                  were already changing: a torn read the
//                                  sequence check could not detect. The acquire
//                                  also synchronises with the previous
//                                  occupant's release, which is what makes its
//                                  completion visible to us.
//   payload  store(relaxed)      - ordered by the release below, not by
//                                  themselves.
//   publish  store(release)      - everything written above is visible to any
//                                  reader whose acquire-load observes this.
//
// Reader:
//   s1       load(acquire)       - pairs with the publish. Payload reads below
//                                  cannot be hoisted above it.
//   payload  load(relaxed)
//   fence    fence(acquire)      - keeps the *second* sequence load from
//                                  floating up above the payload reads. Without
//                                  it the validity check could be answered by a
//                                  value read before the data it is supposed to
//                                  be validating, and the check would be
//                                  vacuous. This fence is the least obvious line
//                                  in the file and the easiest one to delete by
//                                  accident.
//   s2       load(relaxed)       - the fence already ordered it.
//
// ---------------------------------------------------------------------------
// The producer collision
//
// With M producers claiming tickets by fetch_add, two of them can target the
// same cell. Producer A takes position 5 and is descheduled; producers B..Z take
// 6..1029 on a 1024-cell ring; Z's position maps to the same cell as A's. Both
// now want to write cell 5, and a seqlock assumes a single writer - the odd/even
// marker cannot express "two writers", so A and Z would interleave their bytes
// and both would publish an even sequence over the wreckage.
//
// The fix is that a producer must *claim* the cell rather than simply store to
// it: a CAS from the exact sequence value the previous occupant left behind. Z's
// CAS therefore fails while A is still mid-write, and Z retries.
//
// The honest consequence: this is lock-free across cells and obstruction-free
// per cell. A producer stalled between claim and publish blocks that one cell,
// and any producer that laps onto it waits. Readers degrade gracefully - they
// see an odd sequence, treat the cell as not yet readable, and move on - so a
// stall costs latency on one slot rather than correctness anywhere.
//
// ---------------------------------------------------------------------------
// Why the payload is atomic
//
// Reading a cell while a producer may be overwriting it is exactly what the
// sequence check exists to detect - but in the C++ memory model that read is a
// data race, and a data race is undefined behaviour regardless of whether the
// program later notices and discards the result. Seqlocks are a known gap in
// the model, not a mistake unique to this code.
//
// Rather than write a plain memcpy and suppress the sanitizer, the payload is
// held as relaxed atomic words. That makes the race well-defined, keeps
// ThreadSanitizer meaningful on the whole structure instead of blind to its most
// interesting part, and costs nothing: a relaxed atomic load or store of an
// aligned 64-bit word compiles to the same instruction as a plain one on both
// x86_64 and aarch64.
//
// ---------------------------------------------------------------------------
// What testing can and cannot say about the orderings above
//
// It cannot say much, and pretending otherwise would be the most dishonest
// thing in this file. Measured, not assumed:
//
//   x86_64    Weakening the publish store to relaxed and deleting the reader's
//             acquire fence produces **byte-identical machine code**. Verified
//             by diffing the disassembly of both builds. No test on the
//             development machine can distinguish them, because there is
//             nothing to distinguish.
//
//   aarch64   The barriers are real and the compiler emits them: `stlr` goes
//             from 2 to 0 and `dmb` from 1 to 0 when those two edits are made.
//             So the ordering request does reach the hardware.
//
//   Cortex-A53 (Pi 3 B+)
//             And yet neither broken variant failed. Seconds of stress at
//             millions of frames per second, with both detectors armed,
//             reported zero torn and zero out-of-position reads. The A53 is an
//             in-order core; the architecture permits the reordering, this
//             implementation does not appear to perform it.
//
// The conclusion is worth stating plainly, because an earlier version of this
// project assumed the opposite: running on "real ARM hardware" is necessary but
// **not sufficient** to falsify a memory-ordering choice. Falsifying it needs a
// core that actually reorders - an out-of-order ARM such as A72, A76, Graviton
// or Apple silicon - and none of those is on this desk.
//
// So these orderings rest on the model's guarantees plus instruction-level
// evidence that the barriers are emitted, and not on a test that fails without
// them. That is a weaker claim than "tested", and it is the true one.
class BroadcastRing {
 public:
  static constexpr std::size_t kWordSize = sizeof(std::uint64_t);
  static constexpr std::size_t kFrameWords = sizeof(Frame) / kWordSize;
  static_assert(sizeof(Frame) % kWordSize == 0,
                "the frame must divide into whole words for the atomic payload");
  static_assert(offsetof(Frame, seq) == 0,
                "word 0 is overwritten with the ring position; seq must be first");

  // One cell per cache line. Two cells sharing a line would put unrelated
  // producers into a false-sharing fight over the same coherence unit, which is
  // the single easiest way to make a lock-free structure slower than a mutex.
  //
  // The alignment is a build option purely so that claim can be measured rather
  // than asserted: -DETG_CELL_ALIGN=8 packs cells and shows what false sharing
  // actually costs on a given machine. 64 is the only value to ship.
#ifndef ETG_CELL_ALIGN
#define ETG_CELL_ALIGN 64
#endif
  struct alignas(ETG_CELL_ALIGN) Cell {
    std::atomic<std::uint64_t> seq{0};
    std::array<std::atomic<std::uint64_t>, kFrameWords> words{};
  };
  static_assert(ETG_CELL_ALIGN != 64 || sizeof(Cell) == 64,
                "at the shipped alignment a cell must be exactly one cache line");

  enum class ReadStatus : std::uint8_t {
    kOk,      // a frame was produced
    kEmpty,   // nothing new, or the cell is mid-write
    kLapped,  // the reader fell behind and the frame is gone
  };

  // A reader's private position. Never shared, never atomic: the whole point is
  // that readers do not coordinate with each other or with producers.
  struct Cursor {
    std::uint64_t next = 0;
    std::uint64_t missed = 0;  // frames this reader will never see
  };

  struct ReadResult {
    std::size_t count = 0;
    std::uint64_t missed = 0;
  };

  // Capacity is rounded up to a power of two so the index is a mask rather than
  // a division.
  explicit BroadcastRing(std::size_t capacity)
      : capacity_(round_up_pow2(capacity < 2 ? 2 : capacity)),
        mask_(capacity_ - 1),
        cells_(capacity_) {}

  BroadcastRing(const BroadcastRing&) = delete;
  BroadcastRing& operator=(const BroadcastRing&) = delete;

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  // The position the next frame will take. Advisory: by the time a caller reads
  // it, it may already be stale.
  [[nodiscard]] std::uint64_t write_position() const noexcept {
    return write_pos_.load(std::memory_order_relaxed);
  }

  // Reads whose payload did not match the position their sequence word claimed.
  //
  // In a correct build this is unreachable, which is exactly why it is worth
  // counting: it is the detector for a broken publish ordering. If the release
  // on the publish store is weakened, a reader can observe the new sequence
  // before the payload stores are visible and copy out the *previous*
  // occupant's frame - which is internally consistent and therefore invisible
  // to any checksum over the payload. Comparing the frame's own sequence field
  // against the position the cursor asked for catches it, because the ring
  // writes the position into that field as part of the payload.
  [[nodiscard]] std::uint64_t inconsistent_reads() const noexcept {
    return inconsistent_reads_.load(std::memory_order_relaxed);
  }

  // How many times a producer found a cell still held by its previous occupant
  // and had to retry the claim.
  //
  // This exists to make the producer collision observable. The window between
  // claiming a cell and publishing it is a handful of stores, so the collision
  // is nearly impossible to provoke on demand - a stress test that merely runs
  // clean proves nothing, because it proves the same thing with the claim
  // removed. Counting the retries turns "this race is real and the CAS handles
  // it" from an argument into a number: a test can assert that collisions
  // actually happened in the run *and* that nothing tore.
  [[nodiscard]] std::uint64_t claim_retries() const noexcept {
    return claim_retries_.load(std::memory_order_relaxed);
  }

  // Publishes one frame and returns the sequence it was given.
  //
  // The ring is the single point of ordering: a source cannot assign the
  // sequence because a single bus does not know the global order, so the ticket
  // taken here *is* the sequence, and it is written into the stored frame.
  std::uint64_t publish(const Frame& f) {
    const std::uint64_t pos = write_pos_.fetch_add(1, std::memory_order_relaxed);
    write_at(pos, f);
    wake_readers();
    return pos;
  }

  // Claims count contiguous tickets with one RMW.
  //
  // The trade is worth stating: one fetch_add instead of count of them, at the
  // cost of a wider obstruction window - a producer that stalls mid-batch blocks
  // every cell it claimed, not one. Contiguous tickets also preserve per-source
  // FIFO within the batch, which a per-frame loop would preserve as well but
  // only because a single thread claims monotonically.
  std::uint64_t publish_batch(std::span<const Frame> frames) {
    if (frames.empty()) {
      return write_pos_.load(std::memory_order_relaxed);
    }
    const std::uint64_t base = write_pos_.fetch_add(frames.size(), std::memory_order_relaxed);
    for (std::size_t i = 0; i < frames.size(); ++i) {
      write_at(base + i, frames[i]);
    }
    // Once per batch rather than once per frame: a parked reader only needs to
    // be told that something arrived, not how much.
    wake_readers();
    return base;
  }

  ReadStatus try_read(Cursor& cursor, Frame& out) const {
    const Cell& cell = cells_[cursor.next & mask_];

    const std::uint64_t s1 = cell.seq.load(std::memory_order_acquire);
    if (s1 == 0) {
      return ReadStatus::kEmpty;  // this cell has never been written
    }

    const std::uint64_t cell_pos = (s1 - 1) / 2;
    if (cell_pos < cursor.next) {
      return ReadStatus::kEmpty;  // still holds an older lap; ours is not written
    }
    if (cell_pos > cursor.next) {
      lap(cursor);
      return ReadStatus::kLapped;
    }
    if ((s1 & 1U) != 0) {
      return ReadStatus::kEmpty;  // a producer is writing this cell right now
    }

    std::array<std::uint64_t, kFrameWords> words{};
    for (std::size_t i = 0; i < kFrameWords; ++i) {
      words[i] = cell.words[i].load(std::memory_order_relaxed);
    }

    // Keeps the load below from being answered before the reads above. Deleting
    // this line leaves a check that validates nothing.
    std::atomic_thread_fence(std::memory_order_acquire);

    const std::uint64_t s2 = cell.seq.load(std::memory_order_relaxed);
    if (s2 != s1) {
      // A producer took the cell while it was being read. The bytes just copied
      // are a mixture of two frames and are discarded, not reported.
      lap(cursor);
      return ReadStatus::kLapped;
    }

    // One comparison, always on. The value is already in a register and the
    // branch is never taken in a correct build; the cost is nothing and the
    // alternative is a whole class of ordering bug that produces plausible data.
    if (words[0] != cursor.next) {
      inconsistent_reads_.fetch_add(1, std::memory_order_relaxed);
      lap(cursor);
      return ReadStatus::kLapped;
    }

    std::memcpy(&out, words.data(), sizeof(Frame));
    ++cursor.next;
    return ReadStatus::kOk;
  }

  ReadResult read_batch(Cursor& cursor, std::span<Frame> out) const {
    ReadResult result;
    const std::uint64_t missed_before = cursor.missed;

    while (result.count < out.size()) {
      const ReadStatus st = try_read(cursor, out[result.count]);
      if (st == ReadStatus::kOk) {
        ++result.count;
        continue;
      }
      if (st == ReadStatus::kLapped) {
        continue;  // cursor was moved forward; try again from there
      }
      break;  // kEmpty
    }

    result.missed = cursor.missed - missed_before;
    return result;
  }

  // Positions a fresh cursor at the oldest frame still in the ring, so a reader
  // that attaches to a running gateway starts from live data rather than
  // reporting a gap for everything published before it existed.
  [[nodiscard]] Cursor attach() const noexcept {
    Cursor c;
    c.next = oldest();
    return c;
  }

  // ---------------------------------------------------------------------
  // Idle path
  //
  // The protocol above is lock-free and stays that way. What follows is not
  // part of it: it is how a reader with nothing to do stops burning a core.
  //
  // A reader could simply spin, and on a busy feed it effectively does - the
  // first try_read succeeds and none of this is reached. But a consumer polling
  // an idle bus would spin for the whole of its poll timeout, which on a
  // four-core Pi is a core wasted per idle consumer.
  //
  // The cost to a producer is one relaxed load per publish when nobody is
  // parked, which is the shape that matters: the fast path pays almost nothing
  // for a facility only the slow path uses. The mutex is touched by a producer
  // only when a reader is actually waiting on it.

  // Parks until the write position moves past `known`, or the timeout expires.
  // Returns true if there may be new data. Spurious wakeups are fine; the caller
  // re-checks by reading.
  bool wait_for_data(std::uint64_t known, std::chrono::milliseconds timeout) const {
    if (write_pos_.load(std::memory_order_acquire) > known) {
      return true;
    }
    waiters_.fetch_add(1, std::memory_order_seq_cst);

    // Re-check after registering. Without this, a publish that lands between the
    // check above and the wait below would find waiters_ still zero, skip the
    // notify, and leave this reader parked until the timeout - a lost wakeup on
    // every quiet-to-busy transition.
    if (write_pos_.load(std::memory_order_acquire) > known) {
      waiters_.fetch_sub(1, std::memory_order_relaxed);
      return true;
    }

    bool woken = false;
    {
      std::unique_lock<std::mutex> lock(idle_mutex_);
      woken = idle_cv_.wait_for(lock, timeout, [this, known] {
        return write_pos_.load(std::memory_order_acquire) > known || closed_;
      });
    }
    waiters_.fetch_sub(1, std::memory_order_relaxed);
    return woken;
  }

  // Wakes every parked reader and makes further waits return immediately, so
  // shutdown does not have to wait out a timeout.
  void close() {
    {
      const std::lock_guard<std::mutex> lock(idle_mutex_);
      closed_ = true;
    }
    idle_cv_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    const std::lock_guard<std::mutex> lock(idle_mutex_);
    return closed_;
  }

 private:
  static std::size_t round_up_pow2(std::size_t v) {
    std::size_t p = 1;
    while (p < v) {
      p <<= 1U;
    }
    return p;
  }

  [[nodiscard]] std::uint64_t oldest() const noexcept {
    const std::uint64_t w = write_pos_.load(std::memory_order_relaxed);
    return w > capacity_ ? w - capacity_ : 0;
  }

  void lap(Cursor& cursor) const noexcept {
    const std::uint64_t target = oldest();
    if (target > cursor.next) {
      cursor.missed += target - cursor.next;
      cursor.next = target;
    } else {
      // The ring moved on between the check and here. Skipping one keeps the
      // reader making progress instead of spinning on a cell that is being
      // rewritten faster than it can be read.
      ++cursor.missed;
      ++cursor.next;
    }
  }

  void write_at(std::uint64_t pos, const Frame& f) {
    Cell& cell = cells_[pos & mask_];

    // The value the previous occupant of this cell left behind. Anything else
    // means it has not finished, and this producer must wait for it.
    const std::uint64_t expected =
        pos < capacity_ ? 0 : 2 * (pos - capacity_) + 2;
    const std::uint64_t claimed = 2 * pos + 1;

    std::uint64_t witness = expected;
    unsigned spins = 0;
    while (!cell.seq.compare_exchange_weak(witness, claimed, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
      // Only a producer still mid-write on the previous lap can hold this cell.
      // Reset the expectation and retry; this is the obstruction-free window.
      witness = expected;
      claim_retries_.fetch_add(1, std::memory_order_relaxed);

      if (++spins < kSpinsBeforeYield) {
        spin();
        continue;
      }
      // Measured on the Pi: with producers >= cores this loop collapses. Four
      // producers and one reader on four cores fell from 14.4M frames/s to
      // 10k/s, with 75 million retries. The cause is that the cheap spin above
      // is a scheduling *hint* - it does not release the core - so every thread
      // waiting on a descheduled claim holder burns its whole timeslice keeping
      // the scheduler from running the one thread that could make progress.
      //
      // On a four-core edge device with four buses that is the normal operating
      // condition, not a corner case, so the loop gives the core back instead.
      spins = 0;
      ::sched_yield();
    }

    std::array<std::uint64_t, kFrameWords> words{};
    std::memcpy(words.data(), &f, sizeof(Frame));
    words[0] = pos;  // the ticket is the sequence; frame.seq lives at offset 0

    for (std::size_t i = 0; i < kFrameWords; ++i) {
      cell.words[i].store(words[i], std::memory_order_relaxed);
    }

    cell.seq.store(2 * pos + 2, std::memory_order_release);
  }

  // The whole cost of the idle path on the fast path: one relaxed load. The
  // mutex is only taken when a reader is genuinely parked on it.
  void wake_readers() const {
    if (waiters_.load(std::memory_order_seq_cst) == 0) {
      return;
    }
    idle_cv_.notify_all();
  }

  // Long enough that an uncontended collision costs nothing but a few pauses,
  // short enough that a descheduled holder is not waited on for a whole
  // timeslice.
  static constexpr unsigned kSpinsBeforeYield = 64;

  static void spin() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
  }

  std::size_t capacity_;
  std::size_t mask_;

  // On its own line: producers hammer this with fetch_add, and sharing a line
  // with the cells would drag an unrelated cell's coherence state along with it.
  alignas(64) std::atomic<std::uint64_t> write_pos_{0};

  // Diagnostic only, never read by the protocol, so relaxed throughout.
  std::atomic<std::uint64_t> claim_retries_{0};
  mutable std::atomic<std::uint64_t> inconsistent_reads_{0};

  // Idle-path only. None of this participates in the lock-free protocol.
  mutable std::atomic<std::uint32_t> waiters_{0};
  mutable std::mutex idle_mutex_;
  mutable std::condition_variable idle_cv_;
  bool closed_ = false;

  std::vector<Cell> cells_;
};

}  // namespace etg
