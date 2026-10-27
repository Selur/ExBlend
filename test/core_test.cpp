// SPDX-License-Identifier: GPL-2.0-or-later
// Checks the pair records against the 7-frame window of ExBlend v1.04 Findblend() on random 8-bit frames.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "core/exblend.hpp"

using namespace exblend;

namespace {

struct Window {
  float md10_23, md21_34, md32_45, md43_56, fd23, fd34, ld25;
};

// Accumulation of v1.04 Findblend() (planar), reduced to the values the detector uses.
Window reference(const std::vector<std::vector<uint8_t>>& f, int w, int h) {
  float W_FD_23 = 0, W_FD_34 = 0, W_MD_10_23 = 0, W_MD_21_34 = 0, W_MD_32_45 = 0, W_MD_43_56 = 0, W_LD_25 = 0;
  auto cl = [](int v) { return v < 20 ? 20 : v > 235 ? 235 : v; };
  for (int y = 0; y < h - 1; y += 2) {
    for (int x = 0; x < w; ++x) {
      int p[7];
      for (int i = 0; i < 7; ++i) p[i] = cl(f[i][y * w + x]) + cl(f[i][(y + 1) * w + x]);
      const int FD_23 = std::abs(p[2] - p[3]), FD_34 = std::abs(p[3] - p[4]), LD_25 = std::abs(p[2] - p[5]);
      const int UP_10 = p[1] * 2 - p[0], UP_23 = p[2] * 2 - p[3], UP_21 = p[2] * 2 - p[1], UP_34 = p[3] * 2 - p[4];
      const int UP_32 = p[3] * 2 - p[2], UP_45 = p[4] * 2 - p[5], UP_43 = p[4] * 2 - p[3], UP_56 = p[5] * 2 - p[6];
      int MD_10_23 = std::abs(UP_10 - UP_23) / 2, MD_21_34 = std::abs(UP_21 - UP_34) / 2;
      int MD_32_45 = std::abs(UP_32 - UP_45) / 2, MD_43_56 = std::abs(UP_43 - UP_56) / 2;
      const int Y_LIM = 80, Y_BAD = 470, Y_MIN = 4, Y_MAX = 498;
      if (MD_10_23 > Y_LIM || UP_10 < Y_MIN || UP_10 > Y_MAX || UP_23 < Y_MIN || UP_23 > Y_MAX) MD_10_23 = Y_BAD;
      if (MD_21_34 > Y_LIM || UP_21 < Y_MIN || UP_21 > Y_MAX || UP_34 < Y_MIN || UP_34 > Y_MAX) MD_21_34 = Y_BAD;
      if (MD_32_45 > Y_LIM || UP_32 < Y_MIN || UP_32 > Y_MAX || UP_45 < Y_MIN || UP_45 > Y_MAX) MD_32_45 = Y_BAD;
      if (MD_43_56 > Y_LIM || UP_43 < Y_MIN || UP_43 > Y_MAX || UP_56 < Y_MIN || UP_56 > Y_MAX) MD_43_56 = Y_BAD;
      W_FD_23 += FD_23;
      W_FD_34 += FD_34;
      W_MD_10_23 += MD_10_23;
      W_MD_21_34 += MD_21_34;
      W_MD_32_45 += MD_32_45;
      W_MD_43_56 += MD_43_56;
      W_LD_25 += LD_25;
    }
  }
  W_FD_23 *= 0.5;
  W_FD_34 *= 0.5;
  W_MD_10_23 *= 0.5;
  W_MD_21_34 *= 0.5;
  W_MD_32_45 *= 0.5;
  W_MD_43_56 *= 0.5;
  W_LD_25 *= 0.5;
  const float mx = w * (h / 2) * 235.0f;
  return {W_MD_10_23 * 100.0f / mx, W_MD_21_34 * 100.0f / mx, W_MD_32_45 * 100.0f / mx, W_MD_43_56 * 100.0f / mx,
          W_FD_23 * 100.0f / mx,    W_FD_34 * 100.0f / mx,    W_LD_25 * 100.0f / mx};
}

int failures = 0;

void expect(bool ok, const char* what, int round) {
  if (!ok) {
    ++failures;
    std::printf("FAIL round %d: %s\n", round, what);
  }
}

bool isDropAt(const Sequence& s, int k) { return (s.fdat[static_cast<size_t>(k)] & DROP_MASK) != 0; }

// Pair records with blend pairs at `pairs` and a strong pair at `strong` inside the hints of the last earlier pair.
Stats rephaseStats(std::initializer_list<int> pairs, int strong) {
  const double unit = 100.0 * 50 * 235 / 50.0;  // sum per metric unit
  Stats st;
  st.geom = {100, 100, 8};
  st.frames = 60;
  st.pairs.resize(static_cast<size_t>(st.frames) + 3);
  auto set = [&](int p, double md, double fd) {
    PairRecord& r = st.pairs[static_cast<size_t>(p + 2)];
    r = {static_cast<uint64_t>(md * unit), static_cast<uint64_t>(fd * unit), static_cast<uint64_t>(40 * unit),
         static_cast<float>(md), static_cast<float>(fd), 40.0f};
  };
  for (int p = -2; p <= st.frames; ++p) set(p, 5, 5);
  for (int p : pairs) set(p, 0.05, 5);
  set(strong, 0.05, 9);
  return st;
}

int indexAt(const Stats& st, bool compat, int k, Sequence* out = nullptr) {
  Options o;
  o.compat = compat;
  Sequence s;
  std::string err;
  expect(buildSequence(st, o, s, err), "rephase case builds", compat);
  if (out) *out = s;
  return s.fdat[static_cast<size_t>(k)] & IX_MASK;
}

void rephaseCases() {
  // Wrong phase: 23 is no cycle after 15, the strong pair at 26 takes over (not with compat).
  const Stats off = rephaseStats({10, 15, 23, 31, 36, 41, 46}, 26);
  Sequence s;
  expect(indexAt(off, false, 26, &s) == IX1 && (s.fdat[27] & IX_MASK) == IX2 && (s.fdat[23] & IX_MASK) == IXH,
         "strong hinted pair re-phases an off-rhythm pair", 0);
  expect(isDropAt(s, 27) && !isDropAt(s, 25), "decimation follows the new phase", 0);
  expect(indexAt(off, true, 23) == IX1 && indexAt(off, true, 26) == IXH, "compat keeps the first phase", 1);
  // 25 follows 20 by one cycle: the pattern confirms it, no re-phase at 27.
  const Stats on = rephaseStats({10, 15, 20, 25, 32, 37, 42, 47}, 27);
  expect(indexAt(on, false, 25) == IX1 && indexAt(on, false, 27) == IXH, "pair on the cycle rhythm stays", 0);
}

}  // namespace

