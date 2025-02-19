/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/util.h"

#include <stdarg.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "xla/types.h"
#include "tsl/platform/env.h"
#include "tsl/platform/numbers.h"
#include "tsl/platform/stacktrace.h"
#include "tsl/platform/threadpool.h"

namespace xla {

std::vector<int64_t> ToMixedRadix(const int64_t n,
                                  absl::Span<const int64_t> bounds) {
  if (bounds.empty()) {
    return {};
  }

  std::vector<int64_t> digits;
  digits.reserve(bounds.size());
  int64_t divisor = Product(bounds);
  CHECK_GT(divisor, 0);
  int64_t remainder = n % divisor;
  for (const int64_t radix : bounds) {
    CHECK_GT(radix, 0);
    divisor /= radix;
    CHECK_GT(divisor, 0);

    // The divisor is always 1 for the last iteration.
    digits.push_back(remainder / divisor);
    remainder = remainder % divisor;
  }
  return digits;
}

Status WithLogBacktrace(const Status& status) {
  CHECK(!status.ok());
  VLOG(1) << status.ToString();
  VLOG(2) << tsl::CurrentStackTrace();
  return status;
}

ScopedLoggingTimer::ScopedLoggingTimer(absl::string_view label, bool enabled,
                                       const char* file, int line,
                                       TimerStats* timer_stats)
    : label_(label),
      file_(file),
      line_(line),
      timer_stats_(timer_stats),
      enabled_(enabled) {
  if (enabled_) {
    start_micros_ = tsl::Env::Default()->NowMicros();
  }
}

void ScopedLoggingTimer::StopAndLog() {
  if (enabled_) {
    uint64_t end_micros = tsl::Env::Default()->NowMicros();
    double secs = (end_micros - start_micros_) / 1000000.0;

    TimerStats& stats = *timer_stats_;
    absl::MutexLock lock(&stats.stats_mutex);
    stats.cumulative_secs += secs;
    if (secs > stats.max_secs) {
      stats.max_secs = secs;
    }
    stats.times_called++;

    LOG(INFO).AtLocation(file_, line_)
        << label_ << " time: " << tsl::strings::HumanReadableElapsedTime(secs)
        << " (cumulative: "
        << tsl::strings::HumanReadableElapsedTime(stats.cumulative_secs)
        << ", max: " << tsl::strings::HumanReadableElapsedTime(stats.max_secs)
        << ", #called: " << stats.times_called << ")";
    enabled_ = false;
  }
}

ScopedLoggingTimer::~ScopedLoggingTimer() { StopAndLog(); }

Status AddStatus(Status prior, absl::string_view context) {
  CHECK(!prior.ok());
  return Status{prior.code(), absl::StrCat(context, ": ", prior.message())};
}

Status AppendStatus(Status prior, absl::string_view context) {
  CHECK(!prior.ok());
  return Status{prior.code(), absl::StrCat(prior.message(), ": ", context)};
}

std::string Reindent(absl::string_view original,
                     const absl::string_view indentation) {
  std::vector<std::string> pieces =
      absl::StrSplit(absl::string_view(original.data(), original.size()), '\n');
  return absl::StrJoin(
      pieces, "\n", [indentation](std::string* out, absl::string_view s) {
        absl::StrAppend(out, indentation, absl::StripAsciiWhitespace(s));
      });
}

template <typename FloatT>
static void RoundTripNanPayload(FloatT value, std::string* result) {
  static_assert(!std::is_same<FloatT, tsl::float8_e4m3fn>::value,
                "RoundTripNanPayload does not support E4M3");
  static_assert(!std::is_same<FloatT, tsl::float8_e4m3fnuz>::value,
                "RoundTripNanPayload does not support E4M3FNUZ");
  static_assert(!std::is_same<FloatT, tsl::float8_e5m2fnuz>::value,
                "RoundTripNanPayload does not support E5M2FNUZ");
  const int kPayloadBits = NanPayloadBits<FloatT>();
  if (Eigen::numext::isnan(value) && kPayloadBits > 0) {
    auto rep = absl::bit_cast<
        typename UnsignedIntegerTypeForSize<sizeof(FloatT)>::type>(value);
    auto payload = rep & NanPayloadBitMask<FloatT>();
    if (payload != QuietNanWithoutPayload<FloatT>()) {
      absl::StrAppendFormat(result, "(0x%x)", payload);
    }
  }
}

template <typename FloatT>
static std::string GenericRoundTripFpToString(FloatT value) {
  // TODO(majnemer): Remove this temporary variable once Eigen creates a symbol
  // definition for `max_digits10`.
  int max_decimal_digits = std::numeric_limits<FloatT>::max_digits10;
  return absl::StrFormat("%.*g", max_decimal_digits,
                         static_cast<double>(value));
}

std::string RoundTripFpToString(tsl::float8_e5m2 value) {
  std::string result = GenericRoundTripFpToString(value);
  RoundTripNanPayload(value, &result);
  return result;
}

std::string RoundTripFpToString(tsl::float8_e4m3fnuz value) {
  std::string result = GenericRoundTripFpToString(value);
  return result;
}

std::string RoundTripFpToString(tsl::float8_e5m2fnuz value) {
  std::string result = GenericRoundTripFpToString(value);
  return result;
}

std::string RoundTripFpToString(tsl::float8_e4m3fn value) {
  std::string result = GenericRoundTripFpToString(value);
  return result;
}

std::string RoundTripFpToString(tsl::float8_e4m3b11 value) {
  std::string result = GenericRoundTripFpToString(value);
  return result;
}

std::string RoundTripFpToString(bfloat16 value) {
  std::string result = GenericRoundTripFpToString(value);
  RoundTripNanPayload(value, &result);
  return result;
}

std::string RoundTripFpToString(half value) {
  std::string result = GenericRoundTripFpToString(value);
  RoundTripNanPayload(value, &result);
  return result;
}

std::string RoundTripFpToString(float value) {
  float parsed_result;
  std::string result =
      absl::StrFormat("%.*g", std::numeric_limits<float>::digits10, value);
  if (!absl::SimpleAtof(result, &parsed_result) || parsed_result != value) {
    result = GenericRoundTripFpToString(value);
  }
  RoundTripNanPayload(value, &result);
  return result;
}

std::string RoundTripFpToString(double value) {
  double parsed_result;
  std::string result =
      absl::StrFormat("%.*g", std::numeric_limits<double>::digits10, value);
  if (!absl::SimpleAtod(result, &parsed_result) || parsed_result != value) {
    result = GenericRoundTripFpToString(value);
  }
  RoundTripNanPayload(value, &result);
  return result;
}

PaddingConfig MakeNoPaddingConfig(int64_t rank) {
  PaddingConfig padding_config;
  for (int64_t dnum = 0; dnum < rank; ++dnum) {
    auto dimension = padding_config.add_dimensions();
    dimension->set_edge_padding_low(0);
    dimension->set_edge_padding_high(0);
    dimension->set_interior_padding(0);
  }
  return padding_config;
}

PaddingConfig MakeEdgePaddingConfig(
    absl::Span<const std::pair<int64_t, int64_t>> padding) {
  PaddingConfig padding_config;
  for (const std::pair<int64_t, int64_t>& dim : padding) {
    auto dimension = padding_config.add_dimensions();
    dimension->set_edge_padding_low(dim.first);
    dimension->set_edge_padding_high(dim.second);
    dimension->set_interior_padding(0);
  }
  return padding_config;
}

bool HasInteriorPadding(const PaddingConfig& config) {
  for (const auto& dim : config.dimensions()) {
    if (dim.interior_padding() != 0) {
      return true;
    }
  }
  return false;
}

namespace {
std::string HumanReadableNumOps(double flops, double nanoseconds,
                                absl::string_view op_prefix) {
  if (nanoseconds == 0) {
    return absl::StrCat("NaN ", op_prefix, "OP/s");
  }
  double nano_flops = flops / nanoseconds;
  std::string throughput =
      tsl::strings::HumanReadableNum(static_cast<int64_t>(nano_flops * 1e9));
  absl::string_view sp(throughput);
  // Use the more common "G(FLOPS)", rather than "B(FLOPS)"
  if (absl::EndsWith(sp, "B") ||  // Ends in 'B', ignoring case
      absl::EndsWith(sp, "b")) {
    *throughput.rbegin() = 'G';
  }
  throughput += absl::StrCat(op_prefix, "OP/s");
  return throughput;
}
}  // namespace

std::string HumanReadableNumFlops(double flops, double nanoseconds) {
  return HumanReadableNumOps(flops, nanoseconds, "FL");
}

std::string HumanReadableNumTranscendentalOps(double trops,
                                              double nanoseconds) {
  return HumanReadableNumOps(trops, nanoseconds, "TR");
}

void LogLines(int sev, absl::string_view text, const char* fname, int lineno) {
  const int orig_sev = sev;
  if (sev == tsl::FATAL) {
    sev = tsl::ERROR;
  }

  // Protect calls with a mutex so we don't interleave calls to LogLines from
  // multiple threads.
  static absl::Mutex log_lines_mu(absl::kConstInit);
  absl::MutexLock lock(&log_lines_mu);

  size_t cur = 0;
  while (cur < text.size()) {
    size_t eol = text.find('\n', cur);
    if (eol == absl::string_view::npos) {
      eol = text.size();
    }
    auto msg = text.substr(cur, eol - cur);
    tsl::internal::LogString(fname, lineno, sev,
                             std::string(msg.data(), msg.size()));
    cur = eol + 1;
  }

  if (orig_sev == tsl::FATAL) {
    tsl::internal::LogString(fname, lineno, orig_sev,
                             "Aborting due to errors.");
  }
}

int64_t Product(absl::Span<const int64_t> xs) {
  return std::accumulate(xs.begin(), xs.end(), static_cast<int64_t>(1),
                         std::multiplies<int64_t>());
}

absl::InlinedVector<std::pair<int64_t, int64_t>, 8> CommonFactors(
    absl::Span<const int64_t> a, absl::Span<const int64_t> b) {
  CHECK_EQ(Product(a), Product(b));
  absl::InlinedVector<std::pair<int64_t, int64_t>, 8> bounds;
  if (absl::c_equal(a, b)) {
    bounds.reserve(a.size() + 1);
    for (int64_t i = 0; i <= a.size(); ++i) {
      bounds.emplace_back(i, i);
    }
    return bounds;
  }
  int64_t i = 0, j = 0, prior_i = -1, prior_j = -1;
  while (i < a.size() && j < b.size() && a[i] == b[j]) {
    std::tie(prior_i, prior_j) = std::make_pair(i, j);
    bounds.emplace_back(i, j);
    ++i;
    ++j;
  }
  // If the product is different after filtering out zeros, return full group.
  // E.g.,:
  // a={0, 10 ,3}
  //       ^
  //      i=1
  //
  // b={0, 3}
  //       ^
  //      j=1
  if (Product(a.subspan(i)) != Product(b.subspan(j))) {
    return {std::make_pair(0, 0), std::make_pair(a.size(), b.size())};
  }
  if (0 == Product(a.subspan(i))) {
    bounds.push_back(std::make_pair(i, j));
    bounds.push_back(std::make_pair(a.size(), b.size()));
    return bounds;
  }

  for (int64_t partial_size_a = 1, partial_size_b = 1;;) {
    if (partial_size_a == partial_size_b && (i > prior_i || j > prior_j)) {
      std::tie(prior_i, prior_j) = std::make_pair(i, j);
      bounds.emplace_back(i, j);
      continue;
    }
    bool in_bounds_i = i < a.size();
    bool in_bounds_j = j < b.size();
    if (!(in_bounds_i || in_bounds_j)) {
      break;
    }
    bool next_a =
        partial_size_a < partial_size_b ||
        (in_bounds_i &&
         (!in_bounds_j || (partial_size_a == partial_size_b && a[i] <= b[j])));
    bool next_b =
        partial_size_b < partial_size_a ||
        (in_bounds_j &&
         (!in_bounds_i || (partial_size_b == partial_size_a && b[j] <= a[i])));
    if (next_a) {
      partial_size_a *= a[i];
      ++i;
    }
    if (next_b) {
      partial_size_b *= b[j];
      ++j;
    }
  }
  return bounds;
}

ConvertedDimensionNumbers ConvertDimensionNumbers(
    absl::Span<const int64_t> from_dimensions,
    absl::Span<const int64_t> from_sizes, absl::Span<const int64_t> to_sizes) {
  ConvertedDimensionNumbers dimensions;
  auto common_factors = CommonFactors(from_sizes, to_sizes);
  for (int64_t i = 0; i < common_factors.size() - 1; ++i) {
    bool any_present = false;
    bool all_present = true;
    for (int64_t d = common_factors[i].first; d < common_factors[i + 1].first;
         ++d) {
      const bool present = absl::c_linear_search(from_dimensions, d);
      any_present |= present;
      all_present &= present;
    }
    if (all_present) {
      for (int64_t d = common_factors[i].second;
           d < common_factors[i + 1].second; ++d) {
        dimensions.to_dimensions.push_back(d);
      }
      for (int64_t d = common_factors[i].first; d < common_factors[i + 1].first;
           ++d) {
        dimensions.transformed_from_dimensions.push_back(d);
      }
    } else if (any_present) {
      // Try to find if there is a to dimension that is like (from) [2,32] ->
      // (to) [4,4,4] to detect that from dimension 1 can be partially mapped
      // into dimension 1 and 2 of the to sizes with a partial size of 2.
      if (common_factors[i].first + 2 == common_factors[i + 1].first &&
          absl::c_linear_search(from_dimensions, common_factors[i].first + 1)) {
        int64_t from_size = from_sizes[common_factors[i + 1].first - 1];
        bool has_to_dim = false;
        for (int64_t to_dim = common_factors[i + 1].second - 1;
             to_dim >= common_factors[i].second; --to_dim) {
          const int64_t to_size = to_sizes[to_dim];
          if (from_size % to_size == 0) {
            has_to_dim = true;
            from_size /= to_size;
            dimensions.to_dimensions.push_back(to_dim);
          } else {
            break;
          }
        }
        if (has_to_dim) {
          dimensions.split_from_sizes.push_back(from_size);
          dimensions.split_from_dimensions.push_back(common_factors[i].first +
                                                     1);
        }
      }
      for (int64_t d = common_factors[i].first; d < common_factors[i + 1].first;
           ++d) {
        if (absl::c_linear_search(from_dimensions, d)) {
          dimensions.untransformed_from_dimensions.push_back(d);
        }
      }
    }
  }
  absl::c_sort(dimensions.to_dimensions);
  return dimensions;
}
std::string SanitizeFileName(std::string file_name) {
  for (char& c : file_name) {
    if (c == '/' || c == '\\' || c == '[' || c == ']' || c == ' ') {
      c = '_';
    }
  }
  return file_name;
}

bool DistinctNumbersAreConsecutiveIfSorted(absl::Span<const int64_t> seq) {
  return *absl::c_max_element(seq) - *absl::c_min_element(seq) ==
         seq.size() - 1;
}

void PackInt4(absl::Span<const char> input, absl::Span<char> output) {
  CHECK_EQ(output.size(), CeilOfRatio(input.size(), size_t{2}));
  for (size_t i = 0; i < input.size(); ++i) {
    // Mask out the high-order 4 bits in case they have extraneous data.
    char val = input[i] & 0xf;
    if (i % 2 == 0) {
      output[i / 2] = val << 4;
    } else {
      output[i / 2] |= val;
    }
  }
}

void UnpackInt4(absl::Span<const char> input, absl::Span<char> output) {
  CHECK_EQ(input.size(), CeilOfRatio(output.size(), size_t{2}));
  for (size_t i = 0; i < output.size(); ++i) {
    if (i % 2 == 0) {
      output[i] = (input[i / 2] >> 4) & 0xf;
    } else {
      output[i] = input[i / 2] & 0xf;
    }
  }
}

/*static*/ MaybeOwningThreadPool MaybeOwningThreadPool::GetOrCreate(
    int parallelism, tsl::thread::ThreadPool* default_thread_pool,
    int default_parallelism) {
  CHECK_GE(parallelism, 0);
  CHECK_GE(default_parallelism, 1);

  auto create_thread_pool = [&](int num_threads) {
    CHECK_GE(num_threads, 1);
    return std::make_unique<tsl::thread::ThreadPool>(tsl::Env::Default(), "",
                                                     num_threads);
  };

  switch (parallelism) {
    case 0:
      if (default_thread_pool == nullptr && default_parallelism > 1) {
        return MaybeOwningThreadPool(create_thread_pool(default_parallelism));
      }
      return MaybeOwningThreadPool(default_thread_pool);
    case 1:
      return MaybeOwningThreadPool(nullptr);
    default:
      return MaybeOwningThreadPool(create_thread_pool(parallelism));
  }
}

MaybeOwningThreadPool::MaybeOwningThreadPool() : thread_pool_(nullptr) {}

MaybeOwningThreadPool::MaybeOwningThreadPool(
    tsl::thread::ThreadPool* thread_pool)
    : thread_pool_(thread_pool) {}

MaybeOwningThreadPool::MaybeOwningThreadPool(
    std::unique_ptr<tsl::thread::ThreadPool> thread_pool)
    : thread_pool_(std::move(thread_pool)) {}

tsl::thread::ThreadPool* MaybeOwningThreadPool::get() {
  if (std::holds_alternative<tsl::thread::ThreadPool*>(thread_pool_)) {
    return std::get<tsl::thread::ThreadPool*>(thread_pool_);
  }
  return std::get<std::unique_ptr<tsl::thread::ThreadPool>>(thread_pool_).get();
}

const tsl::thread::ThreadPool* MaybeOwningThreadPool::get() const {
  return const_cast<MaybeOwningThreadPool*>(this)->get();
}

tsl::thread::ThreadPool* MaybeOwningThreadPool::operator->() {
  tsl::thread::ThreadPool* thread_pool = get();
  CHECK_NE(thread_pool, nullptr);
  return thread_pool;
}

const tsl::thread::ThreadPool* MaybeOwningThreadPool::operator->() const {
  return const_cast<MaybeOwningThreadPool*>(this)->operator->();
}

MaybeOwningThreadPool::operator bool() const { return get() != nullptr; }

bool MaybeOwningThreadPool::operator!() const { return get() == nullptr; }

}  // namespace xla
