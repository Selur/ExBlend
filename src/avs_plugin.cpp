// SPDX-License-Identifier: GPL-2.0-or-later
// AviSynth+ (3.7.5 or newer) interface: ExBlendScan and ExBlend for 8-16 bit planar YUV and Y clips.
#include <algorithm>
#include <memory>
#include <string>

#include "avisynth.h"
#include "core/exblend.hpp"
#include "core/scan.hpp"
#include "version.hpp"

using namespace exblend;

namespace {

const int kPlanes[3] = {PLANAR_Y, PLANAR_U, PLANAR_V};

Geometry geometryOf(const VideoInfo& vi) { return {vi.width, vi.height, vi.BitsPerComponent()}; }

PlaneRef planeOf(const PVideoFrame& f, int plane) { return {f->GetReadPtr(plane), f->GetPitch(plane)}; }

std::string checkFormat(const VideoInfo& vi, const char* what) {
  const int bits = vi.BitsPerComponent();
  if (!vi.IsPlanar() || !(vi.IsYUV() || vi.IsY()) || vi.IsYUVA() || bits < 8 || bits > 16)
    return std::string(what) + " must be 8-16 bit planar YUV or Y";
  return {};
}

std::string checkDclip(const VideoInfo& vi, const VideoInfo& dvi) {
  if (!vi.IsSameColorspace(dvi) || vi.width != dvi.width || vi.height != dvi.height)
    return "dclip must have the same format and size as clip";
  if (vi.num_frames != dvi.num_frames) return "dclip must have the same number of frames as clip";
  if (static_cast<int64_t>(vi.fps_numerator) * dvi.fps_denominator !=
      static_cast<int64_t>(dvi.fps_numerator) * vi.fps_denominator)
    return "dclip must have the same frame rate as clip";
  return {};
}

void reduce(int64_t& a, int64_t& b) {
  int64_t x = a, y = b;
  while (y) {
    const int64_t t = x % y;
    x = y;
    y = t;
  }
  if (x > 1) {
    a /= x;
    b /= x;
  }
}

// Passes frames through, computes every pair record once and writes the stats file when the clip is done.
class ScanFilter : public GenericVideoFilter {
 public:
  ScanFilter(PClip child, PClip det, std::shared_ptr<ScanState> state)
      : GenericVideoFilter(child), det_(det), state_(std::move(state)) {}

  PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override {
    ScanState& st = *state_;
    const int N = st.frames();
    n = std::min(std::max(n, 0), N - 1);
    const int lo = std::max(0, n - 1), hi = std::min(N - 1, n + 2);
    PVideoFrame det[4];
    for (int k = lo; k <= hi; ++k) det[k - lo] = det_->GetFrame(k, env);
    auto detFrame = [&](int k) -> const PVideoFrame& { return det[std::min(std::max(k, 0), N - 1) - lo]; };
    const Geometry& g = st.geom();
    for (int i = 0; i < 3; ++i)
      if (st.fingerprintFrame(i) == n) st.setFingerprint(n, hashPlane(planeOf(detFrame(n), PLANAR_Y), g, 0));

    int first, last;
    st.pairsOf(n, first, last);
    bool completed = false;
    PairRecord own;
    for (int p = first; p <= last; ++p) {
      PlaneRef planes[4];
      for (int i = 0; i < 4; ++i) planes[i] = planeOf(detFrame(p - 1 + i), PLANAR_Y);
      PairRecord r;
      computePair(planes, g, r);
      if (p == n) own = r;
      if (st.claim(p)) completed = st.store(p, r) || completed;
    }
    if (completed) {
      std::string err;
      if (!st.writeOnce(err)) env->ThrowError("ExBlendScan: %s", err.c_str());
    }

    PVideoFrame dst = child->GetFrame(n, env);
    env->MakePropertyWritable(&dst);
    AVSMap* props = env->getFramePropsRW(dst);
    env->propSetFloat(props, "ExBlendMD", scaleSum(own.md, g), PROPAPPENDMODE_REPLACE);
    env->propSetFloat(props, "ExBlendFD", scaleSum(own.fd, g), PROPAPPENDMODE_REPLACE);
    env->propSetFloat(props, "ExBlendLD", scaleSum(own.ld, g), PROPAPPENDMODE_REPLACE);
    return dst;
  }

