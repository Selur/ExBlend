// SPDX-License-Identifier: GPL-2.0-or-later
// VapourSynth (API 4) interface: exblend.AnimDeblend for 8-16 bit integer and 32 bit float YUV and Gray clips.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "VSHelper4.h"
#include "VapourSynth4.h"
#include "core/anim_deblend.hpp"

using namespace animdeblend;

namespace {

constexpr int kGhostWidth = 720;  // ghost analysis size: thin edges of small drawings survive
constexpr int kAhead = 32;        // frames requested ahead of the one read (keeps the decoder threads busy)

struct Part {
  std::mutex m;
  bool done = false;
  Blends blends;
  Ghosts ghosts;
};

struct AnimData {
  const VSAPI* vsapi = nullptr;
  VSNode* node = nullptr;
  VSNode* small = nullptr;    // luma at the blend analysis size, GRAYS
  VSNode* graw = nullptr;     // luma at the ghost analysis size, GRAYS
  VSVideoInfo vi{};
  Params p;
  double strength = 1.0;
  bool unghost = false;
  int block = 512;
  int overlap = 128;
  std::mutex guard;
  bool varReady = false;
  double var = 0.0;
  std::vector<std::unique_ptr<Part>> parts;
};

int scaledHeight(const VSVideoInfo* vi, int width) {
  return std::max(16, static_cast<int>(std::nearbyint(static_cast<double>(vi->height) * width / vi->width / 2.0)) * 2);
}

// Reads frames of a GRAYS node as luma scaled by `scale`; with read-ahead, the next frames are requested
// asynchronously (the synchronous read of the current one reserves its own thread, so this cannot deadlock).
class Reader {
 public:
  Reader(VSNode* node, const VSAPI* vsapi, int last, int ahead) : node_(node), vsapi_(vsapi), last_(last), ahead_(ahead) {}

  bool read(int k, Frame& out, std::string& err, float scale = 255.0f) {
    const int until = std::min(last_, k + ahead_);
    for (int a = std::max(next_, k + 1); a <= until; ++a)
      vsapi_->getFrameAsync(a, node_, [](void* user, const VSFrame* f, int, VSNode*, const char*) {
        static_cast<const VSAPI*>(user)->freeFrame(f);
      }, const_cast<VSAPI*>(vsapi_));
    next_ = std::max(next_, until + 1);
    char buf[1024];
    const VSFrame* f = vsapi_->getFrame(k, node_, buf, sizeof buf);
    if (!f) {
      err = buf;
      return false;
    }
    const int w = vsapi_->getFrameWidth(f, 0), h = vsapi_->getFrameHeight(f, 0);
    const ptrdiff_t stride = vsapi_->getStride(f, 0);
    const uint8_t* base = vsapi_->getReadPtr(f, 0);
    out.width = w;
    out.height = h;
    out.px.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
      const float* row = reinterpret_cast<const float*>(base + stride * y);
      for (int x = 0; x < w; ++x) out.px[static_cast<size_t>(y) * w + x] = row[x] * scale;
    }
    vsapi_->freeFrame(f);
    return true;
  }

 private:
  VSNode* node_;
  const VSAPI* vsapi_;
  int last_;
  int ahead_;
  int next_ = 0;
};

// Runs fn(0..count-1) on up to hardware_concurrency threads.
template <typename Fn>
void parallelFor(size_t count, Fn fn) {
  const size_t workers = std::min<size_t>(count, std::max(1u, std::thread::hardware_concurrency()));
  std::atomic<size_t> next{0};
  auto run = [&] {
    for (size_t i = next++; i < count; i = next++) fn(i);
  };
  std::vector<std::thread> threads;
  for (size_t t = 1; t < workers; ++t) threads.emplace_back(run);
  run();
  for (std::thread& t : threads) t.join();
}

