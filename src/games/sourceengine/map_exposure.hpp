#ifndef SRC_GAMES_SOURCEENGINE_MAP_EXPOSURE_HPP_
#define SRC_GAMES_SOURCEENGINE_MAP_EXPOSURE_HPP_

#include <algorithm>
#include <array>
#include <cmath>

namespace map_exposure {

constexpr int BINS_PER_DECADE = 12;
constexpr int BINS = 6 * BINS_PER_DECADE;
constexpr double LOWEST_EDGE = 1e-4;

inline double BinEdge(int index) {
  return LOWEST_EDGE * std::pow(10.0, static_cast<double>(index) / BINS_PER_DECADE);
}

struct Bucket {
  double min;
  double max;
};

inline double DecodeSrgb(double encoded) {
  return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

constexpr int SPREAD = 8;

struct Tables {
  std::array<double, BINS * SPREAD> raw;
  std::array<Bucket, 30> buckets[2];
  int bucket_count[2];
  std::array<double, 30> upper[2][2];
};

inline const Tables& GetTables() {
  static const Tables tables = [] {
    Tables t = {};
    for (int i = 0; i < BINS; ++i) {
      for (int part = 0; part < SPREAD; ++part) {
        t.raw[i * SPREAD + part] = BinEdge(i) * std::pow(10.0, (part + 0.5) / (SPREAD * BINS_PER_DECADE));
      }
    }
    for (int original = 0; original < 2; ++original) {
      t.bucket_count[original] = original ? 30 : 16;
      for (int k = 0; k < t.bucket_count[original]; ++k) {
        const auto edge = [&](int i) {
          return original ? -0.01 + std::exp(std::log(0.01) + (std::log(1.01) - std::log(0.01)) * i / 30.0) : std::pow(i / 16.0, 1.5);
        };
        t.buckets[original][k] = {edge(k), edge(k + 1)};
        t.upper[original][1][k] = edge(k + 1);
        t.upper[original][0][k] = DecodeSrgb(edge(k + 1));
      }
    }
    return t;
  }();
  return tables;
}

inline double FindLocation(const std::array<Bucket, 30>& buckets, const std::array<double, 30>& share, double percent, double snap_percent) {
  double tested_share = 0.0;
  double tested_range = 0.0;
  for (int k = 15; k >= 0; --k) {
    const double needed = percent / 100.0 - tested_share;
    const double range = buckets[k].max - buckets[k].min;
    if (share[k] >= needed && share[k] > 0.0) {
      if (snap_percent >= 0.0 && buckets[k].min <= snap_percent / 100.0 && snap_percent / 100.0 <= buckets[k].max) return snap_percent / 100.0;
      const double location = 1.0 - (tested_range + range * needed / share[k]);
      return std::clamp(location, buckets[k].min, buckets[k].max);
    }
    tested_share += share[k];
    tested_range += range;
  }
  return -1.0;
}

inline double Target(const std::array<double, BINS>& bins, double exposure, bool read_linear, bool original_algorithm) {
  const Tables& tables = GetTables();
  const auto& buckets = tables.buckets[original_algorithm];
  const auto& upper = tables.upper[original_algorithm][read_linear];
  const int bucket_count = tables.bucket_count[original_algorithm];
  std::array<double, 30> share = {};
  int k = 0;
  for (int sample = 0; sample < BINS * SPREAD; ++sample) {
    const double weight = bins[sample / SPREAD];
    if (weight <= 0.0) continue;
    const double luminance = std::min(tables.raw[sample] * exposure, 1.0);
    while (k + 1 < bucket_count && luminance > upper[k]) ++k;
    share[k] += weight / SPREAD;
  }

  if (original_algorithm) {
    double mean = 0.0;
    for (int b = 0; b < bucket_count; ++b) mean += share[b] * (buckets[b].min + buckets[b].max) * 0.5;
    return 0.15 / std::max(mean, 0.0001 * 30.0);
  }

  double location = FindLocation(buckets, share, 2.0, 60.0);
  if (location < 0.0) location = 0.6;
  double target = 0.6 / std::max(location, 0.0001);
  const double median = FindLocation(buckets, share, 50.0, -1.0);
  if (median > 0.0) target = std::max(target, 0.03 / median);
  return std::max(target * exposure, 0.001);
}

inline double Settle(const std::array<double, BINS>& bins, bool read_linear, bool original_algorithm, double minimum, double maximum) {
  double exposure = std::clamp(1.0, minimum, maximum);
  for (int step = 0; step < 400; ++step) {
    const double goal = std::clamp(Target(bins, exposure, read_linear, original_algorithm), minimum, maximum);
    const double change = (goal - exposure) * 0.05;
    exposure += change;
    if (std::abs(change) < 1e-6 * exposure) break;
  }
  return exposure;
}

}

#endif
