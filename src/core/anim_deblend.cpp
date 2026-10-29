// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/anim_deblend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace animdeblend {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// cadence: blend ratio below which a frame on the cadence pays off / that it must reach to be treated
constexpr double kCadenceTheta = 0.6;
constexpr double kCadenceAccept = 0.8;
constexpr double kCadenceClean = 0.75;  // clean frames of a two-blend cadence must exceed this ratio
constexpr int kCadencePeriodMin = 2, kCadencePeriodMax = 8;
// a single blend off any cadence: clear blend, neighbours no blends, different drawings, mostly one of them
constexpr double kIsolatedQ = 0.5, kIsolatedNeighbour = 0.45, kIsolatedCopy = 8.0, kIsolatedSkew = 0.1;
// gap filling: run periods, missing frames filled from context, detections needed beside a gap / before a run end
constexpr int kFillPeriodMin = 2, kFillPeriodMax = 4, kFillMaxHole = 3, kFillSide = 2, kFillEnd = 3;
constexpr double kFillRatio = 0.75, kFillNoise = 4.0;
// ghosts: ratio histogram bins, accepted weights, peak sharpness / prominence, ghost gone after unblending, no fade
constexpr double kGhostFMin = 0.08, kGhostFMax = 0.6, kGhostSharp = 1.8, kGhostProminence = 1.4, kGhostGone = 0.05;
constexpr double kGhostMaxR2 = 0.95;

struct Pair {
  int da, db;
};
constexpr Pair kCadencePairs[3] = {{-1, 1}, {-1, 2}, {-2, 1}};

bool contains(const Blends& m, int k) { return m.find(k) != m.end(); }

int pyMod(int a, int b) { return ((a % b) + b) % b; }

}  // namespace

double meanSquaredDiff(const Frame& x, const Frame& y) {
  double s = 0.0;
  const size_t n = x.px.size();
  for (size_t i = 0; i < n; ++i) {
    const double d = static_cast<double>(x.px[i]) - y.px[i];
    s += d * d;
  }
  return n ? s / static_cast<double>(n) : 0.0;
}

double blendFit(const Frame& o, const Frame& a, const Frame& b, double& r) {
  const size_t n = o.px.size();
  double dd = 0.0, od = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double d = static_cast<double>(b.px[i]) - a.px[i];
    dd += d * d;
    od += (static_cast<double>(o.px[i]) - a.px[i]) * d;
  }
  if (dd < 1e-6 * static_cast<double>(n)) {
    r = meanSquaredDiff(o, a);
    return 0.0;
  }
  const double f = std::min(std::max(od / dd, 0.0), 1.0);
  double s = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double e = static_cast<double>(o.px[i]) - (a.px[i] + f * (static_cast<double>(b.px[i]) - a.px[i]));
    s += e * e;
  }
  r = n ? s / static_cast<double>(n) : 0.0;
  return f;
}

double noiseFromSteps(std::vector<double> steps) {
  if (steps.empty()) return 0.0;
  std::sort(steps.begin(), steps.end());
  return 0.5 * steps[steps.size() / 10];
}

std::vector<int> noiseSamplePositions(int frames, int count) {
  std::vector<int> pos;
  const int m = std::min(count, frames - 1);
  if (m <= 0) return pos;
  for (int i = 0; i < m; ++i) {
    const double v = m == 1 ? 1.0 : 1.0 + static_cast<double>(i) * (frames - 2) / (m - 1);
    pos.push_back(static_cast<int>(v));
  }
  return pos;
}