int main() {
  rephaseCases();
  std::mt19937 rng(12345);
  for (int round = 0; round < 40; ++round) {
    // Large frames push the float sums past 2^24 so rounding of the v1.04 accumulation is exercised.
    const int w = round < 20 ? 64 + round * 7 : 720 + round, h = round < 20 ? 48 + round * 2 : 480 + (round & 1);
    std::vector<std::vector<uint8_t>> f(7, std::vector<uint8_t>(static_cast<size_t>(w) * h));
    std::uniform_int_distribution<int> dist(0, 255), small(-6, 6);
    for (int i = 0; i < 7; ++i)
      for (size_t k = 0; k < f[i].size(); ++k)
        f[i][k] = static_cast<uint8_t>(round % 3 == 0 ? dist(rng) : std::min(255, std::max(0, 120 + i * 3 + small(rng))));
    const Window ref = reference(f, w, h);
    const Geometry g{w, h, 8};
    auto pair = [&](int p) {
      PlaneRef planes[4];
      for (int i = 0; i < 4; ++i) planes[i] = {f[static_cast<size_t>(p + i)].data(), w};
      PairRecord r;
      computePair(planes, g, r);
      return r;
    };
    // Window frames 0..6 are n-3..n+3; pair (a, a+1) uses frames a-1..a+2, so pair n-2 starts at index 0.
    const PairRecord p10 = pair(0), p21 = pair(1), p32 = pair(2), p43 = pair(3);
    expect(p10.cmd == ref.md10_23, "MD_10_23", round);
    expect(p21.cmd == ref.md21_34, "MD_21_34", round);
    expect(p32.cmd == ref.md32_45, "MD_32_45", round);
    expect(p43.cmd == ref.md43_56, "MD_43_56", round);
    expect(p21.cfd == ref.fd23, "FD_23", round);
    expect(p32.cfd == ref.fd34, "FD_34", round);
    expect(p32.cld == ref.ld25, "LD_25", round);
    const double rel = std::fabs(scaleSum(p32.md, g) - p32.cmd) / std::max(1e-9, scaleSum(p32.md, g));
    expect(rel < 0.05, "exact sum close to float sum", round);
  }
  std::printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
  return failures ? 1 : 0;
}
