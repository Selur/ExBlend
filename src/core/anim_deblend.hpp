// SPDX-License-Identifier: GPL-2.0-or-later
// AnimDeblend: blend detection for animation without a fixed frame rate (port of prototype/anim_deblend.py).
// Host-independent; works on single-precision luma frames scaled to 0..255 at an analysis size.
#pragma once

#include <cstddef>
#include <map>
#include <vector>

namespace animdeblend {

struct Frame {
  std::vector<float> px;  // row-major luma, 0..255
  int width = 0;
  int height = 0;
};

// A blend frame: mix of the anchors a < k < b with weight f of b.
struct Blend {
  int a = 0;
  int b = 0;
  double f = 0.0;
};

// A frame with a faint copy of neighbour nb at weight f.
struct Ghost {
  int nb = 0;
  double f = 0.0;
};

using Blends = std::map<int, Blend>;
using Ghosts = std::map<int, Ghost>;

enum class Mode { Anchor, Cadence, Both };

struct Params {
  Mode mode = Mode::Anchor;
  int maxgap = 4;
  double rho = 0.1;
  double kappa = 3.0;
  double minweight = 0.1;
  double switchCost = 1.5;
  bool fill = true;
};

// Weight f of b in o = (1-f) a + f b (clamped to 0..1) and the mean squared residual r.
double blendFit(const Frame& o, const Frame& a, const Frame& b, double& r);
double meanSquaredDiff(const Frame& x, const Frame& y);

// Half the 10 % quantile of the given frame-to-frame differences (holds and calm stretches).
double noiseFromSteps(std::vector<double> steps);
// Frame numbers of the sampled pairs (k-1, k) used for the clip-wide noise estimate.
std::vector<int> noiseSamplePositions(int frames, int count = 1000);

Blends analyseAnchor(const std::vector<Frame>& frames, const Params& p, double var);
Blends analyseCadence(const std::vector<Frame>& frames, const Params& p, double var);
Blends fillGaps(const std::vector<Frame>& frames, const Blends& blends, double var);

// Blends of frames keep..keepEnd-1 from the frames start..start+frames.size()-1 (the part plus its context).
Blends analysePart(const std::vector<Frame>& frames, int start, int keep, int keepEnd, const Params& p, double var);

// 3x3 box blur, two horizontal then two vertical passes, edges repeated (std.BoxBlur with radius 1 and 2 passes).
Frame boxBlur(const Frame& in);

// Ghost analysis: raw frame and its 3x3 box blur (two passes) at the ghost analysis size.
struct GhostFrame {
  GhostFrame(Frame raw, const Frame& smooth);
  Frame raw;
  double mean = 0.0;
  double var = 0.0;
  int width = 0;
  int height = 0;
  std::vector<float> grad[2];      // smoothed frame: rows (y+1 - y), columns (x+1 - x)
  std::vector<unsigned char> mask[2];  // the strongest 5 % of each gradient
};

// (f, sharpness) of n's ghost in o; returns false when there is none.
bool ghostOf(const GhostFrame& o, const GhostFrame& n, double& f, double& sharpness);
// Ghosts of the frames 1..frames.size()-2 of the given run (frame numbers offset by start).
Ghosts analyseGhosts(const std::vector<GhostFrame>& frames, int start);

}  // namespace animdeblend