Blends analyseAnchor(const std::vector<Frame>& frames, const Params& p, double var) {
  const int n = static_cast<int>(frames.size());
  const double tau = p.kappa * std::max(var, 0.25);
  std::vector<double> dp(static_cast<size_t>(n), kInf);
  std::vector<int> back(static_cast<size_t>(n), -1);
  std::vector<std::vector<double>> weights(static_cast<size_t>(n));
  // an anchor costs tau, a blend frame its residual
  for (int b = 0; b < n; ++b) {
    if (b == 0) {
      dp[0] = tau;
      continue;
    }
    for (int a = std::max(0, b - p.maxgap); a < b; ++a) {
      if (dp[a] == kInf) continue;
      double cost = 0.0;
      std::vector<double> fs;
      bool ok = true;
      for (int k = a + 1; k < b; ++k) {
        double r;
        const double f = blendFit(frames[k], frames[a], frames[b], r);
        const double copy = std::min(meanSquaredDiff(frames[k], frames[a]), meanSquaredDiff(frames[k], frames[b]));
        if (f < p.minweight || f > 1.0 - p.minweight || (!fs.empty() && f < fs.back()) ||
            r - 1.5 * var > p.rho * std::max(copy - 2.0 * var, 0.0)) {
          ok = false;
          break;
        }
        fs.push_back(f);
        cost += r;
      }
      if (ok && dp[a] + cost + tau < dp[b]) {
        dp[b] = dp[a] + cost + tau;
        back[b] = a;
        weights[b] = fs;
      }
    }
  }
  Blends blends;
  int b = n - 1;
  while (b > 0) {
    const int a = back[b];
    for (int k = a + 1; k < b; ++k) blends[k] = {a, b, weights[b][static_cast<size_t>(k - a - 1)]};
    b = a;
  }
  return blends;
}

namespace {

struct CadenceState {
  int period, phase, length;
};

bool cadenceOn(int k, const CadenceState& s) { return s.period > 0 && pyMod(k - s.phase, s.period) < s.length; }

// Cheapest state per frame: 'no cadence' or (P, phase, L) with L consecutive blends per period P.
std::vector<CadenceState> cadencePath(const std::vector<double>& q, double switchCost) {
  std::vector<CadenceState> st{{0, 0, 0}};
  // at most two blends per period: with longer groups the path could hop phases to dodge the few clean frames
  for (int period = kCadencePeriodMin; period <= kCadencePeriodMax; ++period)
    for (int phase = 0; phase < period; ++phase)
      for (int length = 1; length < std::min(period, 3); ++length) st.push_back({period, phase, length});
  const int n = static_cast<int>(q.size()), m = static_cast<int>(st.size());
  std::vector<CadenceState> path;
  if (n == 0) return path;
  auto cost = [&](int k, int j) {
    const CadenceState& s = st[static_cast<size_t>(j)];
    if (s.period == 0) return 0.0;
    const double qq = std::isfinite(q[k]) ? std::min(q[k], 1.5) : 1.5;
    // on the cadence a clear blend pays off; off the cadence a frame that looks like a blend argues against it
    if (cadenceOn(k, s)) return qq - kCadenceTheta;
    if (s.length > 1) {
      // two blends per period need clean frames that are clearly no mix; a frame without a measurement is no proof
      const double measured = std::isfinite(q[k]) ? qq : kCadenceTheta;
      return std::max(0.0, kCadenceClean - measured) * 3.0 * s.length / (s.period - s.length);
    }
    return std::max(0.0, kCadenceTheta - qq);
  };
  std::vector<double> acc(static_cast<size_t>(m)), next(static_cast<size_t>(m));
  std::vector<int> back(static_cast<size_t>(n) * m);
  for (int j = 0; j < m; ++j) acc[j] = cost(0, j);
  for (int k = 1; k < n; ++k) {
    const int best = static_cast<int>(std::min_element(acc.begin(), acc.end()) - acc.begin());
    const double viaSwitch = acc[best] + switchCost;
    for (int j = 0; j < m; ++j) {
      const bool sw = viaSwitch < acc[j];
      back[static_cast<size_t>(k) * m + j] = sw ? best : j;
      next[j] = (sw ? viaSwitch : acc[j]) + cost(k, j);
    }
    acc.swap(next);
  }
  std::vector<int> idx(static_cast<size_t>(n));
  idx[n - 1] = static_cast<int>(std::min_element(acc.begin(), acc.end()) - acc.begin());
  for (int k = n - 1; k > 0; --k) idx[k - 1] = back[static_cast<size_t>(k) * m + idx[k]];
  path.reserve(static_cast<size_t>(n));
  for (int j : idx) path.push_back(st[static_cast<size_t>(j)]);
  return path;
}

}  // namespace

