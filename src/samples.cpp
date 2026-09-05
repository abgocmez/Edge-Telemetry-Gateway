#include "samples.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace etg {
namespace {

std::int64_t nearest_rank(const std::vector<std::int64_t>& sorted, double p) {
  if (sorted.empty()) {
    return 0;
  }
  const auto n = static_cast<double>(sorted.size());
  auto idx = static_cast<std::size_t>(std::ceil(p * n));
  if (idx == 0) {
    idx = 1;
  }
  if (idx > sorted.size()) {
    idx = sorted.size();
  }
  return sorted[idx - 1];
}

}  // namespace

Samples::Samples(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {
  values_.reserve(capacity_ < (1U << 16U) ? capacity_ : (1U << 16U));
}

void Samples::add(std::int64_t value) {
  ++total_;
  if (++seen_ < stride_) {
    return;
  }
  seen_ = 0;
  values_.push_back(value);
  if (values_.size() >= capacity_) {
    decimate();
  }
}

void Samples::decimate() {
  std::size_t out = 0;
  for (std::size_t i = 0; i < values_.size(); i += 2) {
    values_[out++] = values_[i];
  }
  values_.resize(out);
  stride_ *= 2;
}

Percentiles Samples::compute() const {
  Percentiles p;
  p.count = total_;
  if (values_.empty()) {
    return p;
  }

  std::vector<std::int64_t> sorted = values_;
  std::sort(sorted.begin(), sorted.end());

  p.min = sorted.front();
  p.max = sorted.back();
  p.mean = static_cast<double>(std::accumulate(sorted.begin(), sorted.end(), std::int64_t{0})) /
           static_cast<double>(sorted.size());
  p.p50 = nearest_rank(sorted, 0.50);
  p.p90 = nearest_rank(sorted, 0.90);
  p.p99 = nearest_rank(sorted, 0.99);
  p.p999 = nearest_rank(sorted, 0.999);
  return p;
}

bool Samples::write_csv(const std::string& path, const char* header) const {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (f == nullptr) {
    return false;
  }
  std::fprintf(f, "%s\n", header);
  for (const std::int64_t v : values_) {
    std::fprintf(f, "%lld\n", static_cast<long long>(v));
  }
  std::fclose(f);
  return true;
}

}  // namespace etg
