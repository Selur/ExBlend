// SPDX-License-Identifier: GPL-2.0-or-later
// VapourSynth (API 4) interface: exblend.Scan and exblend.ExBlend for 8-16 bit integer YUV and Gray clips.
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "VSHelper4.h"
#include "VapourSynth4.h"
#include "core/exblend.hpp"
#include "core/scan.hpp"
#include "version.hpp"

using namespace exblend;

namespace {

Geometry geometryOf(const VSVideoInfo* vi) { return {vi->width, vi->height, vi->format.bitsPerSample}; }

PlaneRef planeOf(const VSFrame* f, int plane, const VSAPI* vsapi) {
  return {vsapi->getReadPtr(f, plane), vsapi->getStride(f, plane)};
}

std::string checkFormat(const VSVideoInfo* vi, const char* what) {
  if (!vsh::isConstantVideoFormat(vi)) return std::string(what) + " must have a constant format and size";
  const VSVideoFormat& f = vi->format;
  if (f.sampleType != stInteger || f.bitsPerSample < 8 || f.bitsPerSample > 16 ||
      (f.colorFamily != cfYUV && f.colorFamily != cfGray))
    return std::string(what) + " must be 8-16 bit integer YUV or Gray";
  return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Scan: passes frames through, computes every pair record once and writes the stats file when the clip is done.

struct ScanData {
  VSNode* node = nullptr;   // frames passed on
  VSNode* dnode = nullptr;  // detection clip
  std::shared_ptr<ScanState> state;
};

void setPairProps(VSMap* props, const PairRecord& r, const Geometry& g, const VSAPI* vsapi) {
  vsapi->mapSetFloat(props, "ExBlendMD", scaleSum(r.md, g), maReplace);
  vsapi->mapSetFloat(props, "ExBlendFD", scaleSum(r.fd, g), maReplace);
  vsapi->mapSetFloat(props, "ExBlendLD", scaleSum(r.ld, g), maReplace);
}

const VSFrame* VS_CC scanGetFrame(int n, int reason, void* instance, void**, VSFrameContext* ctx, VSCore*,
                                  const VSAPI* vsapi) {
  auto* d = static_cast<ScanData*>(instance);
  ScanState& st = *d->state;
  const int N = st.frames();
  const int lo = std::max(0, n - 1), hi = std::min(N - 1, n + 2);
  if (reason == arInitial) {
    for (int k = lo; k <= hi; ++k) vsapi->requestFrameFilter(k, d->dnode, ctx);
    if (d->node != d->dnode) vsapi->requestFrameFilter(n, d->node, ctx);
    return nullptr;
  }
  if (reason != arAllFramesReady) return nullptr;

  const VSFrame* det[4] = {nullptr, nullptr, nullptr, nullptr};
  for (int k = lo; k <= hi; ++k) det[k - lo] = vsapi->getFrameFilter(k, d->dnode, ctx);
  auto detFrame = [&](int k) { return det[std::min(std::max(k, 0), N - 1) - lo]; };
  const Geometry& g = st.geom();
  for (int i = 0; i < 3; ++i)
    if (st.fingerprintFrame(i) == n) st.setFingerprint(n, hashPlane(planeOf(detFrame(n), 0, vsapi), g, 0));

  int first, last;
  st.pairsOf(n, first, last);
  bool completed = false;
  PairRecord own;
  for (int p = first; p <= last; ++p) {
    PlaneRef planes[4];
    for (int i = 0; i < 4; ++i) planes[i] = planeOf(detFrame(p - 1 + i), 0, vsapi);
    PairRecord r;
    computePair(planes, g, r);
    if (p == n) own = r;
    if (st.claim(p)) completed = st.store(p, r) || completed;
  }

  const VSFrame* src = d->node == d->dnode ? vsapi->addFrameRef(detFrame(n)) : vsapi->getFrameFilter(n, d->node, ctx);
  for (const VSFrame* f : det) vsapi->freeFrame(f);
  VSFrame* dst = vsapi->copyFrame(src, nullptr);
  vsapi->freeFrame(src);
  setPairProps(vsapi->getFramePropertiesRW(dst), own, g, vsapi);

  if (completed) {
    std::string err;
    if (!st.writeOnce(err)) {
      vsapi->freeFrame(dst);
      vsapi->setFilterError(("ExBlend: " + err).c_str(), ctx);
      return nullptr;
    }
  }
  return dst;
}

void VS_CC scanFree(void* instance, VSCore*, const VSAPI* vsapi) {
  auto* d = static_cast<ScanData*>(instance);
  if (d->node != d->dnode) vsapi->freeNode(d->dnode);
  vsapi->freeNode(d->node);
  delete d;
}

// ---------------------------------------------------------------------------------------------------------------
// ExBlend

struct ExBlendData {
  VSNode* node = nullptr;
  VSNode* dnode = nullptr;  // detection clip or nullptr
  VSVideoInfo vi{};
  Sequence seq;
  Stats stats;
  OutputParams op;
  bool compat = false;
  bool show = false;
};

const VSFrame* VS_CC exblendGetFrame(int n, int reason, void* instance, void**, VSFrameContext* ctx, VSCore* core,
                                     const VSAPI* vsapi) {
  auto* d = static_cast<ExBlendData*>(instance);
  const FramePlan plan = planFrame(d->seq, d->op, n);
  if (reason == arInitial) {
    for (int i = 0; i < plan.count; ++i)
      vsapi->requestFrameFilter(plan.frame[i], plan.fromDclip[i] ? d->dnode : d->node, ctx);
    vsapi->requestFrameFilter(plan.sourceFrame, d->node, ctx);
    return nullptr;
  }
  if (reason != arAllFramesReady) return nullptr;

  const VSFrame* propSrc = vsapi->getFrameFilter(plan.sourceFrame, d->node, ctx);
  VSFrame* dst = nullptr;
  if (plan.kind == FramePlan::COPY && !plan.fromDclip[0] && plan.frame[0] == plan.sourceFrame) {
    dst = vsapi->copyFrame(propSrc, core);
  } else {
    const VSFrame* src[4] = {nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < plan.count; ++i)
      src[i] = vsapi->getFrameFilter(plan.frame[i], plan.fromDclip[i] ? d->dnode : d->node, ctx);
    const VSVideoFormat& fmt = d->vi.format;
    dst = vsapi->newVideoFrame(&fmt, d->vi.width, d->vi.height, propSrc, core);
    for (int p = 0; p < fmt.numPlanes; ++p) {
      PlaneRef planes[4];
      for (int i = 0; i < plan.count; ++i) planes[i] = planeOf(src[i], p, vsapi);
      renderPlane(plan, planes, vsapi->getWritePtr(dst, p), vsapi->getStride(dst, p), vsapi->getFrameWidth(dst, p),
                  vsapi->getFrameHeight(dst, p), fmt.bitsPerSample);
    }
    for (const VSFrame* f : src) vsapi->freeFrame(f);
  }
  vsapi->freeFrame(propSrc);

  const int s = plan.sourceFrame;
  const uint8_t flags = d->seq.fdat[static_cast<size_t>(s)];
  VSMap* props = vsapi->getFramePropertiesRW(dst);
  vsapi->mapSetInt(props, "ExBlendIndex", flags & IX_MASK, maReplace);
  vsapi->mapSetInt(props, "ExBlendFlags", flags, maReplace);
  vsapi->mapSetInt(props, "ExBlendSource", s, maReplace);
  vsapi->mapSetInt(props, "ExBlendPal", d->seq.pal, maReplace);
  vsapi->mapSetFloat(props, "ExBlendScore", d->seq.score[d->seq.pal], maReplace);
  const PairRecord& r = d->stats.pair(s);
  vsapi->mapSetFloat(props, "ExBlendMD", d->compat ? r.cmd : scaleSum(r.md, d->stats.geom), maReplace);
  vsapi->mapSetFloat(props, "ExBlendFD", d->compat ? r.cfd : scaleSum(r.fd, d->stats.geom), maReplace);
  if (d->op.decimate) {
    int err = 0;
    int64_t num = vsapi->mapGetInt(props, "_DurationNum", 0, &err);
    int64_t den = err ? 0 : vsapi->mapGetInt(props, "_DurationDen", 0, &err);
    if (!err && num > 0 && den > 0) {
      num *= d->seq.cycle;
      den *= d->seq.cycle - 1;
      vsh::reduceRational(&num, &den);
      vsapi->mapSetInt(props, "_DurationNum", num, maReplace);
      vsapi->mapSetInt(props, "_DurationDen", den, maReplace);
    }
  }

  // Marks frames recovered from a blend pair: blue block (U) for index 1, red block (V) for index 2.
  if (d->show && plan.actblend && d->vi.format.numPlanes == 3) {
    const int plane = plan.actblend == IX1 ? 1 : 2;
    const int w = std::min(10, vsapi->getFrameWidth(dst, plane)), h = std::min(10, vsapi->getFrameHeight(dst, plane));
    const int v = 239 << (d->vi.format.bitsPerSample - 8);
    uint8_t* base = vsapi->getWritePtr(dst, plane);
    const ptrdiff_t stride = vsapi->getStride(dst, plane);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        if (d->vi.format.bytesPerSample == 1)
          base[stride * y + x] = static_cast<uint8_t>(v);
        else
          reinterpret_cast<uint16_t*>(base + stride * y)[x] = static_cast<uint16_t>(v);
      }
  }
  return dst;
}

void VS_CC exblendFree(void* instance, VSCore*, const VSAPI* vsapi) {
  auto* d = static_cast<ExBlendData*>(instance);
  vsapi->freeNode(d->node);
  vsapi->freeNode(d->dnode);
  delete d;
}

std::string mapString(const VSMap* in, const char* key, const VSAPI* vsapi) {
  int err = 0;
  const char* s = vsapi->mapGetData(in, key, 0, &err);
  return err || !s ? std::string() : std::string(s);
}

int64_t mapIntOr(const VSMap* in, const char* key, int64_t def, const VSAPI* vsapi) {
  int err = 0;
  const int64_t v = vsapi->mapGetInt(in, key, 0, &err);
  return err ? def : v;
}

double mapFloatOr(const VSMap* in, const char* key, double def, const VSAPI* vsapi) {
  int err = 0;
  const double v = vsapi->mapGetFloat(in, key, 0, &err);
  return err ? def : v;
}

// Checks the detection clip against the clip; returns an error message or an empty string.
std::string checkDclip(const VSVideoInfo* vi, const VSVideoInfo* dvi) {
  if (!vsh::isSameVideoInfo(vi, dvi)) return "dclip must have the same format and size as clip";
  if (dvi->numFrames != vi->numFrames) return "dclip must have the same number of frames as clip";
  if (dvi->fpsNum != vi->fpsNum || dvi->fpsDen != vi->fpsDen) return "dclip must have the same frame rate as clip";
  return {};
}

uint64_t fingerprintOf(VSNode* det, int N, const Geometry& g, const VSAPI* vsapi, std::string& err) {
  uint64_t h[3] = {0, 0, 0};
  const int frames[3] = {0, N / 2, N - 1};
  char buf[1024];
  for (int i = 0; i < 3; ++i) {
    const VSFrame* f = vsapi->getFrame(frames[i], det, buf, sizeof buf);
    if (!f) {
      err = buf;
      return 0;
    }
    h[i] = hashPlane(planeOf(f, 0, vsapi), g, 0);
    vsapi->freeFrame(f);
  }
  return ScanState::combineFingerprint(h[0], h[1], h[2]);
}

// Renders the scan node completely, several frames in flight; returns an error message or an empty string.
std::string runScan(VSNode* scanNode, int N, VSCore* core, const VSAPI* vsapi) {
  struct Wait {
    std::mutex m;
    std::condition_variable cv;
    int inFlight = 0;
    int done = 0;
    std::string error;
    const VSAPI* vsapi;
  } w;
  w.vsapi = vsapi;
  VSCoreInfo info;
  vsapi->getCoreInfo(core, &info);
  const int limit = std::max(4, info.numThreads * 2);
  auto cb = [](void* user, const VSFrame* f, int, VSNode*, const char* errorMsg) {
    auto* w = static_cast<Wait*>(user);
    w->vsapi->freeFrame(f);
    std::lock_guard<std::mutex> lock(w->m);
    if (errorMsg && w->error.empty()) w->error = errorMsg;
    --w->inFlight;
    ++w->done;
    w->cv.notify_all();
  };
  int nextReport = 10;
  for (int n = 0; n < N; ++n) {
    {
      std::unique_lock<std::mutex> lock(w.m);
      w.cv.wait(lock, [&] { return w.inFlight < limit; });
      if (!w.error.empty()) break;
      ++w.inFlight;
      const int percent = w.done * 100 / N;
      if (percent >= nextReport) {
        vsapi->logMessage(mtInformation, ("ExBlend: scan " + std::to_string(percent) + "%").c_str(), core);
        nextReport = percent / 10 * 10 + 10;
      }
    }
    vsapi->getFrameAsync(n, scanNode, cb, &w);
  }
  std::unique_lock<std::mutex> lock(w.m);
  w.cv.wait(lock, [&] { return w.inFlight == 0; });
  return w.error;
}

VSNode* createScanNode(VSNode* node, VSNode* dnode, std::shared_ptr<ScanState> state, VSCore* core,
                       const VSAPI* vsapi) {
  auto* d = new ScanData();
  d->node = vsapi->addNodeRef(node);
  d->dnode = dnode == node ? d->node : vsapi->addNodeRef(dnode);
  d->state = std::move(state);
  VSFilterDependency deps[2] = {{d->node, rpGeneral}, {d->dnode, rpGeneral}};
  return vsapi->createVideoFilter2("ExBlendScan", vsapi->getVideoInfo(node), scanGetFrame, scanFree, fmParallel, deps,
                                   d->node == d->dnode ? 1 : 2, d, core);
}

void VS_CC scanCreate(const VSMap* in, VSMap* out, void*, VSCore* core, const VSAPI* vsapi) {
  VSNode* node = vsapi->mapGetNode(in, "clip", 0, nullptr);
  int err = 0;
  VSNode* dnode = vsapi->mapGetNode(in, "dclip", 0, &err);
  if (err) dnode = nullptr;
  auto fail = [&](const std::string& msg) {
    vsapi->mapSetError(out, ("Scan: " + msg).c_str());
    vsapi->freeNode(node);
    vsapi->freeNode(dnode);
  };
  const VSVideoInfo* vi = vsapi->getVideoInfo(node);
  std::string msg = checkFormat(vi, "clip");
  if (msg.empty() && dnode) msg = checkDclip(vi, vsapi->getVideoInfo(dnode));
  if (msg.empty() && vi->numFrames < kMinFrames) msg = "clip needs at least 7 frames";
  if (!msg.empty()) return fail(msg);
  auto state = std::make_shared<ScanState>(vi->numFrames, geometryOf(vi), mapString(in, "statsfile", vsapi));
  VSNode* scan = createScanNode(node, dnode ? dnode : node, state, core, vsapi);
  vsapi->freeNode(node);
  vsapi->freeNode(dnode);
  vsapi->mapConsumeNode(out, "clip", scan, maReplace);
}

void VS_CC exblendCreate(const VSMap* in, VSMap* out, void*, VSCore* core, const VSAPI* vsapi) {
  auto d = std::make_unique<ExBlendData>();
  d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
  int err = 0;
  d->dnode = vsapi->mapGetNode(in, "dclip", 0, &err);
  if (err) d->dnode = nullptr;
  auto fail = [&](const std::string& msg) {
    vsapi->mapSetError(out, ("ExBlend: " + msg).c_str());
    vsapi->freeNode(d->node);
    vsapi->freeNode(d->dnode);
  };

  const VSVideoInfo* vi = vsapi->getVideoInfo(d->node);
  d->vi = *vi;
  std::string msg = checkFormat(vi, "clip");
  if (msg.empty() && d->dnode) msg = checkDclip(vi, vsapi->getVideoInfo(d->dnode));
  if (msg.empty() && vi->numFrames < kMinFrames) msg = "clip needs at least 7 frames";
  if (!msg.empty()) return fail(msg);

  Options opt;
  opt.pal = static_cast<int>(mapIntOr(in, "pal", 0, vsapi));
  opt.lockthresh = mapFloatOr(in, "lockthresh", 1.0, vsapi);
  opt.ithresh = mapFloatOr(in, "ithresh", 3.0, vsapi);
  opt.fullscan = mapIntOr(in, "fullscan", 1, vsapi) != 0;
  opt.compat = mapIntOr(in, "compat", 0, vsapi) != 0;
  opt.overrideFile = mapString(in, "override", vsapi);
  d->op.disp = static_cast<int>(mapIntOr(in, "disp", DISP_EXBLEND, vsapi));
  d->op.compub = static_cast<int>(mapIntOr(in, "compub", 2, vsapi));
  d->op.decimate = mapIntOr(in, "decimate", 1, vsapi) != 0;
  d->op.decompix = static_cast<int>(mapIntOr(in, "decompix", 1, vsapi));
  d->op.hasDclip = d->dnode != nullptr;
  d->show = mapIntOr(in, "show", 0, vsapi) != 0;
  d->compat = opt.compat;
  const int decclpix = static_cast<int>(mapIntOr(in, "decclpix", 1, vsapi));

  if (opt.pal < -1 || opt.pal > 1) return fail("pal must be -1 (auto), 0 (film) or 1 (PAL)");
  if (opt.lockthresh < 0 || opt.lockthresh > 100) return fail("lockthresh must be 0.0 .. 100.0");
  if (opt.ithresh < 0 || opt.ithresh > 100) return fail("ithresh must be 0.0 .. 100.0");
  if (d->op.disp < DISP_BLENDED || d->op.disp > DISP_ADJACENT) return fail("disp must be 0 .. 4");
  if (d->op.disp == DISP_ADJACENT && !d->op.decimate) return fail("disp=4 needs decimate=1");
  if (d->op.disp == DISP_SRESTORE && d->op.decimate) return fail("disp=2 needs decimate=0");
  if (d->op.compub < 0 || d->op.compub > 2) return fail("compub must be 0 .. 2");
  if (d->op.decompix != 1 && d->op.decompix != 2) return fail("decompix must be 1 or 2");
  if (decclpix > 255) return fail("decclpix must be 255 or less");
  if (opt.compat && vi->format.bitsPerSample != 8) return fail("compat=1 needs 8-bit input");

  VSNode* det = d->dnode ? d->dnode : d->node;
  const Geometry g = geometryOf(vi);
  const std::string statsfile = mapString(in, "statsfile", vsapi);
  bool loaded = false;
  if (!statsfile.empty()) {
    std::string lerr;
    Stats s;
    if (loadStats(statsfile, s, lerr)) {
      const uint64_t fp = fingerprintOf(det, vi->numFrames, g, vsapi, lerr);
      if (!lerr.empty()) return fail(lerr);
      loaded = s.frames == vi->numFrames && s.geom.width == g.width && s.geom.height == g.height &&
               s.geom.bits == g.bits && s.fingerprint == fp;
      if (loaded) d->stats = std::move(s);
    }
  }
  if (!loaded) {
    auto state = std::make_shared<ScanState>(vi->numFrames, g, statsfile);
    VSNode* scan = createScanNode(det, det, state, core, vsapi);
    msg = runScan(scan, vi->numFrames, core, vsapi);
    vsapi->freeNode(scan);
    if (!msg.empty()) return fail(msg);
    if (!state->complete()) return fail("scan incomplete");
    d->stats = state->stats();
    if (!statsfile.empty() && !state->writeOnce(msg)) return fail(msg);
  }

  if (!buildSequence(d->stats, opt, d->seq, msg)) return fail(msg);
  if (opt.pal == -1) vsapi->logMessage(mtInformation, ("ExBlend: " + d->seq.log.front()).c_str(), core);
  if (!d->seq.warning.empty()) vsapi->logMessage(mtWarning, ("ExBlend: " + d->seq.warning).c_str(), core);
  const std::string decfile = mapString(in, "decfile", vsapi);
  if (!decfile.empty() && !writeDecimatedFile(decfile, d->seq, decclpix, msg)) return fail(msg);
  const std::string logfile = mapString(in, "logfile", vsapi);
  if (!logfile.empty() && !writeLogFile(logfile, d->seq, d->stats, opt, msg)) return fail(msg);

  if (d->op.decimate) {
    d->vi.numFrames = static_cast<int>(d->seq.decimatedToSource.size());
    if (d->vi.fpsNum > 0) vsh::muldivRational(&d->vi.fpsNum, &d->vi.fpsDen, d->seq.cycle - 1, d->seq.cycle);
  }
  VSFilterDependency deps[2] = {{d->node, rpGeneral}, {d->dnode, rpGeneral}};
  const int numDeps = d->dnode ? 2 : 1;
  ExBlendData* data = d.release();
  vsapi->createVideoFilter(out, "ExBlend", &data->vi, exblendGetFrame, exblendFree, fmParallel, deps, numDeps, data,
                           core);
}

}  // namespace

void registerAnimDeblend(VSPlugin* plugin, const VSPLUGINAPI* vspapi);  // vs_anim_deblend.cpp

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
  vspapi->configPlugin("io.github.selur.exblend", "exblend", "ExBlend " EXBLEND_VERSION,
                       VS_MAKE_VERSION(EXBLEND_VERSION_MAJOR, EXBLEND_VERSION_MINOR), VAPOURSYNTH_API_VERSION, 0,
                       plugin);
  vspapi->registerFunction("Scan", "clip:vnode;statsfile:data:opt;dclip:vnode:opt;", "clip:vnode;", scanCreate,
                           nullptr, plugin);
  vspapi->registerFunction("ExBlend",
                           "clip:vnode;statsfile:data:opt;dclip:vnode:opt;pal:int:opt;lockthresh:float:opt;"
                           "ithresh:float:opt;fullscan:int:opt;compat:int:opt;override:data:opt;disp:int:opt;"
                           "compub:int:opt;decimate:int:opt;decompix:int:opt;show:int:opt;decfile:data:opt;"
                           "decclpix:int:opt;logfile:data:opt;",
                           "clip:vnode;", exblendCreate, nullptr, plugin);
  registerAnimDeblend(plugin, vspapi);
}