Blends analyseCadence(const std::vector<Frame>& frames, const Params& p, double var) {
  const int n = static_cast<int>(frames.size());
  std::vector<double> q(static_cast<size_t>(n), kInf), copies(static_cast<size_t>(n), 0.0);
  std::vector<Blend> pick(static_cast<size_t>(n));
  // per frame the best anchor pair gives the blend ratio q = (resid - 1.5 var) / (copy - 2 var)
  for (int k = 2; k < n; ++k) {
    for (const Pair& pr : kCadencePairs) {
      const int ia = k + pr.da, ib = k + pr.db;
      if (ia < 0 || ib >= n) continue;
      double r;
      const double f = blendFit(frames[k], frames[ia], frames[ib], r);
      const double copy = std::min(meanSquaredDiff(frames[k], frames[ia]), meanSquaredDiff(frames[k], frames[ib]));
      if (f < p.minweight || f > 1.0 - p.minweight || copy - 2.0 * var <= 3.0 * std::max(var, 0.25)) continue;
      const double ratio = (r - 1.5 * var) / (copy - 2.0 * var);
      if (ratio < q[k]) {
        q[k] = ratio;
        copies[k] = copy;
        pick[k] = {ia, ib, f};
      }
    }
  }
  Blends blends;
  const std::vector<CadenceState> path = cadencePath(q, p.switchCost);
  for (int k = 0; k < n; ++k) {
    if (cadenceOn(k, path[static_cast<size_t>(k)]) && q[k] <= kCadenceAccept) {
      blends[k] = pick[k];
    } else if (k > 0 && k < n - 1 && q[k] <= kIsolatedQ && q[k - 1] > kIsolatedNeighbour &&
               q[k + 1] > kIsolatedNeighbour && copies[k] - 2.0 * var >= kIsolatedCopy &&
               std::abs(pick[k].f - 0.5) >= kIsolatedSkew) {
      blends[k] = pick[k];
    }
  }
  return blends;
}

namespace {

// Period of the regular run of blends (2-4) whose gap k fills or whose end k extends, or 0.
int onRun(int k, const Blends& blends, int n) {
  for (int period = kFillPeriodMin; period <= kFillPeriodMax; ++period) {
    auto count = [&](int start, int step) {
      int c = 0;
      for (int j = start; j >= 0 && j < n && contains(blends, j); j += step) ++c;
      return c;
    };
    int holesBefore = 0;
    while (holesBefore < kFillMaxHole - 1 && k - period * (holesBefore + 1) >= 0 &&
           !contains(blends, k - period * (holesBefore + 1)))
      ++holesBefore;
    if (contains(blends, k - period)) holesBefore = 0;
    int holesAfter = 0;
    while (holesAfter < kFillMaxHole - 1 && k + period * (holesAfter + 1) < n &&
           !contains(blends, k + period * (holesAfter + 1)))
      ++holesAfter;
    if (contains(blends, k + period)) holesAfter = 0;
    const int before = count(k - period * (holesBefore + 1), -period);
    const int after = count(k + period * (holesAfter + 1), period);
    if (before >= kFillSide && after >= kFillSide && holesBefore + holesAfter + 1 <= kFillMaxHole) return period;
    if (holesBefore == 0 && before >= kFillEnd && !contains(blends, k + period)) return period;
    if (holesAfter == 0 && after >= kFillEnd && !contains(blends, k - period)) return period;
  }
  return 0;
}

}  // namespace

