// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/exblend.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace exblend {

namespace {

// Detection constants of v1.04 in 8-bit units of the summed vertical pixel pair.
constexpr int kClampMin = 20;
constexpr int kClampMax = 235;
constexpr int kGap = 14 * 2;
constexpr int kLim = 40 * 2;
constexpr int kBad = 235 * 2;
constexpr int kMin = 16 * 2 - kGap;
constexpr int kMax = 235 * 2 + kGap;

template <typename P>
void computePairT(const PlaneRef f[4], const Geometry& g, PairRecord& out) {
  const int s = g.bits - 8;
  const int cmin = kClampMin << s, cmax = kClampMax << s;
  const int lim = kLim << s, bad = kBad << s, ymin = kMin << s, ymax = kMax << s;
  uint64_t smd = 0, sfd = 0, sld = 0;
  // v1.04 accumulates every pixel into a float; kept for compat mode.
  float wmd = 0.0f, wfd = 0.0f, wld = 0.0f;
  for (int y = 0; y < g.height - 1; y += 2) {
    const P* ra[4];
    const P* rb[4];
    for (int i = 0; i < 4; ++i) {
      ra[i] = reinterpret_cast<const P*>(f[i].ptr + f[i].stride * y);
      rb[i] = reinterpret_cast<const P*>(f[i].ptr + f[i].stride * (y + 1));
    }
    for (int x = 0; x < g.width; ++x) {
      int q[4];
      for (int i = 0; i < 4; ++i) {
        const int a = std::min(std::max(static_cast<int>(ra[i][x]), cmin), cmax);
        const int b = std::min(std::max(static_cast<int>(rb[i][x]), cmin), cmax);
        q[i] = a + b;
      }
      const int fd = std::abs(q[1] - q[2]);
      const int ld = std::abs(q[0] - q[3]);
      const int upa = q[1] * 2 - q[0];
      const int upb = q[2] * 2 - q[3];
      int md = std::abs(upa - upb) / 2;
      if (md > lim || upa < ymin || upa > ymax || upb < ymin || upb > ymax) md = bad;
      smd += static_cast<uint64_t>(md);
      sfd += static_cast<uint64_t>(fd);
      sld += static_cast<uint64_t>(ld);
      wmd += md;
      wfd += fd;
      wld += ld;
    }
  }
  out.md = smd;
  out.fd = sfd;
  out.ld = sld;
  wmd *= 0.5f;
  wfd *= 0.5f;
  wld *= 0.5f;
  const float mx = static_cast<float>(g.width * (g.height / 2)) * 235.0f * static_cast<float>(1 << s);
  out.cmd = wmd * 100.0f / mx;
  out.cfd = wfd * 100.0f / mx;
  out.cld = wld * 100.0f / mx;
}

constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr char kStatsMagic[8] = {'E', 'X', 'B', 'L', 'S', 'T', 'A', 'T'};
constexpr uint32_t kStatsVersion = 1;

bool isIX0(uint8_t f) { return (f & (INIT_MASK | IX_MASK)) == IX0; }
bool isIX1(uint8_t f) { return (f & (INIT_MASK | IX_MASK)) == IX1; }
bool isIX2(uint8_t f) { return (f & (INIT_MASK | IX_MASK)) == IX2; }
bool isIXH(uint8_t f) { return (f & (INIT_MASK | IX_MASK)) == IXH; }
bool isIgnore(uint8_t f) { return (f & (INIT_MASK | STAT_MASK)) == STAT_IGNORE; }
bool isLock(uint8_t f) { return (f & (INIT_MASK | STAT_MASK)) == STAT_LOCK; }
bool isManLong(uint8_t f) { return (f & (INIT_MASK | STAT_MASK)) == STAT_MANLONG; }
bool isMan(uint8_t f) {
  const int st = f & (INIT_MASK | STAT_MASK);
  return st == STAT_MANSHORT || st == STAT_MANLONG;
}
bool isDrop(uint8_t f) { return (f & (INIT_MASK | DROP_MASK)) == DROP_MASK; }
bool isLockIX1(uint8_t f) { return (f & (INIT_MASK | STAT_MASK | IX_MASK)) == (STAT_LOCK | IX1); }

std::string format(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

class Decider {
 public:
  Decider(const Stats& st, const Options& opt, Sequence& seq) : st_(st), opt_(opt), seq_(seq), n_(st.frames) {}

  bool run(std::string& err) {
    seq_.frames = n_;
    seq_.hints = opt_.pal == 1 ? kPalHints : kFilmHints;
    seq_.cycle = seq_.hints + 2;
    fdat().assign(static_cast<size_t>(n_), FDAT_INIT);
    fdat()[0] = fdat()[1] = 0;
    fdat()[n_ - 1] = IXH;
    if (!opt_.overrideFile.empty() && !parseOverride(err)) return false;
    if (opt_.compat)
      decide<float>();
    else
      decide<double>();
    return true;
  }

 private:
  std::vector<uint8_t>& fdat() { return seq_.fdat; }

  void setFdat(int s, uint8_t d) {
    if (s >= 2 && s < n_ && fdat()[s] == FDAT_INIT) fdat()[s] = d;
  }

  void info(const std::string& s) { seq_.log.push_back(s); }

  // Manual override command file, syntax and checks of v1.04 ('#' starts a comment line).
  bool parseOverride(std::string& err) {
    FILE* fp = std::fopen(opt_.overrideFile.c_str(), "rt");
    if (!fp) {
      err = "cannot open override file " + opt_.overrideFile;
      return false;
    }
    const int hints = seq_.hints;
    char obuf[256];
    int line = 0, cursor = 0;
    bool ok = true;
    while (ok && std::fgets(obuf, sizeof obuf - 2, fp)) {
      for (char* q = obuf; *q; ++q) {
        if (*q == '\n' || *q == '\r') {
          *q = '\0';
          break;
        }
      }
      ++line;
      const char* p = obuf;
      while (std::isspace(static_cast<unsigned char>(*p))) ++p;
      if (*p == '\0' || *p == '#') continue;
      bool h = false, star = false;
      int count = 1;
      std::string why;
      if (!std::isdigit(static_cast<unsigned char>(*p))) {
        why = "frame number expected";
      }
      const int frame = std::atoi(p);
      while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
      while (std::isspace(static_cast<unsigned char>(*p))) ++p;
      if (*p == '=') {
        ++p;
        while (std::isspace(static_cast<unsigned char>(*p))) ++p;
      }
      if (std::toupper(static_cast<unsigned char>(*p)) == 'H' || *p == '*') {
        star = *p == '*';
        h = !star;
        ++p;
        while (std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (*p == '=') {
          ++p;
          while (std::isspace(static_cast<unsigned char>(*p))) ++p;
        }
        if (std::isdigit(static_cast<unsigned char>(*p))) {
          count = std::atoi(p);
          while (std::isdigit(static_cast<unsigned char>(*p))) ++p;
        }
      }
      while (std::isspace(static_cast<unsigned char>(*p))) ++p;
      if (why.empty()) {
        if (*p != '\0') {
          why = "unexpected text";
        } else if (frame < 0 || frame >= n_) {
          why = format("start frame %d outside the clip (%d..%d)", frame, cursor, n_ - 1);
        } else if (frame < cursor) {
          why = format("overrides must be in ascending order, frame cannot be less than %d", cursor);
        } else if (count <= 0) {
          why = "invalid count";
        } else if (h) {
          int i = 0;
          for (int off = frame; i < count && off < n_; ++i, ++off) {
            if (off < 2)
              fdat()[off] = STAT_MANSHORT | IXH;
            else
              setFdat(off, STAT_MANSHORT | IXH);
          }
          count = i;
          cursor = frame + count;
          info(format("OVERRIDE: line %d [%d] HINTED (H): count=%d", line, frame, count));
        } else if (frame == 0) {
          why = "cannot pre-hint a blend pair before frame 0";
        } else if (frame >= n_ - kBlendCount) {
          why = format("cannot post-hint a blend pair after frame %d", frame);
        } else if (star) {
          if (frame <= 2) {
            fdat()[frame - 1] = IXH;
            fdat()[frame] = STAT_MANLONG | IX1;
          }
          setFdat(frame - 1, IXH);
          const int step = hints + 2;
          int i = 0;
          for (int off = frame; i < count && off < n_ - 2; ++i, off += step) {
            setFdat(off, STAT_MANLONG | IX1);
            setFdat(off + 1, IX2);
            setFdat(off + 2, IXH);
            setFdat(off + 3, IXH);
            setFdat(off + 4, IXH);
            if (hints == 4) setFdat(off + 5, IXH);
          }
          count = i;
          cursor = frame + count * step;
          info(format("OVERRIDE: line %d [%d] LONG BLEND pair (H12%s): count=%d", line, frame,
                      hints == 3 ? "HHH" : "HHHH", count));
        } else {
          if (frame <= 2) {
            fdat()[frame - 1] = IXH;
            fdat()[frame] = STAT_MANSHORT | IX1;
          }
          setFdat(frame - 1, IXH);
          setFdat(frame, STAT_MANSHORT | IX1);
          setFdat(frame + 1, IX2);
          setFdat(frame + 2, IXH);
          cursor = frame + count * 3;
          info(format("OVERRIDE: line %d [%d] SHORT BLEND pair (H12H)", line, frame));
        }
      }
      if (!why.empty()) {
        err = format("override file line %d '%s': ", line, obuf) + why;
        ok = false;
      }
    }
    std::fclose(fp);
    return ok;
  }

  template <typename T>
  void decide() {
    const int N = n_;
    std::vector<T> md(static_cast<size_t>(N + 3)), fd(md.size()), ld(md.size());
    for (int p = -2; p <= N; ++p) {
      const PairRecord& r = st_.pair(p);
      const size_t i = static_cast<size_t>(p + 2);
      if (opt_.compat) {
        md[i] = static_cast<T>(r.cmd);
        fd[i] = static_cast<T>(r.cfd);
        ld[i] = static_cast<T>(r.cld);
      } else {
        md[i] = static_cast<T>(scaleSum(r.md, st_.geom));
        fd[i] = static_cast<T>(scaleSum(r.fd, st_.geom));
        ld[i] = static_cast<T>(scaleSum(r.ld, st_.geom));
      }
    }
    auto MD = [&](int p) { return md[static_cast<size_t>(p + 2)]; };
    auto FD = [&](int p) { return fd[static_cast<size_t>(p + 2)]; };
    auto LD = [&](int p) { return ld[static_cast<size_t>(p + 2)]; };

    const int hints = seq_.hints;
    const T ithresh = static_cast<T>(opt_.ithresh);
    const T lockthresh = static_cast<T>(opt_.lockthresh);
    const bool lock = lockthresh > T(0);
    // Without fullscan, v1.04 evaluates a frame only when it is unvisited or no earlier frame stored its decimation metric.
    std::vector<char> metricsSet(static_cast<size_t>(N) + 2, 0);

    for (int n = 0; n < N; ++n) {
      const bool run = opt_.fullscan || fdat()[n] == FDAT_INIT || !metricsSet[static_cast<size_t>(n)];
      if (run) {
        int slock = 0;
        T lockdiff = 0;
        const T md10 = MD(n - 2), md21 = MD(n - 1), md32 = MD(n), md43 = MD(n + 1);
        const T fd23 = FD(n - 1), fd34 = FD(n), ld25 = LD(n);
        int result = 0;
        if (md32 < md21 && md32 < md43) {
          if (md32 < fd34) {
            result = 1;
          } else if (lock && md32 <= ithresh) {
            slock = 1;
            lockdiff = static_cast<T>(std::fabs(fd34 - (ld25 / T(2))));
            if (lockdiff <= lockthresh) slock = 2;
          }
        } else if (md21 < md10 && md21 < md32) {
          if (md21 < fd23) result = 2;
        } else if (lock && md32 <= ithresh) {
          if (md32 < fd34) {
            slock = 1;
            lockdiff = static_cast<T>(std::fabs(fd34 - (ld25 / T(2))));
            if (lockdiff <= lockthresh) slock = 2;
          }
        }
        for (int k = n; k <= n + 2 && k < N; ++k) metricsSet[static_cast<size_t>(k)] = 1;

        const uint8_t prev = n > 0 ? fdat()[n - 1] : static_cast<uint8_t>(IX0);
        const uint8_t curr = fdat()[n];
        const uint8_t next = n < N - 1 ? fdat()[n + 1] : static_cast<uint8_t>(IX0);
        switch (result) {
          case 0:
            if (isLockIX1(curr)) {
            } else if (slock == 2 && isIXH(prev) && curr == FDAT_INIT && next == FDAT_INIT) {
              setFdat(n, STAT_LOCK | IX1);
              setFdat(n + 1, IX2);
              for (int i = 0; i < hints; ++i) {
                setFdat(n + kBlendCount + i, IXH);
                setFdat(n - hints + i, IXH);
              }
            } else {
              setFdat(n, IX0);
            }
            break;
          case 1:
            if (n < 2) {
              setFdat(n, IX0);
              setFdat(n + 1, IX0);
            } else if (!isIX1(curr)) {
              if (md32 <= ithresh && curr == FDAT_INIT && next == FDAT_INIT) {
                setFdat(n, IX1);
                setFdat(n + 1, IX2);
                for (int i = 0; i < hints; ++i) {
                  setFdat(n + kBlendCount + i, IXH);
                  setFdat(n - hints + i, IXH);
                }
              } else if (md32 > ithresh && !isIXH(curr) && !isIXH(next)) {
                setFdat(n, STAT_IGNORE | IX0);
                setFdat(n + 1, IX0);
              } else {
                setFdat(n, IX0);
                setFdat(n + 1, IX0);
              }
            }
            break;
          case 2:
            if (n < 3) {
              setFdat(n - 1, IX0);
              setFdat(n, IX0);
            } else if (!isIX2(curr)) {
              if (md21 <= ithresh && curr == FDAT_INIT && prev == FDAT_INIT) {
                setFdat(n - 1, IX1);
                setFdat(n, IX2);
                for (int i = 0; i < hints; ++i) {
                  setFdat(n - 1 + kBlendCount + i, IXH);
                  setFdat(n - 1 - hints + i, IXH);
                }
              } else if (md21 > ithresh && !isIXH(curr) && !isIXH(prev)) {
                setFdat(n - 1, STAT_IGNORE | IX0);
                setFdat(n, IX0);
              } else {
                setFdat(n - 1, IX0);
                setFdat(n, IX0);
              }
            }
            break;
        }
      }

      const uint8_t flags = fdat()[n];
      int actblend = flags & IX_MASK;
      bool hinted = false;
      if (actblend == IXH) {
        hinted = true;
        actblend = IX0;
      }
      const uint8_t prev = n > 0 ? fdat()[n - 1] : static_cast<uint8_t>(IX0);
      if (actblend == IX0) {
        ++seq_.straight;
        if (isIgnore(flags)) info(format("[%d] IGNORED: MD=%6.4f", n, static_cast<double>(MD(n))));
        if (!hinted && isIXH(prev)) info(format("[%d] Lost Sequence: MD=%6.4f", n, static_cast<double>(MD(n))));
        if (isMan(flags)) info(format("[%d] MANUAL override: HINTED", n));
      } else if (actblend == IX1) {
        ++seq_.blendPairs;
        if (isIX0(prev)) info(format("[%d] New  Sequence: MD=%6.4f", n, static_cast<double>(MD(n))));
        if (isLock(flags)) info(format("[%d] LOCKED: MD=%6.4f", n, static_cast<double>(MD(n))));
        if (isMan(flags))
          info(format("[%d] MANUAL override: %s", n,
                      isManLong(flags) ? (hints == 3 ? "LONG BLEND (H12HHH)" : "LONG BLEND (H12HHHH)")
                                       : "SHORT BLEND (H12H)"));
      }
    }

    // Decimation: per cycle drop the frame most similar to its predecessor; blend index 2 duplicates index 1.
    std::vector<T> w(static_cast<size_t>(N));
    for (int k = 0; k < N; ++k) w[static_cast<size_t>(k)] = FD(k - 1);
    // v1.04 compares frame 0 with itself (weight 0) and so always drops it from the first cycle.
    if (!opt_.compat) w[0] = std::numeric_limits<T>::infinity();
    for (int i = 0; i < N; ++i) {
      if (isIX2(fdat()[i]) && i >= 1 && i + 1 < N) {
        w[static_cast<size_t>(i - 1)] *= T(2);
        w[static_cast<size_t>(i)] = T(0);
        w[static_cast<size_t>(i + 1)] *= T(2);
        i += 2;
      }
    }
    const int cycle = seq_.cycle;
    for (int i = 0; i < N; i += cycle) {
      const int lim = std::min(i + cycle, N);
      int dix = i;
      for (int j = i + 1; j < lim; ++j) {
        if (w[static_cast<size_t>(j)] < w[static_cast<size_t>(dix)]) {
          dix = j;
        } else if (w[static_cast<size_t>(j)] == w[static_cast<size_t>(dix)]) {
          if (isIX2(fdat()[j]) && !isIX2(fdat()[dix])) dix = j;
        }
      }
      fdat()[dix] |= DROP_MASK;
    }
    seq_.decimatedToSource.clear();
    for (int k = 0; k < N; ++k)
      if (!isDrop(fdat()[k])) seq_.decimatedToSource.push_back(k);
  }

  const Stats& st_;
  const Options& opt_;
  Sequence& seq_;
  const int n_;
};

template <typename P>
void renderT(const FramePlan& plan, const PlaneRef src[4], uint8_t* dst, ptrdiff_t dstStride, int width, int height,
             int bits) {
  const int maxv = (1 << bits) - 1;
  for (int y = 0; y < height; ++y) {
    P* d = reinterpret_cast<P*>(dst + dstStride * y);
    const P* s[4];
    for (int i = 0; i < plan.count; ++i) s[i] = reinterpret_cast<const P*>(src[i].ptr + src[i].stride * y);
    switch (plan.kind) {
      case FramePlan::COPY:
        std::memcpy(d, s[0], sizeof(P) * static_cast<size_t>(width));
        break;
      case FramePlan::COMPONENT:
        for (int x = 0; x < width; ++x) {
          const int u = std::min(std::max(s[1][x] * 2 - s[0][x], 0), maxv);
          d[x] = static_cast<P>(u);
        }
        break;
      case FramePlan::UNBLEND:
        for (int x = 0; x < width; ++x) {
          const int ab = std::min(std::max(s[1][x] * 2 - s[0][x], 0), maxv);
          const int cd = std::min(std::max(s[2][x] * 2 - s[3][x], 0), maxv);
          d[x] = static_cast<P>((ab + cd + 1) / 2);
        }
        break;
    }
  }
}

}  // namespace

void computePair(const PlaneRef frames[4], const Geometry& g, PairRecord& out) {
  if (g.bits == 8)
    computePairT<uint8_t>(frames, g, out);
  else
    computePairT<uint16_t>(frames, g, out);
}

double scaleSum(uint64_t sum, const Geometry& g) {
  const double mx = static_cast<double>(g.width) * (g.height / 2) * 235.0 * static_cast<double>(1 << (g.bits - 8));
  return static_cast<double>(sum) * 0.5 * 100.0 / mx;
}

uint64_t hashPlane(const PlaneRef& plane, const Geometry& g, uint64_t seed) {
  uint64_t h = seed ? seed : kFnvOffset;
  const size_t rowBytes = static_cast<size_t>(g.width) * (g.bits > 8 ? 2 : 1);
  for (int y = 0; y < g.height; ++y) {
    const uint8_t* r = plane.ptr + plane.stride * y;
    for (size_t x = 0; x < rowBytes; ++x) {
      h ^= r[x];
      h *= kFnvPrime;
    }
  }
  return h;
}

bool saveStats(const std::string& path, const Stats& s, std::string& err) {
  const std::string tmp = path + ".tmp";
  FILE* fp = std::fopen(tmp.c_str(), "wb");
  if (!fp) {
    err = "cannot write stats file " + tmp;
    return false;
  }
  bool ok = std::fwrite(kStatsMagic, 1, 8, fp) == 8;
  const uint32_t hdr[6] = {kStatsVersion,
                           static_cast<uint32_t>(s.frames),
                           static_cast<uint32_t>(s.geom.width),
                           static_cast<uint32_t>(s.geom.height),
                           static_cast<uint32_t>(s.geom.bits),
                           0};
  ok = ok && std::fwrite(hdr, sizeof hdr, 1, fp) == 1;
  ok = ok && std::fwrite(&s.fingerprint, sizeof s.fingerprint, 1, fp) == 1;
  for (const PairRecord& r : s.pairs) {
    if (!ok) break;
    const uint64_t u[3] = {r.md, r.fd, r.ld};
    const float f[3] = {r.cmd, r.cfd, r.cld};
    ok = std::fwrite(u, sizeof u, 1, fp) == 1 && std::fwrite(f, sizeof f, 1, fp) == 1;
  }
  ok = (std::fclose(fp) == 0) && ok;
  if (ok) {
    std::remove(path.c_str());
    ok = std::rename(tmp.c_str(), path.c_str()) == 0;
  }
  if (!ok) {
    std::remove(tmp.c_str());
    err = "cannot write stats file " + path;
  }
  return ok;
}

bool loadStats(const std::string& path, Stats& s, std::string& err) {
  err.clear();
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  char magic[8];
  uint32_t hdr[6];
  bool ok = std::fread(magic, 1, 8, fp) == 8 && std::memcmp(magic, kStatsMagic, 8) == 0 &&
            std::fread(hdr, sizeof hdr, 1, fp) == 1 && hdr[0] == kStatsVersion &&
            std::fread(&s.fingerprint, sizeof s.fingerprint, 1, fp) == 1;
  if (ok) {
    s.frames = static_cast<int>(hdr[1]);
    s.geom = {static_cast<int>(hdr[2]), static_cast<int>(hdr[3]), static_cast<int>(hdr[4])};
    ok = s.frames >= kMinFrames && s.frames < (1 << 28);
  }
  if (ok) {
    s.pairs.resize(static_cast<size_t>(s.frames) + 3);
    for (PairRecord& r : s.pairs) {
      uint64_t u[3];
      float f[3];
      if (std::fread(u, sizeof u, 1, fp) != 1 || std::fread(f, sizeof f, 1, fp) != 1) {
        ok = false;
        break;
      }
      r = {u[0], u[1], u[2], f[0], f[1], f[2]};
    }
  }
  std::fclose(fp);
  if (!ok) err = "stats file " + path + " is damaged or from another version";
  return ok;
}

bool buildSequence(const Stats& stats, const Options& opt, Sequence& seq, std::string& err) {
  seq = Sequence();
  Decider d(stats, opt, seq);
  return d.run(err);
}

bool writeDecimatedFile(const std::string& path, const Sequence& seq, int pal, int clipIndex, std::string& err) {
  FILE* fp = std::fopen(path.c_str(), "wt");
  if (!fp) {
    err = "cannot write " + path;
    return false;
  }
  const int N = seq.frames, cycle = seq.cycle;
  const int lo = static_cast<int>(seq.decimatedToSource.size());
  std::fprintf(fp, "#\n# PAL=%d, Cycle = %d.\n", pal, cycle);
  std::fprintf(fp, "# Inframes=%d, OutFrames = %d.\n#\n", N, lo);
  if (clipIndex == 1) {
    std::fprintf(fp, "# ClipClop() Command file for Exblend mode 2 Decimated output.\n");
    std::fprintf(fp, "# The frame number (2nd) is an unblended frame.\n#\n");
    std::fprintf(fp, "# You could also select only the recovered frames to check out using Prune() plugin\n");
    std::fprintf(fp, "# using eg 'Prune(Clip,Clip,Cmd=\"ExBlend_Decimated.TXT\");'\n");
    std::fprintf(fp, "# Requires Clip arg twice as Prunes clips index starts at 0 and the frames file at 1.\n\n");
  }
  // One line per blend pair whose index 2 was dropped and index 1 kept: the output position of index 1.
  int out = 0;
  bool ok = true;
  for (int j = 0; j < N && ok; ++j) {
    const uint8_t f = seq.fdat[static_cast<size_t>(j)];
    if (isDrop(f)) {
      if (isIX2(f) && j > 0 && !isDrop(seq.fdat[static_cast<size_t>(j - 1)])) {
        ok = (clipIndex < 0 ? std::fprintf(fp, "%d\n", out - 1) : std::fprintf(fp, "%d %d\n", clipIndex, out - 1)) >= 0;
      }
    } else {
      ++out;
    }
  }
  ok = (std::fclose(fp) == 0) && ok;
  if (!ok) err = "cannot write " + path;
  return ok;
}

bool writeLogFile(const std::string& path, const Sequence& seq, const Stats& stats, const Options& opt,
                  std::string& err) {
  FILE* fp = std::fopen(path.c_str(), "wt");
  if (!fp) {
    err = "cannot write " + path;
    return false;
  }
  std::fprintf(fp, "# ExBlend log: frames=%d pal=%d lockthresh=%.4f ithresh=%.4f fullscan=%d compat=%d\n", seq.frames,
               opt.pal, opt.lockthresh, opt.ithresh, opt.fullscan ? 1 : 0, opt.compat ? 1 : 0);
  for (const std::string& l : seq.log) std::fprintf(fp, "%s\n", l.c_str());
  const int maxPairs = seq.frames / (seq.hints + kBlendCount);
  if (seq.blendPairs) {
    double ratio = static_cast<double>(seq.straight) / seq.blendPairs;
    if (ratio < seq.hints - 1.0) ratio = seq.hints;
    const double percent = 100.0 * (seq.hints + kBlendCount) / (ratio + kBlendCount);
    std::fprintf(fp, "# BlendPairs=%d (max=%d) Ratio=%.4f (%.2f%%)\n", seq.blendPairs, maxPairs, ratio, percent);
  } else {
    std::fprintf(fp, "# BlendPairs=0 (max=%d)\n", maxPairs);
  }
  std::fprintf(fp, "# frame flags index MD FD\n");
  for (int n = 0; n < seq.frames; ++n) {
    const uint8_t f = seq.fdat[static_cast<size_t>(n)];
    const double md = opt.compat ? stats.pair(n).cmd : scaleSum(stats.pair(n).md, stats.geom);
    const double fd = opt.compat ? stats.pair(n).cfd : scaleSum(stats.pair(n).fd, stats.geom);
    std::fprintf(fp, "%d 0x%02X %c%s %.4f %.4f\n", n, f, "012H"[f & IX_MASK], isDrop(f) ? " drop" : "", md, fd);
  }
  const bool ok = std::fclose(fp) == 0;
  if (!ok) err = "cannot write " + path;
  return ok;
}

FramePlan planFrame(const Sequence& seq, const OutputParams& op, int n) {
  const int N = seq.frames;
  auto clampF = [N](int f) { return std::min(std::max(f, 0), N - 1); };
  const bool uD = op.hasDclip && op.compub >= 1;  // blended frames from the detection clip
  const bool aD = op.hasDclip && op.compub == 2;  // blend neighbours from the detection clip
  FramePlan p;
  auto copy = [&](int f, bool d) {
    p.kind = FramePlan::COPY;
    p.count = 1;
    p.frame[0] = clampF(f);
    p.fromDclip[0] = d;
  };
  // Component of a blend: index 1 uses (n-1, n), index 2 uses (n+1, n); value 2*src[1] - src[0].
  auto component = [&](int f, int offset) {
    p.kind = FramePlan::COMPONENT;
    p.count = 2;
    p.frame[0] = clampF(offset == 1 ? f - 1 : f + 1);
    p.fromDclip[0] = aD;
    p.frame[1] = clampF(f);
    p.fromDclip[1] = uD;
  };
  auto unblend = [&](int f, int offset) {
    p.kind = FramePlan::UNBLEND;
    p.count = 4;
    for (int i = 0; i < 4; ++i) p.frame[i] = clampF(f - offset + i);
    p.fromDclip[0] = aD;
    p.fromDclip[1] = uD;
    p.fromDclip[2] = uD;
    p.fromDclip[3] = aD;
  };

  int src = n;
  if (op.decimate) src = seq.decimatedToSource[static_cast<size_t>(n)];
  p.sourceFrame = src;
  int act = seq.fdat[static_cast<size_t>(src)] & IX_MASK;
  if (act == IXH) act = IX0;
  p.actblend = act;
  if (act == IX0) {
    copy(src, false);
    return p;
  }
  if (!op.decimate) {
    switch (op.disp) {
      case DISP_BLENDED:
        copy(src, uD);
        break;
      case DISP_COMPONENT:
        component(src, act);
        break;
      case DISP_SRESTORE:
        if (act == IX1)
          unblend(src, act);
        else
          copy(src + 1, false);
        break;
      default:
        unblend(src, act);
        break;
    }
    return p;
  }
  switch (op.disp) {
    case DISP_BLENDED:
      if (op.decompix == 1)
        copy(act == IX1 ? src : src - 1, uD);
      else
        copy(act == IX2 ? src : src + 1, uD);
      break;
    case DISP_ADJACENT:
      if (op.decompix == 1)
        copy(act == IX1 ? src - 1 : src - 2, aD);
      else
        copy(act == IX2 ? src + 1 : src + 2, aD);
      break;
    case DISP_COMPONENT:
      if (op.decompix == 1)
        component(act == IX1 ? src : src - 1, 1);
      else
        component(act == IX2 ? src : src + 1, 2);
      break;
    default:
      unblend(src, act);
      break;
  }
  return p;
}

void renderPlane(const FramePlan& plan, const PlaneRef src[4], uint8_t* dst, ptrdiff_t dstStride, int width, int height,
                 int bits) {
  if (bits == 8)
    renderT<uint8_t>(plan, src, dst, dstStride, width, height, bits);
  else
    renderT<uint16_t>(plan, src, dst, dstStride, width, height, bits);
}

}  // namespace exblend