  int __stdcall SetCacheHints(int cachehints, int) override { return cachehints == CACHE_GET_MTMODE ? MT_NICE_FILTER : 0; }

 private:
  PClip det_;
  std::shared_ptr<ScanState> state_;
};

class ExBlendFilter : public GenericVideoFilter {
 public:
  ExBlendFilter(PClip child, PClip dclip, Sequence seq, Stats stats, OutputParams op, bool compat, bool show)
      : GenericVideoFilter(child), dclip_(dclip), seq_(std::move(seq)), stats_(std::move(stats)), op_(op),
        compat_(compat), show_(show) {
    if (op_.decimate) {
      vi.num_frames = static_cast<int>(seq_.decimatedToSource.size());
      vi.MulDivFPS(static_cast<unsigned>(seq_.cycle - 1), static_cast<unsigned>(seq_.cycle));
    }
  }

  PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override {
    n = std::min(std::max(n, 0), vi.num_frames - 1);
    const FramePlan plan = planFrame(seq_, op_, n);
    PVideoFrame propSrc = child->GetFrame(plan.sourceFrame, env);
    PVideoFrame dst;
    if (plan.kind == FramePlan::COPY && !plan.fromDclip[0] && plan.frame[0] == plan.sourceFrame) {
      dst = propSrc;
      env->MakeWritable(&dst);
    } else {
      PVideoFrame src[4];
      for (int i = 0; i < plan.count; ++i) src[i] = (plan.fromDclip[i] ? dclip_ : child)->GetFrame(plan.frame[i], env);
      dst = env->NewVideoFrameP(vi, &propSrc);
      const int planes = vi.IsY() ? 1 : 3;
      const int bytes = vi.ComponentSize();
      for (int p = 0; p < planes; ++p) {
        const int pl = kPlanes[p];
        PlaneRef refs[4];
        for (int i = 0; i < plan.count; ++i) refs[i] = planeOf(src[i], pl);
        renderPlane(plan, refs, dst->GetWritePtr(pl), dst->GetPitch(pl), dst->GetRowSize(pl) / bytes,
                    dst->GetHeight(pl), vi.BitsPerComponent());
      }
    }

    const int s = plan.sourceFrame;
    const uint8_t flags = seq_.fdat[static_cast<size_t>(s)];
    AVSMap* props = env->getFramePropsRW(dst);
    env->propSetInt(props, "ExBlendIndex", flags & IX_MASK, PROPAPPENDMODE_REPLACE);
    env->propSetInt(props, "ExBlendFlags", flags, PROPAPPENDMODE_REPLACE);
    env->propSetInt(props, "ExBlendSource", s, PROPAPPENDMODE_REPLACE);
    env->propSetInt(props, "ExBlendPal", seq_.pal, PROPAPPENDMODE_REPLACE);
    const PairRecord& r = stats_.pair(s);
    env->propSetFloat(props, "ExBlendMD", compat_ ? r.cmd : scaleSum(r.md, stats_.geom), PROPAPPENDMODE_REPLACE);
    env->propSetFloat(props, "ExBlendFD", compat_ ? r.cfd : scaleSum(r.fd, stats_.geom), PROPAPPENDMODE_REPLACE);
    if (op_.decimate) {
      int e1 = 0, e2 = 0;
      int64_t num = env->propGetInt(props, "_DurationNum", 0, &e1);
      int64_t den = env->propGetInt(props, "_DurationDen", 0, &e2);
      if (!e1 && !e2 && num > 0 && den > 0) {
        num *= seq_.cycle;
        den *= seq_.cycle - 1;
        reduce(num, den);
        env->propSetInt(props, "_DurationNum", num, PROPAPPENDMODE_REPLACE);
        env->propSetInt(props, "_DurationDen", den, PROPAPPENDMODE_REPLACE);
      }
    }

    if (show_ && plan.actblend && !vi.IsY()) {
      const int pl = plan.actblend == IX1 ? PLANAR_U : PLANAR_V;
      const int bytes = vi.ComponentSize();
      const int w = std::min(10, dst->GetRowSize(pl) / bytes), h = std::min(10, dst->GetHeight(pl));
      const int v = 239 << (vi.BitsPerComponent() - 8);
      uint8_t* base = dst->GetWritePtr(pl);
      const int pitch = dst->GetPitch(pl);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          if (bytes == 1)
            base[pitch * y + x] = static_cast<uint8_t>(v);
          else
            reinterpret_cast<uint16_t*>(base + pitch * y)[x] = static_cast<uint16_t>(v);
        }
    }
    return dst;
  }