Blends fillGaps(const std::vector<Frame>& frames, const Blends& blends, double var) {
  const int n = static_cast<int>(frames.size());
  Blends out = blends;
  var = std::max(var, 0.25);
  // (f, residual / copy) of k against its direct neighbours, or false if no usable mix
  auto pairFit = [&](int k, double& f, double& ratio) {
    double r;
    f = blendFit(frames[k], frames[k - 1], frames[k + 1], r);
    const double copy = std::min(meanSquaredDiff(frames[k], frames[k - 1]), meanSquaredDiff(frames[k], frames[k + 1]));
    if (f < 0.02 || f > 0.98 || r >= copy) return false;
    if (r - 1.5 * var > kFillRatio * std::max(copy - 2.0 * var, 0.0) && r > kFillNoise * var) return false;
    ratio = r / copy;
    return true;
  };
  for (int k = 1; k < n - 1; ++k) {
    const int period = contains(blends, k) ? 0 : onRun(k, blends, n);
    if (!period) continue;
    double f, ratio;
    if (!pairFit(k, f, ratio)) continue;
    // a blend next to the gap that took it as an anchor and has no partner on the run's lattice sits off the run
    for (int j : {k - 1, k + 1}) {
      auto it = out.find(j);
      if (it == out.end() || (it->second.a != k && it->second.b != k) || contains(blends, j - period) ||
          contains(blends, j + period))
        continue;
      double of = 0.0, oratio = 0.0;
      const bool other = j > 0 && j < n - 1 && pairFit(j, of, oratio);
      if (!other || ratio < oratio) out.erase(it);
    }
    if (!contains(out, k - 1) && !contains(out, k + 1)) out[k] = {k - 1, k + 1, f};
  }
  // run frames whose anchor turned out to be a blend: fit again against the direct neighbours
  for (auto& kv : out) {
    const int k = kv.first;
    if ((contains(out, kv.second.a) || contains(out, kv.second.b)) && k > 0 && k < n - 1 && !contains(out, k - 1) &&
        !contains(out, k + 1)) {
      double f, ratio;
      if (pairFit(k, f, ratio)) kv.second = {k - 1, k + 1, f};
    }
  }
  return out;
}

