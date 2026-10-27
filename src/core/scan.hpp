// SPDX-License-Identifier: GPL-2.0-or-later
// Collects the pair records of a clip while frames are produced in any order and by several threads.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "core/exblend.hpp"

namespace exblend {

class ScanState {
 public:
  ScanState(int frames, const Geometry& g, std::string path)
      : path_(std::move(path)), claimed_(new std::atomic<uint8_t>[static_cast<size_t>(frames) + 3]),
        remaining_(frames + 3) {
    stats_.frames = frames;
    stats_.geom = g;
    stats_.pairs.resize(static_cast<size_t>(frames) + 3);
    for (int i = 0; i < frames + 3; ++i) claimed_[i] = 0;
    for (auto& h : hash_) h = 0;
  }

  int frames() const { return stats_.frames; }
  const Geometry& geom() const { return stats_.geom; }

  // Pairs computed when frame n is produced: every pair exactly once over the whole clip.
  void pairsOf(int n, int& first, int& last) const {
    const int N = stats_.frames;
    first = n == 0 ? -2 : n;
    last = n == N - 1 ? N : n;
  }

  // Frames whose luma identifies the clip.
  int fingerprintFrame(int i) const { return i == 0 ? 0 : i == 1 ? stats_.frames / 2 : stats_.frames - 1; }

  void setFingerprint(int n, uint64_t h) {
    for (int i = 0; i < 3; ++i)
      if (fingerprintFrame(i) == n) hash_[i] = h;
  }

  // True when the caller should compute pair p.
  bool claim(int p) { return claimed_[p + 2].exchange(1) == 0; }

  // Stores a claimed pair; returns true for the call that completed the clip.
  bool store(int p, const PairRecord& r) {
    stats_.pairs[static_cast<size_t>(p + 2)] = r;
    return remaining_.fetch_sub(1) == 1;
  }

  const PairRecord& record(int p) const { return stats_.pairs[static_cast<size_t>(p + 2)]; }

  bool complete() const { return remaining_.load() == 0; }

  // Final statistics; valid once complete() is true.
  const Stats& stats() {
    stats_.fingerprint = combineFingerprint(hash_[0], hash_[1], hash_[2]);
    return stats_;
  }

  const std::string& path() const { return path_; }

  static uint64_t combineFingerprint(uint64_t a, uint64_t b, uint64_t c) {
    return a ^ ((b << 21) | (b >> 43)) ^ ((c << 42) | (c >> 22));
  }

  bool writeOnce(std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (written_ || path_.empty()) return true;
    written_ = saveStats(path_, stats(), err);
    return written_;
  }

 private:
  std::string path_;
  Stats stats_;
  std::unique_ptr<std::atomic<uint8_t>[]> claimed_;
  std::atomic<int> remaining_;
  std::atomic<uint64_t> hash_[3];
  std::mutex mutex_;
  bool written_ = false;
};

}  // namespace exblend
