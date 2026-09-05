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
class Samples {
 public:
  explicit Samples(std::size_t capacity = 8U << 20U);

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
