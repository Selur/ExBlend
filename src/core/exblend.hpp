// SPDX-License-Identifier: GPL-2.0-or-later
// ExBlend core: blend-pair detection, sequence decisions, decimation and unblending.
// Port of ExBlend v1.04 by StainlessS (AviSynth), host independent.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace exblend {

constexpr int kFilmHints = 3;
constexpr int kPalHints = 4;
constexpr int kBlendCount = 2;
constexpr int kMinFrames = 7;

// Per-frame decision byte, same layout as the v1.04 XBD file.
enum : uint8_t {
  IX_MASK = 0x03,
  STAT_MASK = 0x1C,
  DROP_MASK = 0x40,
  INIT_MASK = 0x80,
};
enum : uint8_t { IX0 = 0, IX1 = 1, IX2 = 2, IXH = 3 };
enum : uint8_t {
  STAT_NONE = 0 << 2,
  STAT_IGNORE = 1 << 2,
  STAT_LOCK = 2 << 2,
  STAT_MANSHORT = 3 << 2,
  STAT_MANLONG = 4 << 2,
};
constexpr uint8_t FDAT_INIT = 0xFF;

enum Disp { DISP_BLENDED = 0, DISP_EXBLEND = 1, DISP_SRESTORE = 2, DISP_COMPONENT = 3, DISP_ADJACENT = 4 };

struct PlaneRef {
  const uint8_t* ptr;
  ptrdiff_t stride;  // bytes
};

struct Geometry {
  int width;   // luma width in samples
  int height;  // luma height
  int bits;    // 8..16
};

// Detection sums of the frame pair (p, p+1) with neighbours p-1 and p+2.
struct PairRecord {
  uint64_t md = 0, fd = 0, ld = 0;   // exact integer sums
  float cmd = 0, cfd = 0, cld = 0;   // v1.04 float accumulation, scaled to metric units
};

// Computes one pair record; frames[0..3] are the luma planes of p-1, p, p+1, p+2 (indices clamped by the caller).
void computePair(const PlaneRef frames[4], const Geometry& g, PairRecord& out);

// Metric value of an exact sum in the v1.04 units (percent of the luma range).
double scaleSum(uint64_t sum, const Geometry& g);

// FNV-1a 64 over a luma plane, used to recognise the clip a stats file belongs to.
uint64_t hashPlane(const PlaneRef& plane, const Geometry& g, uint64_t seed);

// Pair records for p = -2 .. frames, index p + 2.
struct Stats {
  Geometry geom{};
  int frames = 0;
  uint64_t fingerprint = 0;
  std::vector<PairRecord> pairs;
  const PairRecord& pair(int p) const { return pairs[static_cast<size_t>(p + 2)]; }
};

bool saveStats(const std::string& path, const Stats& s, std::string& err);
// Returns false with err empty when the file does not exist.
bool loadStats(const std::string& path, Stats& s, std::string& err);

// Automatic film/PAL choice: a variant wins with at least this share of continued cycles and this lead over the other.
constexpr double kAutoMinScore = 0.5;
constexpr double kAutoMinLead = 2.0;

// Re-phase: a hinted frame pair whose frame difference is at least kRephaseMinFD and kRephaseRatio times its merge
// delta is taken as the real blend pair and replaces the earlier pair (not with compat).
constexpr double kRephaseMinFD = 1.0;
constexpr double kRephaseRatio = 20.0;

struct Options {
  int pal = 0;              // 0 film (cycle 5), 1 PAL (cycle 6), -1 automatic
  double lockthresh = 1.0;  // 0 switches locking off
  double ithresh = 3.0;
  bool fullscan = true;     // evaluate every frame (v1.04 mode 1 with show >= 2)
  bool compat = false;      // reproduce v1.04 exactly, including its defects
  std::string overrideFile;
};

// Result of the sequential decision pass.
struct Sequence {
  int frames = 0;
  int pal = 0;                      // variant used, also when Options::pal is -1
  double score[2] = {0, 0};         // share of continued cycles for film and PAL (the variant used, both with pal=-1)
  std::string warning;              // set when the clip shows no clear blend pattern
  int hints = kFilmHints;
  int cycle = 5;
  std::vector<uint8_t> fdat;        // decision byte per input frame
  std::vector<int> decimatedToSource;  // output frame -> input frame when decimating
  std::vector<std::string> log;     // events, one line each
  int blendPairs = 0;
  int straight = 0;
};

// Runs override parsing, the decision pass and the decimation choice. Returns false with err set on failure.
bool buildSequence(const Stats& stats, const Options& opt, Sequence& seq, std::string& err);

// ClipClop command file of the recovered frames in the decimated clip (v1.04 format).
bool writeDecimatedFile(const std::string& path, const Sequence& seq, int clipIndex, std::string& err);

bool writeLogFile(const std::string& path, const Sequence& seq, const Stats& stats, const Options& opt, std::string& err);

// Which input frames a host has to fetch for one output frame, and how to combine them.
struct FramePlan {
  // COPY: src[0]; COMPONENT: clamp(2*src[1] - src[0]);
  // UNBLEND: (clamp(2*src[1] - src[0]) + clamp(2*src[2] - src[3]) + 1) / 2.
  enum Kind { COPY, UNBLEND, COMPONENT } kind = COPY;
  int frame[4] = {0, 0, 0, 0};
  bool fromDclip[4] = {false, false, false, false};
  int count = 1;
  int sourceFrame = 0;  // input frame whose properties are passed on
  int actblend = 0;
};

struct OutputParams {
  int disp = DISP_EXBLEND;
  int compub = 2;
  bool decimate = true;
  int decompix = 1;
  bool hasDclip = false;
};

// Plans output frame n.
FramePlan planFrame(const Sequence& seq, const OutputParams& op, int n);

// Pixel operations of a plan for one plane; dst and the sources have the same sample type and size.
void renderPlane(const FramePlan& plan, const PlaneRef src[4], uint8_t* dst, ptrdiff_t dstStride, int width, int height,
                 int bits);

}  // namespace exblend
