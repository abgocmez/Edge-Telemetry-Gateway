#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace etg {

struct Percentiles {
  std::uint64_t count = 0;  // samples observed, not samples retained
  std::int64_t min = 0;
  std::int64_t max = 0;
  double mean = 0.0;
  std::int64_t p50 = 0;
  std::int64_t p90 = 0;
  std::int64_t p99 = 0;
  std::int64_t p999 = 0;
};

// A bounded sample buffer that keeps exact values rather than bucketing them,
// so percentiles are computed from real samples and not from a bucket boundary
// that happens to be nearby.
//
// Memory is bounded by systematic decimation: when the buffer fills, every
// second retained sample is discarded and the keep-stride doubles. That is
// unbiased for order statistics, unlike keeping the first N (which reports the
// warm-up) or the last N (which reports the tail of the run). `stride()` is
// reported alongside the numbers so a reader can see whether decimation
// happened at all.
//
// Nearest-rank percentiles: the p-th percentile is the value at index
// ceil(p * n) - 1 of the sorted retained samples. Stated because a reader
// comparing against a tool that interpolates will otherwise see a small
// disagreement and not know which is which.
// Percentiles over a caller-owned vector, which this sorts in place. Split out
// so a caller holding a lock can copy the values, release the lock, and only
// then pay for the sort - see the note in etg-view. Same nearest-rank rule as
// Samples::compute, so the two agree.
[[nodiscard]] Percentiles percentiles_of(std::vector<std::int64_t>& values);

class Samples {
 public:
  // The buffer is allocated and touched in full at construction, never grown
  // during a run. A std::vector that doubles as it fills charges some unlucky
  // sample for copying the whole buffer plus first-touch faults on the new one,
  // and in a measurement tool that cost lands in the tail being reported: it is
  // what was left of etg-probe's inflated tail after the periodic sort was
  // removed, showing up as isolated ~100 ms outliers each time the buffer
  // stepped up a power of two. Past capacity, decimate() keeps the size fixed
  // rather than reallocating.
  //
  // The default is a megabyte of samples, 8 MB, which at 20 000 frames/s is
  // about a minute before decimation begins. Callers that sample far less often
  // should ask for less: this is allocated per instance.
  explicit Samples(std::size_t capacity = 1U << 20U);

  void add(std::int64_t value);

  [[nodiscard]] Percentiles compute() const;
  [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
  [[nodiscard]] std::size_t retained() const noexcept { return values_.size(); }
  [[nodiscard]] std::uint64_t stride() const noexcept { return stride_; }

  // One value per line, for analysis from committed scripts rather than from
  // whatever was on screen at the time.
  [[nodiscard]] bool write_csv(const std::string& path, const char* header) const;

 private:
  void decimate();

  std::vector<std::int64_t> values_;
  std::size_t capacity_;
  std::uint64_t total_ = 0;   // every sample ever offered
  std::uint64_t stride_ = 1;  // keep one in every `stride_`
  std::uint64_t seen_ = 0;    // counts toward the next keep
};

}  // namespace etg