// One noise estimate for the whole clip: a part without holds would measure motion instead of noise.
bool ensureNoise(AnimData* d, std::string& err) {
  std::lock_guard<std::mutex> lock(d->guard);
  if (d->varReady) return true;
  const int n = d->vi.numFrames;
  std::vector<double> steps;
  if (d->block < n) {
    // evenly spaced pairs: close to the full estimate without reading the whole clip
    Reader rd(d->small, d->vsapi, n - 1, 0);
    for (int k : noiseSamplePositions(n)) {
      Frame a, b;
      if (!rd.read(k, a, err, 1.0f) || !rd.read(k - 1, b, err, 1.0f)) return false;
      steps.push_back(meanSquaredDiff(a, b) * 255.0 * 255.0);
    }
  } else {
    Reader rd(d->small, d->vsapi, n - 1, kAhead);
    Frame prev, cur;
    for (int k = 0; k < n; ++k) {
      if (!rd.read(k, cur, err)) return false;
      if (k > 0) steps.push_back(meanSquaredDiff(cur, prev));
      std::swap(prev, cur);
    }
  }
  d->var = noiseFromSteps(std::move(steps));
  d->varReady = true;
  return true;
}

// Blends and ghosts of block i; false with err set on a read error.
bool analyseBlock(AnimData* d, int i, Blends& blends, Ghosts& ghosts, std::string& err) {
  const int n = d->vi.numFrames;
  const int s = i * d->block, e = std::min(n, s + d->block);
  const int lo = std::max(0, s - d->overlap), hi = std::min(n, e + d->overlap);
  std::vector<Frame> frames(static_cast<size_t>(hi - lo));
  Reader rd(d->small, d->vsapi, hi - 1, kAhead);
  for (int k = lo; k < hi; ++k)
    if (!rd.read(k, frames[static_cast<size_t>(k - lo)], err)) return false;
  blends = analysePart(frames, lo, s, e, d->p, d->var);
  if (d->unghost) {
    // ghosts: all frames examined, a frame the blend modes treat is replaced instead
    const int glo = std::max(0, s - 1), ghi = std::min(n, e + 1);
    const size_t count = static_cast<size_t>(ghi - glo);
    std::vector<Frame> raw(count);
    Reader rraw(d->graw, d->vsapi, ghi - 1, kAhead);
    for (int k = glo; k < ghi; ++k)
      if (!rraw.read(k, raw[static_cast<size_t>(k - glo)], err)) return false;
    // the per-frame work is independent: spread it over the cores
    std::vector<std::unique_ptr<GhostFrame>> gf(count);
    parallelFor(count, [&](size_t i) {
      const Frame smooth = boxBlur(raw[i]);
      gf[i] = std::make_unique<GhostFrame>(std::move(raw[i]), smooth);
    });
    std::vector<Ghosts> found(count);
    parallelFor(count, [&](size_t i) {
      if (i == 0 || i + 1 == count) return;
      double bestSharp = 0.0;
      for (size_t nb : {i - 1, i + 1}) {
        double f, sharp;
        if (ghostOf(*gf[i], *gf[nb], f, sharp) && (found[i].empty() || sharp > bestSharp)) {
          found[i][static_cast<int>(i) + glo] = {static_cast<int>(nb) + glo, f};
          bestSharp = sharp;
        }
      }
    });
    for (const Ghosts& g : found)
      for (const auto& kv : g)
        if (kv.first >= s && kv.first < e && !blends.count(kv.first)) ghosts[kv.first] = kv.second;
  }
  return true;
}

// Block i, analysed on first use. Requests wait for a running analysis of their block; the analysing thread is one of
// VapourSynth's, whose synchronous frame requests get a reserved thread, so waiting threads cannot starve it (a thread
// of its own, analysing ahead, would get none and could deadlock).
Part* ensurePart(AnimData* d, int i, std::string& err) {
  Part* part = d->parts[static_cast<size_t>(i)].get();
  std::lock_guard<std::mutex> lock(part->m);
  if (part->done) return part;
  if (!ensureNoise(d, err)) return nullptr;
  if (!analyseBlock(d, i, part->blends, part->ghosts, err)) return nullptr;
  part->done = true;
  return part;
}

struct Decision {
  enum Kind { Plain, Copy, Unghost } kind = Plain;
  int src = 0;  // frame copied or unblended against
  Blend blend;
  Ghost ghost;
};

bool decide(AnimData* d, int n, Decision& dec, std::string& err) {
  Part* part = ensurePart(d, n / d->block, err);
  if (!part) return false;
  const int last = d->vi.numFrames - 1;
  auto g = part->ghosts.find(n);
  if (g != part->ghosts.end()) {
    dec.kind = Decision::Unghost;
    dec.ghost = g->second;
    dec.src = std::min(std::max(g->second.nb, 0), last);
    return true;
  }
  auto b = part->blends.find(n);
  if (b != part->blends.end()) {
    dec.kind = Decision::Copy;
    dec.blend = b->second;
    dec.src = std::min(std::max(b->second.f < 0.5 ? b->second.a : b->second.b, 0), last);
  }
  return true;
}