Blends analysePart(const std::vector<Frame>& frames, int start, int keep, int keepEnd, const Params& p, double var) {
  Blends found;
  if (p.mode == Mode::Anchor || p.mode == Mode::Both)
    for (const auto& kv : analyseAnchor(frames, p, var)) found[kv.first] = kv.second;
  if (p.mode == Mode::Cadence || p.mode == Mode::Both)
    for (const auto& kv : analyseCadence(frames, p, var)) found[kv.first] = kv.second;
  if (p.fill) found = fillGaps(frames, found, var);
  Blends out;
  for (const auto& kv : found) {
    const int k = kv.first + start;
    if (k >= keep && k < keepEnd) out[k] = {kv.second.a + start, kv.second.b + start, kv.second.f};
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Ghosts

namespace {

// Bin edges -0.3, -0.28, ..., 0.70 (numpy.arange(-0.3, 0.71, 0.02)).
const std::vector<double>& ghostEdges() {
  static const std::vector<double> edges = [] {
    std::vector<double> e;
    for (int i = 0; i < 51; ++i) e.push_back(-0.3 + i * 0.02);
    return e;
  }();
  return edges;
}

uint32_t absBits(float v) {
  uint32_t u;
  std::memcpy(&u, &v, sizeof u);
  return u & 0x7fffffffu;  // the bits of a non-negative float order like the value
}

// Linear-interpolation percentile of the absolute values (numpy.percentile), exact: the values are counted by their
// top 16 bits, then only the bucket holding the wanted ranks is sorted.
double percentileAbs(const std::vector<float>& g, double pct) {
  if (g.empty()) return 0.0;
  const double pos = pct / 100.0 * static_cast<double>(g.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(pos));
  const size_t hi = std::min(lo + 1, g.size() - 1);
  thread_local std::vector<uint32_t> counts;
  counts.assign(1u << 16, 0);
  for (float v : g) ++counts[absBits(v) >> 15];
  auto valueAt = [&](size_t rank) {
    size_t below = 0;
    uint32_t bucket = 0;
    while (below + counts[bucket] <= rank) below += counts[bucket++];
    thread_local std::vector<float> in;
    in.clear();
    for (float v : g)
      if ((absBits(v) >> 15) == bucket) in.push_back(std::abs(v));
    std::nth_element(in.begin(), in.begin() + static_cast<ptrdiff_t>(rank - below), in.end());
    return static_cast<double>(in[rank - below]);
  };
  const double vlo = valueAt(lo);
  const double vhi = hi == lo ? vlo : valueAt(hi);
  const double t = pos - static_cast<double>(lo);
  return t >= 0.5 ? vhi - (vhi - vlo) * (1.0 - t) : vlo + (vhi - vlo) * t;
}

// Histogram of the ratios over the bins (last bin closed), smoothed with 1 2 3 2 1.
std::vector<double> ghostHist(const std::vector<float>& r) {
  const std::vector<double>& e = ghostEdges();
  const int bins = static_cast<int>(e.size()) - 1;
  std::vector<double> h(static_cast<size_t>(bins), 0.0);
  for (float v : r) {
    const double x = v;
    if (!(x >= e.front() && x <= e.back())) continue;
    int i = static_cast<int>(std::upper_bound(e.begin(), e.end(), x) - e.begin()) - 1;
    if (i >= bins) i = bins - 1;
    h[static_cast<size_t>(i)] += 1.0;
  }
  static const double kernel[5] = {1, 2, 3, 2, 1};
  std::vector<double> s(static_cast<size_t>(bins), 0.0);
  for (int i = 0; i < bins; ++i)
    for (int j = -2; j <= 2; ++j)
      if (i + j >= 0 && i + j < bins) s[static_cast<size_t>(i)] += kernel[j + 2] * h[static_cast<size_t>(i + j)];
  return s;
}

double binCentre(int i) { return 0.5 * (ghostEdges()[static_cast<size_t>(i)] + ghostEdges()[static_cast<size_t>(i) + 1]); }

int argmaxFirst(const std::vector<double>& h) {
  return static_cast<int>(std::max_element(h.begin(), h.end()) - h.begin());
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

Frame boxBlur(const Frame& in) {
  const int w = in.width, h = in.height;
  Frame a = in, b = in;
  for (int pass = 0; pass < 2; ++pass) {
    for (int y = 0; y < h; ++y) {
      const float* s = &a.px[static_cast<size_t>(y) * w];
      float* d = &b.px[static_cast<size_t>(y) * w];
      for (int x = 0; x < w; ++x) d[x] = (s[std::max(x - 1, 0)] + s[x] + s[std::min(x + 1, w - 1)]) / 3.0f;
    }
    std::swap(a, b);
  }
  for (int pass = 0; pass < 2; ++pass) {
    for (int y = 0; y < h; ++y) {
      const float* up = &a.px[static_cast<size_t>(std::max(y - 1, 0)) * w];
      const float* mid = &a.px[static_cast<size_t>(y) * w];
      const float* dn = &a.px[static_cast<size_t>(std::min(y + 1, h - 1)) * w];
      float* d = &b.px[static_cast<size_t>(y) * w];
      for (int x = 0; x < w; ++x) d[x] = (up[x] + mid[x] + dn[x]) / 3.0f;
    }
    std::swap(a, b);
  }
  return a;
}

GhostFrame::GhostFrame(Frame r, const Frame& smooth) : raw(std::move(r)), width(raw.width), height(raw.height) {
  double s = 0.0;
  for (float v : raw.px) s += v;
  const double n = static_cast<double>(raw.px.size());
  mean = n ? s / n : 0.0;
  double sv = 0.0;
  for (float v : raw.px) sv += (v - mean) * (v - mean);
  var = n ? sv / n : 0.0;
  const int w = smooth.width, h = smooth.height;
  const float* sp = smooth.px.data();
  grad[0].resize(static_cast<size_t>(w) * (h - 1));
  for (int y = 0; y + 1 < h; ++y)
    for (int x = 0; x < w; ++x)
      grad[0][static_cast<size_t>(y) * w + x] = sp[static_cast<size_t>(y + 1) * w + x] - sp[static_cast<size_t>(y) * w + x];
  grad[1].resize(static_cast<size_t>(w - 1) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x + 1 < w; ++x)
      grad[1][static_cast<size_t>(y) * (w - 1) + x] = sp[static_cast<size_t>(y) * w + x + 1] - sp[static_cast<size_t>(y) * w + x];
  for (int a = 0; a < 2; ++a) {
    const double thr = percentileAbs(grad[a], 95.0);  // the strongest 5 % of the edges
    mask[a].resize(grad[a].size());
    for (size_t i = 0; i < grad[a].size(); ++i) mask[a][i] = std::abs(grad[a][i]) > thr;
  }
}

bool ghostOf(const GhostFrame& o, const GhostFrame& n, double& f, double& sharpness) {
  if (n.var < 1e-6 || o.var < 1e-6) return false;  // flat frame (black): nothing to unblend
  // a frame explained by a N + b (fade, near hold) has no new drawing to recover
  double sp = 0.0;
  const size_t count = o.raw.px.size();
  for (size_t i = 0; i < count; ++i) sp += static_cast<double>(o.raw.px[i]) * n.raw.px[i];
  const double cov = sp / static_cast<double>(count) - o.mean * n.mean;
  if (cov * cov / (n.var * o.var) >= kGhostMaxR2) return false;
  // where o's own drawing is flat, o's edges are exactly f times n's
  thread_local std::vector<float> r;
  r.clear();
  for (int a = 0; a < 2; ++a)
    for (size_t i = 0; i < n.grad[a].size(); ++i)
      if (n.mask[a][i]) r.push_back(o.grad[a][i] / n.grad[a][i]);
  const std::vector<double> h = ghostHist(r);
  const int i = argmaxFirst(h);
  f = binCentre(i);
  if (f < kGhostFMin || f > kGhostFMax) return false;
  sharpness = h[static_cast<size_t>(i)] / std::max(median(h), 1.0);
  double zero = 0.0;
  for (int j = 0; j < static_cast<int>(h.size()); ++j)
    if (std::abs(binCentre(j)) < 0.03) zero = std::max(zero, h[static_cast<size_t>(j)]);
  const double prominence = h[static_cast<size_t>(i)] / std::max(zero, 1.0);
  if (sharpness < kGhostSharp || prominence < kGhostProminence) return false;
  // after unblending, X's edges at n's edges have the ratio (r - f) / (1 - f); the ghost must be gone
  const float ff = static_cast<float>(f), den = static_cast<float>(1.0 - f);
  for (float& v : r) v = (v - ff) / den;
  return std::abs(binCentre(argmaxFirst(ghostHist(r)))) <= kGhostGone;
}

Ghosts analyseGhosts(const std::vector<GhostFrame>& frames, int start) {
  Ghosts ghosts;
  const int n = static_cast<int>(frames.size());
  for (int k = 1; k + 1 < n; ++k) {
    bool found = false;
    Ghost best;
    double bestSharp = 0.0;
    for (int nb : {k - 1, k + 1}) {
      double f, sharp;
      if (ghostOf(frames[k], frames[nb], f, sharp) && (!found || sharp > bestSharp)) {
        found = true;
        best = {nb + start, f};
        bestSharp = sharp;
      }
    }
    if (found) ghosts[k + start] = best;
  }
  return ghosts;
}

}  // namespace animdeblend