  int __stdcall SetCacheHints(int cachehints, int) override { return cachehints == CACHE_GET_MTMODE ? MT_NICE_FILTER : 0; }

 private:
  PClip dclip_;
  Sequence seq_;
  Stats stats_;
  OutputParams op_;
  bool compat_;
  bool show_;
};

uint64_t fingerprintOf(PClip det, int N, const Geometry& g, IScriptEnvironment* env) {
  uint64_t h[3];
  const int frames[3] = {0, N / 2, N - 1};
  for (int i = 0; i < 3; ++i) h[i] = hashPlane(planeOf(det->GetFrame(frames[i], env), PLANAR_Y), g, 0);
  return ScanState::combineFingerprint(h[0], h[1], h[2]);
}

void requireApi(IScriptEnvironment* env, const char* name) {
  try {
    env->CheckVersion(10);
  } catch (const AvisynthError&) {
    env->ThrowError("%s: needs AviSynth+ 3.7.5 or newer", name);
  }
}

AVSValue __cdecl createScan(AVSValue args, void*, IScriptEnvironment* env) {
  requireApi(env, "ExBlendScan");
  PClip child = args[0].AsClip();
  PClip det = args[2].Defined() ? args[2].AsClip() : child;
  const VideoInfo& vi = child->GetVideoInfo();
  std::string msg = checkFormat(vi, "clip");
  if (msg.empty() && args[2].Defined()) msg = checkDclip(vi, det->GetVideoInfo());
  if (msg.empty() && vi.num_frames < kMinFrames) msg = "clip needs at least 7 frames";
  if (!msg.empty()) env->ThrowError("ExBlendScan: %s", msg.c_str());
  auto state = std::make_shared<ScanState>(vi.num_frames, geometryOf(vi), args[1].AsString(""));
  return new ScanFilter(child, det, state);
}