template <typename T>
void unghostPlane(const uint8_t* xp, ptrdiff_t xs, const uint8_t* yp, ptrdiff_t ys, uint8_t* dp, ptrdiff_t ds, int w,
                  int h, float g, int bits) {
  const float den = 1.0f - g, maxv = static_cast<float>((1 << bits) - 1);
  for (int y = 0; y < h; ++y) {
    const T* x = reinterpret_cast<const T*>(xp + xs * y);
    const T* n = reinterpret_cast<const T*>(yp + ys * y);
    T* o = reinterpret_cast<T*>(dp + ds * y);
    for (int i = 0; i < w; ++i) {
      const float v = (static_cast<float>(x[i]) - g * static_cast<float>(n[i])) / den;
      if constexpr (std::is_same<T, float>::value)
        o[i] = v;
      else
        o[i] = static_cast<T>(std::lrint(std::min(std::max(v, 0.0f), maxv)));
    }
  }
}

template <typename T>
void mergePlane(const uint8_t* ap, ptrdiff_t as, const uint8_t* bp, ptrdiff_t bs, uint8_t* dp, ptrdiff_t ds, int w,
                int h, double weight) {
  const unsigned wi = static_cast<unsigned>(weight * 65536.0 + 0.5);
  for (int y = 0; y < h; ++y) {
    const T* a = reinterpret_cast<const T*>(ap + as * y);
    const T* b = reinterpret_cast<const T*>(bp + bs * y);
    T* o = reinterpret_cast<T*>(dp + ds * y);
    for (int i = 0; i < w; ++i) {
      if constexpr (std::is_same<T, float>::value)
        o[i] = static_cast<float>(a[i] + (b[i] - a[i]) * weight);
      else
        o[i] = static_cast<T>((static_cast<unsigned>(a[i]) * (65536u - wi) + static_cast<unsigned>(b[i]) * wi + 32768u) >> 16);
    }
  }
}

