# Notes for Claude

## Project

Port of StainlessS' AviSynth plugin ExBlend v1.04 (GPL source, `F:\workspace\ExBlendAvisynth`, not part of this
repository): one plugin binary for AviSynth+ 3.7.5+ (`AvisynthPluginInit3`, functions `ExBlend`, `ExBlendScan`) and
VapourSynth API 4 (`exblend.ExBlend`, `exblend.Scan`). 8..16 bit integer planar YUV and Gray. User documentation is
`README.md`.

- `src/core/exblend.*`: host-independent engine.
  - `computePair()`: detector sums of one frame pair (p, p+1) with neighbours p-1, p+2. The v1.04 7-frame window of
    `Findblend()` is exactly the pairs n-2..n+1, so every frame needs one pair record (`Stats::pair(p)`, p = -2..N).
    Each record holds exact 64-bit sums and the v1.04 float sums (`cmd`/`cfd`/`cld`, per-pixel float accumulation in
    the original order, 8 bit only since `compat` needs 8 bit) so `compat` needs no rescan. Strict single precision matters (`/fp:precise`,
    `-ffp-contract=off`).
  - `buildSequence()`: override parser, the decision pass of v1.04 `GetFrame()` mode 1 run forward over all frames
    (`Decider::decide<T>`, `float` for compat, `double` otherwise), decimation choice of the v1.04 destructor.
    `fdat` bytes have the XBD layout. `pal=-1` runs the pass for both variants and keeps the one with clearly more
    continued cycles (`kAutoMinScore`, `kAutoMinLead`, measured on `G:\clips\blends`), film when undecided.
  - `planFrame()`/`renderPlane()`: which input frames make an output frame (v1.04 mode 2 branches) and the pixel ops.
- `src/core/scan.hpp`: `ScanState` collects pair records from any thread in any order; each frame n owns pair n
  (frame 0 also -2, -1; the last frame also N); the stats file is written by the call that completes the clip.
- `src/vs_plugin.cpp`: `ExBlend` scans in the constructor through an internal `Scan` node with `getFrameAsync`
  (several frames in flight), `src/avs_plugin.cpp` scans sequentially. Both `fmParallel`/`MT_NICE_FILTER` afterwards.
- Defects of v1.04 fixed by default and restored by `compat=1`: float sums, frame 0 weight 0 in decimation, phase
  lock after a static stretch (`canRephase()`/`rephase()`, thresholds `kRephaseMinFD`, `kRephaseRatio`, rhythm rule).
  `fullscan=0` restores the decisions of v1.04 with `show` <= 1. Keep every other behaviour identical to v1.04.
- `exblend.AnimDeblend` (VapourSynth only): port of `prototype/anim_deblend.py`, which stays the reference for
  experiments. `src/core/anim_deblend.*` mirrors the prototype function by function (same tie-breaking, numpy
  conventions for percentile and histogram); `src/vs_anim_deblend.cpp` analyses per block on first request
  (`ensurePart`, block mutex). The analysing thread must be a VapourSynth worker: its synchronous `getFrame` gets a
  reserved thread; an own thread analysing ahead (or a request to the filter's own node) deadlocked when all worker
  threads waited on the block. Per-frame ghost work runs on `parallelFor`; the 3x3 box blur is computed in C++
  (a `std.BoxBlur` node in the read path cost more than the whole ghost analysis). Rule changes go into the prototype
  first, then here; keep both deciding alike (`test/compare_prototype.py`).

## Tests

- `build.bat` builds `build\ExBlend.dll` and `build\core_test.exe` (MSVC 18, static runtime).
- `test/core_test.cpp`: pair records against the v1.04 detector code on random frames (exact float compare).
- `test/test_exblend.py`: VapourSynth functional tests on synthetic blend clips (known film frames); runs in CI. Locally
  with Hybrid's VapourSynth: `F:\Hybrid\64bit\Vapoursynth\python.exe -I test\test_exblend.py build\ExBlend.dll`.
- `test/compare_original.py` (Windows, `F:\Python4Claude\python.exe`): AviSynth+ via `avs2yuv64`, port with
  `compat=true` against the original `ExBlend_x64.dll`: XBD bytes (show=5 / show=0 against fullscan=1 / 0),
  `_Decimated.TXT`, MD5 of every output frame for all disp/decimate/compub/decompix cases. Clips from
  `G:\clips\blends`, decoded once to y4m with ffmpeg, read with BestSource in both hosts.
- `test/compare_hosts.py` (Hybrid's VapourSynth Python): VapourSynth against AviSynth+ output of the same build,
  stats files byte-identical.
- `test/test_animdeblend.py`: AnimDeblend on synthetic blocky drawings (known mixes and ghosts); runs in CI. The clips
  start with held frames: without holds the noise estimate measures motion and every frame looks like a blend.
- `test/compare_prototype.py` (Hybrid's VapourSynth Python): AnimDeblend decisions against the prototype on real clips;
  identical apart from rare borderline frames.
- Work files go to `J:\tmp\claude\exblend\`.

## Build

- `meson setup build && ninja -C build`; on Windows with MSVC `meson setup build --vsenv -Db_vscrt=mt`.
- Wheel: `pip install .` (meson-python, `-Dpython_package=true` installs into `site-packages/vapoursynth/plugins`).
- `.github/workflows/build.yml` builds and tests wheels for Windows x64, Linux x86_64 and macOS arm64 with
  cibuildwheel, zips the bare plugin for AviSynth+ and attaches both to a GitHub release for `v*` tags. Only CI builds
  Linux and macOS; locally only the Windows build is checked.

## Versioning

Semantic versioning, the same number in the git tag, `meson.build` and `src/version.hpp`. Never bump on your own
initiative; propose it. 2.0.0 is the first version of the port (v1.04 was the last AviSynth-only release).