AVSValue __cdecl createExBlend(AVSValue args, void*, IScriptEnvironment* env) {
  requireApi(env, "ExBlend");
  PClip child = args[0].AsClip();
  const bool hasD = args[2].Defined();
  PClip det = hasD ? args[2].AsClip() : child;
  const VideoInfo& vi = child->GetVideoInfo();
  std::string msg = checkFormat(vi, "clip");
  if (msg.empty() && hasD) msg = checkDclip(vi, det->GetVideoInfo());
  if (msg.empty() && vi.num_frames < kMinFrames) msg = "clip needs at least 7 frames";
  if (!msg.empty()) env->ThrowError("ExBlend: %s", msg.c_str());

  Options opt;
  opt.pal = args[3].AsInt(0);
  opt.lockthresh = args[4].AsFloat(1.0f);
  opt.ithresh = args[5].AsFloat(3.0f);
  opt.fullscan = args[6].AsBool(true);
  opt.compat = args[7].AsBool(false);
  opt.overrideFile = args[8].AsString("");
  OutputParams op;
  op.disp = args[9].AsInt(DISP_EXBLEND);
  op.compub = args[10].AsInt(2);
  op.decimate = args[11].AsBool(true);
  op.decompix = args[12].AsInt(1);
  op.hasDclip = hasD;
  const bool show = args[13].AsBool(false);
  const std::string decfile = args[14].AsString("");
  const int decclpix = args[15].AsInt(1);
  const std::string logfile = args[16].AsString("");
  const std::string statsfile = args[1].AsString("");

  if (opt.pal < -1 || opt.pal > 1) env->ThrowError("ExBlend: pal must be -1 (auto), 0 (film) or 1 (PAL)");
  if (opt.lockthresh < 0 || opt.lockthresh > 100) env->ThrowError("ExBlend: lockthresh must be 0.0 .. 100.0");
  if (opt.ithresh < 0 || opt.ithresh > 100) env->ThrowError("ExBlend: ithresh must be 0.0 .. 100.0");
  if (op.disp < DISP_BLENDED || op.disp > DISP_ADJACENT) env->ThrowError("ExBlend: disp must be 0 .. 4");
  if (op.disp == DISP_ADJACENT && !op.decimate) env->ThrowError("ExBlend: disp=4 needs decimate=true");
  if (op.disp == DISP_SRESTORE && op.decimate) env->ThrowError("ExBlend: disp=2 needs decimate=false");
  if (op.compub < 0 || op.compub > 2) env->ThrowError("ExBlend: compub must be 0 .. 2");
  if (op.decompix != 1 && op.decompix != 2) env->ThrowError("ExBlend: decompix must be 1 or 2");
  if (decclpix > 255) env->ThrowError("ExBlend: decclpix must be 255 or less");
  if (opt.compat && vi.BitsPerComponent() != 8) env->ThrowError("ExBlend: compat=true needs 8-bit input");

  const Geometry g = geometryOf(vi);
  Stats stats;
  bool loaded = false;
  if (!statsfile.empty()) {
    std::string lerr;
    if (loadStats(statsfile, stats, lerr)) {
      loaded = stats.frames == vi.num_frames && stats.geom.width == g.width && stats.geom.height == g.height &&
               stats.geom.bits == g.bits && stats.fingerprint == fingerprintOf(det, vi.num_frames, g, env);
    }
  }
  if (!loaded) {
    auto state = std::make_shared<ScanState>(vi.num_frames, g, statsfile);
    PClip scan = new ScanFilter(det, det, state);
    for (int n = 0; n < vi.num_frames; ++n) scan->GetFrame(n, env);
    if (!state->complete()) env->ThrowError("ExBlend: scan incomplete");
    stats = state->stats();
    if (!statsfile.empty() && !state->writeOnce(msg)) env->ThrowError("ExBlend: %s", msg.c_str());
  }

  Sequence seq;
  if (!buildSequence(stats, opt, seq, msg)) env->ThrowError("ExBlend: %s", msg.c_str());
  if (!decfile.empty() && !writeDecimatedFile(decfile, seq, decclpix, msg))
    env->ThrowError("ExBlend: %s", msg.c_str());
  if (!logfile.empty() && !writeLogFile(logfile, seq, stats, opt, msg)) env->ThrowError("ExBlend: %s", msg.c_str());
  return new ExBlendFilter(child, hasD ? det : nullptr, std::move(seq), std::move(stats), op, opt.compat, show);
}

}  // namespace

const AVS_Linkage* AVS_linkage = nullptr;

#if defined(_WIN32)
#define EXBLEND_AVS_EXPORT extern "C" __declspec(dllexport)
#else
#define EXBLEND_AVS_EXPORT extern "C" __attribute__((visibility("default")))
#endif

EXBLEND_AVS_EXPORT const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors) {
  AVS_linkage = vectors;
  env->AddFunction("ExBlendScan", "c[statsfile]s[dclip]c", createScan, nullptr);
  env->AddFunction("ExBlend",
                   "c[statsfile]s[dclip]c[pal]i[lockthresh]f[ithresh]f[fullscan]b[compat]b[override]s[disp]i"
                   "[compub]i[decimate]b[decompix]i[show]b[decfile]s[decclpix]i[logfile]s",
                   createExBlend, nullptr);
  return "ExBlend " EXBLEND_VERSION " - repairs blend-deinterlaced telecined video";
}