const VSFrame* VS_CC animGetFrame(int n, int reason, void* instance, void**, VSFrameContext* ctx, VSCore* core,
                                  const VSAPI* vsapi) {
  auto* d = static_cast<AnimData*>(instance);
  Decision dec;
  std::string err;
  if (!decide(d, n, dec, err)) {
    vsapi->setFilterError(("AnimDeblend: " + err).c_str(), ctx);
    return nullptr;
  }
  const bool merge = dec.kind == Decision::Copy && d->strength < 1.0;
  if (reason == arInitial) {
    if (dec.kind != Decision::Copy || merge) vsapi->requestFrameFilter(n, d->node, ctx);
    if (dec.kind != Decision::Plain) vsapi->requestFrameFilter(dec.src, d->node, ctx);
    return nullptr;
  }
  if (reason != arAllFramesReady) return nullptr;

  VSFrame* dst = nullptr;
  const VSVideoFormat& fmt = d->vi.format;
  if (dec.kind == Decision::Plain) {
    const VSFrame* src = vsapi->getFrameFilter(n, d->node, ctx);
    dst = vsapi->copyFrame(src, core);
    vsapi->freeFrame(src);
  } else if (dec.kind == Decision::Copy && !merge) {
    const VSFrame* src = vsapi->getFrameFilter(dec.src, d->node, ctx);
    dst = vsapi->copyFrame(src, core);
    vsapi->freeFrame(src);
  } else {
    const VSFrame* cur = vsapi->getFrameFilter(n, d->node, ctx);
    const VSFrame* other = vsapi->getFrameFilter(dec.src, d->node, ctx);
    dst = vsapi->newVideoFrame(&fmt, d->vi.width, d->vi.height, cur, core);
    // strength scales how much of the ghost is taken out
    const float g = static_cast<float>(std::round(dec.ghost.f * 100.0) / 100.0 * std::min(d->strength, 1.0));
    for (int p = 0; p < fmt.numPlanes; ++p) {
      const uint8_t* cp = vsapi->getReadPtr(cur, p);
      const uint8_t* op = vsapi->getReadPtr(other, p);
      uint8_t* dp = vsapi->getWritePtr(dst, p);
      const ptrdiff_t cs = vsapi->getStride(cur, p), os = vsapi->getStride(other, p), ds = vsapi->getStride(dst, p);
      const int w = vsapi->getFrameWidth(dst, p), h = vsapi->getFrameHeight(dst, p);
      if (dec.kind == Decision::Unghost) {
        if (fmt.sampleType == stFloat)
          unghostPlane<float>(cp, cs, op, os, dp, ds, w, h, g, 32);
        else if (fmt.bytesPerSample == 1)
          unghostPlane<uint8_t>(cp, cs, op, os, dp, ds, w, h, g, fmt.bitsPerSample);
        else
          unghostPlane<uint16_t>(cp, cs, op, os, dp, ds, w, h, g, fmt.bitsPerSample);
      } else {
        if (fmt.sampleType == stFloat)
          mergePlane<float>(cp, cs, op, os, dp, ds, w, h, d->strength);
        else if (fmt.bytesPerSample == 1)
          mergePlane<uint8_t>(cp, cs, op, os, dp, ds, w, h, d->strength);
        else
          mergePlane<uint16_t>(cp, cs, op, os, dp, ds, w, h, d->strength);
      }
    }
    vsapi->freeFrame(cur);
    vsapi->freeFrame(other);
  }

  VSMap* props = vsapi->getFramePropertiesRW(dst);
  vsapi->mapSetInt(props, "AnimBlend", dec.kind == Decision::Copy, maReplace);
  vsapi->mapSetInt(props, "AnimGhost", dec.kind == Decision::Unghost, maReplace);
  if (dec.kind == Decision::Copy) {
    vsapi->mapSetFloat(props, "AnimBlendWeight", dec.blend.f, maReplace);
    vsapi->mapSetInt(props, "AnimBlendFrom", dec.blend.a, maReplace);
    vsapi->mapSetInt(props, "AnimBlendTo", dec.blend.b, maReplace);
  } else if (dec.kind == Decision::Unghost) {
    vsapi->mapSetInt(props, "AnimGhostFrom", dec.ghost.nb, maReplace);
    vsapi->mapSetFloat(props, "AnimGhostWeight", dec.ghost.f, maReplace);
  }
  return dst;
}

void VS_CC animFree(void* instance, VSCore*, const VSAPI* vsapi) {
  auto* d = static_cast<AnimData*>(instance);
  vsapi->freeNode(d->graw);
  vsapi->freeNode(d->small);
  vsapi->freeNode(d->node);
  delete d;
}

// Luma of the clip at the given width as GRAYS (resize.Bilinear); nullptr with err set.
VSNode* analysisNode(VSNode* node, int width, VSCore* core, const VSAPI* vsapi, std::string& err) {
  const VSVideoInfo* vi = vsapi->getVideoInfo(node);
  VSPlugin* resize = vsapi->getPluginByID("com.vapoursynth.resize", core);
  VSMap* args = vsapi->createMap();
  vsapi->mapSetNode(args, "clip", node, maReplace);
  vsapi->mapSetInt(args, "width", width, maReplace);
  vsapi->mapSetInt(args, "height", scaledHeight(vi, width), maReplace);
  vsapi->mapSetInt(args, "format", pfGrayS, maReplace);
  VSMap* ret = vsapi->invoke(resize, "Bilinear", args);
  vsapi->freeMap(args);
  if (vsapi->mapGetError(ret)) {
    err = vsapi->mapGetError(ret);
    vsapi->freeMap(ret);
    return nullptr;
  }
  VSNode* out = vsapi->mapGetNode(ret, "clip", 0, nullptr);
  vsapi->freeMap(ret);
  return out;
}

double floatOr(const VSMap* in, const char* key, double def, const VSAPI* vsapi) {
  int err = 0;
  const double v = vsapi->mapGetFloat(in, key, 0, &err);
  return err ? def : v;
}

int64_t intOr(const VSMap* in, const char* key, int64_t def, const VSAPI* vsapi) {
  int err = 0;
  const int64_t v = vsapi->mapGetInt(in, key, 0, &err);
  return err ? def : v;
}

void VS_CC animCreate(const VSMap* in, VSMap* out, void*, VSCore* core, const VSAPI* vsapi) {
  auto d = std::make_unique<AnimData>();
  d->vsapi = vsapi;
  d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
  auto fail = [&](const std::string& msg) {
    vsapi->mapSetError(out, ("AnimDeblend: " + msg).c_str());
    vsapi->freeNode(d->graw);
    vsapi->freeNode(d->small);
    vsapi->freeNode(d->node);
  };
  const VSVideoInfo* vi = vsapi->getVideoInfo(d->node);
  d->vi = *vi;
  const VSVideoFormat& f = vi->format;
  if (!vsh::isConstantVideoFormat(vi)) return fail("clip must have a constant format and size");
  if ((f.colorFamily != cfYUV && f.colorFamily != cfGray) ||
      !((f.sampleType == stInteger && f.bitsPerSample >= 8 && f.bitsPerSample <= 16) ||
        (f.sampleType == stFloat && f.bitsPerSample == 32)))
    return fail("clip must be 8-16 bit integer or 32 bit float YUV or Gray");
  if (vi->numFrames < 3) return fail("clip needs at least 3 frames");

  int err = 0;
  const char* mode = vsapi->mapGetData(in, "mode", 0, &err);
  const std::string m = err || !mode ? "anchor" : mode;
  if (m == "anchor")
    d->p.mode = Mode::Anchor;
  else if (m == "cadence")
    d->p.mode = Mode::Cadence;
  else if (m == "both")
    d->p.mode = Mode::Both;
  else
    return fail("mode must be \"anchor\", \"cadence\" or \"both\"");
  d->strength = floatOr(in, "strength", 1.0, vsapi);
  d->p.maxgap = static_cast<int>(intOr(in, "maxgap", 4, vsapi));
  d->p.rho = floatOr(in, "rho", 0.1, vsapi);
  d->p.kappa = floatOr(in, "kappa", 3.0, vsapi);
  d->p.minweight = floatOr(in, "minweight", 0.1, vsapi);
  d->p.switchCost = floatOr(in, "switch", 1.5, vsapi);
  d->p.fill = intOr(in, "fill", 1, vsapi) != 0;
  d->unghost = intOr(in, "unghost", 0, vsapi) != 0;
  const int width = static_cast<int>(intOr(in, "width", 160, vsapi));
  d->block = static_cast<int>(intOr(in, "block", 512, vsapi));
  d->overlap = static_cast<int>(intOr(in, "overlap", 128, vsapi));
  if (d->strength < 0.0 || d->strength > 1.0) return fail("strength must be 0.0 .. 1.0");
  if (d->p.minweight < 0.01 || d->p.minweight > 0.49) return fail("minweight must be between 0.01 and 0.49");
  if (d->p.maxgap < 2) return fail("maxgap must be 2 or more");
  if (d->p.rho <= 0.0) return fail("rho must be greater than 0");
  if (d->p.kappa <= 0.0) return fail("kappa must be greater than 0");
  if (d->p.switchCost < 0.0) return fail("switch must be 0 or more");
  if (width < 16) return fail("width must be 16 or more");
  if (d->block < 0 || d->overlap < 0) return fail("block and overlap must be 0 or more");
  if (d->block == 0 || d->block > vi->numFrames) d->block = vi->numFrames;

  std::string msg;
  d->small = analysisNode(d->node, width, core, vsapi, msg);
  if (d->small && d->unghost) d->graw = analysisNode(d->node, kGhostWidth, core, vsapi, msg);
  if (!d->small || (d->unghost && !d->graw)) return fail(msg);
  const int parts = (vi->numFrames + d->block - 1) / d->block;
  for (int i = 0; i < parts; ++i) d->parts.push_back(std::make_unique<Part>());

  VSFilterDependency deps[1] = {{d->node, rpGeneral}};
  AnimData* data = d.release();
  vsapi->createVideoFilter(out, "AnimDeblend", &data->vi, animGetFrame, animFree, fmParallel, deps, 1, data, core);
}

}  // namespace

void registerAnimDeblend(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
  vspapi->registerFunction("AnimDeblend",
                           "clip:vnode;strength:float:opt;mode:data:opt;unghost:int:opt;fill:int:opt;rho:float:opt;"
                           "minweight:float:opt;switch:float:opt;maxgap:int:opt;kappa:float:opt;width:int:opt;"
                           "block:int:opt;overlap:int:opt;",
                           "clip:vnode;", animCreate, nullptr, plugin);
}
