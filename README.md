# ExBlend

Repairs **blend-deinterlaced telecined video** for **VapourSynth** and **AviSynth+**: material where 3:2 pulldown
was deinterlaced by blending (or vertically halved, e.g. NTSC DVD → VCD), so that every cycle of five frames holds
three good frames and two blends (`GGGBB`; PAL origin `GGGGBB`). ExBlend finds the blend pairs, recovers the
missing film frame from them and, by default, decimates back to the film rate.

This is a port of StainlessS' AviSynth plugin ExBlend v1.04 (2018,
[doom9 thread](https://forum.doom9.org/showthread.php?t=175948)). One file serves both hosts: 64-bit AviSynth+ 3.7.5
or newer and VapourSynth R55 or newer (API 4). 8–16 bit integer planar YUV and Gray.

## How it works

For every pair of neighbouring frames *(n, n+1)* ExBlend tries to unblend the middle film frame twice
(`2·n − (n−1)` and `2·(n+1) − (n+2)`) and measures how well both candidates agree (merge delta, MD) against the
plain frame difference (FD). A blend pair shows up as an MD that is smaller than FD and smaller than the MD of the
neighbouring pairs. A sequential decision pass turns these measurements into blend indexes, with hints (no further
blend for three or four frames on either side), locking for near-static scenes and an ignore threshold.
The output frame of a blend pair is the average of both candidates; decimation drops one frame per cycle.

The decision pass needs the measurements of the whole clip, so ExBlend scans the clip once before the first frame
is delivered and keeps the measurements in a **stats file**. The stats file holds measurements, not decisions:
`pal`, `lockthresh`, `ithresh`, `fullscan`, `compat` and the override file can be changed without a new scan. A
changed clip (other frame count, size, depth or content of three sample frames) is rescanned automatically.

## Usage

VapourSynth:

```python
clip = core.exblend.ExBlend(clip, statsfile="movie.exbstats")                 # repair and decimate
clip = core.exblend.ExBlend(clip, statsfile="movie.exbstats",
                            dclip=core.std.BoxBlur(clip, hradius=0, vradius=1))  # detection on a blurred clip
```

AviSynth+:

```
ExBlend(statsfile="movie.exbstats")
ExBlend(statsfile="movie.exbstats", dclip=Blur(0, 1.0))
```

Without `statsfile` the scan runs every time the script is loaded. To scan with a progress display (e.g. vspipe),
render `Scan` once, then load the script with `ExBlend`:

```python
core.exblend.Scan(clip, statsfile="movie.exbstats")   # vspipe -p scan.vpy -- ; writes the file after the last frame
```

```
ExBlendScan(statsfile="movie.exbstats")               # AviSynth+: render all frames once
```

## Functions

```
exblend.ExBlend(vnode clip[, data statsfile, vnode dclip, int pal=0, float lockthresh=1.0, float ithresh=3.0,
                int fullscan=1, int compat=0, data override, int disp=1, int compub=2, int decimate=1,
                int decompix=1, int show=0, data decfile, int decclpix=1, data logfile])
exblend.Scan(vnode clip[, data statsfile, vnode dclip])

ExBlend(clip clip, string "statsfile", clip "dclip", int "pal", float "lockthresh", float "ithresh",
        bool "fullscan", bool "compat", string "override", int "disp", int "compub", bool "decimate",
        int "decompix", bool "show", string "decfile", int "decclpix", string "logfile")
ExBlendScan(clip clip, string "statsfile", clip "dclip")
```

| Parameter | Default | Meaning |
|---|---|---|
| `statsfile` | none | File for the scan measurements; read when it belongs to the clip, otherwise (re)written. |
| `dclip` | none | Detection clip (same format, size, length and rate), e.g. denoised or vertically blurred. Detection always uses it; `compub` decides whether unblending does. |
| `pal` | 0 | 0: film origin, cycle of 5 (3 good + 2 blends). 1: PAL origin, cycle of 6. -1: automatic, see below. |
| `lockthresh` | 1.0 | 0.0–100.0. Locks onto the sequence in near-static scenes when the lock metric is at or below this value; 0 switches locking off. |
| `ithresh` | 3.0 | 0.0–100.0. A detected pair whose merge delta is above this value is ignored (protects against wrong unblends at scene changes; raise it for poor clips with many ignored pairs). |
| `fullscan` | 1 | 1: the detector evaluates every frame. 0: frames already decided by a neighbouring blend pair are skipped, as v1.04 did with `show=0`/`1`; gives slightly different decisions. |
| `compat` | 0 | 1: reproduce v1.04 exactly, including its defects (float sums, frame 0 decimation). 8-bit only. For comparisons. |
| `override` | none | Manual override file, see below. |
| `disp` | 1 | What replaces a blend frame. 0: the blend itself (from `dclip` if `compub` ≥ 1). 1: the recovered frame. 2: SRestore style, recovered frame at index 1 and the frame after the pair at index 2 (`decimate=0` only). 3: the component frame of the index (`decompix` selects it when decimating). 4: the frame next to the blend selected by `decompix` (`decimate=1` only). Non-blend frames always come from `clip`. |
| `compub` | 2 | With `dclip`: 0 unblends from `clip` only, 1 takes the blends from `dclip` and their neighbours from `clip`, 2 takes both from `dclip`. |
| `decimate` | 1 | Drops one frame per cycle (rate × 4/5 or × 5/6); frame durations are adjusted. |
| `decompix` | 1 | With `decimate=1` and `disp` 0, 3 or 4: 1 or 2 selects the first or second blend of the pair. |
| `show` | 0 | 1: marks frames from a blend pair with a block in the top left corner (blue for index 1, red for index 2). |
| `decfile` | none | Writes the output frame numbers of the recovered frames in the decimated clip, as ClipClop command file (`decclpix` = clip index, < 0 for none). |
| `logfile` | none | Writes events (new/lost sequence, locked, ignored, overrides), the blend pair ratio and one line per frame. |

### Frame properties

`ExBlend` sets on every output frame: `ExBlendIndex` (0 none, 1/2 blend index, 3 hinted), `ExBlendFlags` (raw
decision byte, as in v1.04 XBD files), `ExBlendSource` (input frame number), `ExBlendPal` (0 film, 1 PAL; the variant used, also with `pal=-1`), `ExBlendMD`, `ExBlendFD` (metrics of the
pair starting at that input frame). `Scan` sets `ExBlendMD`, `ExBlendFD`, `ExBlendLD`.

### Automatic film/PAL choice

With `pal=-1` the decision pass runs for both variants. For each, the share of cycles whose blend pair continues
exactly one cycle later is measured. A variant is used when it reaches at least 0.5 and twice the value of the
other; otherwise the clip counts as undecided and film is used. On real clips the right variant scores 0.5–0.98
and the wrong one at most 0.2; clips without a clean blend pattern give 0.4 or less for both. The scores and the
choice are the first line of the log file (`AUTO: film=… PAL=… -> …`) and, in VapourSynth, an information message.
The choice holds for the whole clip; process clips that mix both in parts (`Trim`) with a fixed `pal` each.

### Checking a clip

The log file ends with `BlendPairs=… Ratio=… (…%)`. A clean film-origin clip gives a ratio of about 3 (4 for PAL)
with 95 % or more of the possible pairs found; below about 90 % the clip is probably not suitable (triple blends,
field blends, combing, wrong `pal`). The input must be progressive: combing looks like blends.

### Override file

One command per line, in ascending frame order; `#` starts a comment line.

```
444=h2      # frames 444 and 445: hinted (no blend)
555         # short blend pair: 554 hinted, 555 index 1, 556 index 2, 557 hinted
666=*4      # four long blend pairs from 666 on: (H12HHH) repeated, (H12HHHH) with pal=1
```

`2=*1000000` forces a clip that is known to stay in sequence, with its first pair at frame 2.

## Differences from ExBlend v1.04

- No modes. The scan replaces mode 1, the output replaces mode 2; the stats file replaces the XBD file and stores
  measurements, so parameter changes need no new scan. The scan no longer depends on the clip being freed after a
  complete pass.
- Fixed:
  - The detector summed every pixel into a 32-bit float; above 2^24 the sums lose precision, which can move decisions
    on large or high-motion frames. Sums are exact 64-bit integers now.
  - Decimation compared frame 0 with itself (difference 0) and so dropped frame 0 from the first cycle whenever the
    first cycle held no second blend index; frame 0 is never preferred now.
  - Decisions depended on `show` (with `show` ≤ 1 some frames skipped the detector). They do not depend on display
    settings now; `fullscan=0` keeps the old variant.
  - Invalid combinations (`disp=2` with decimation, `disp=4` without) are errors instead of silently using `disp=1`.
- `compat=1` brings back the first two behaviours for 8-bit comparisons with the original.
- Changed: `pal=-1` decides once for the whole clip after the scan instead of switching while frames are delivered
  (v1.04 mode 0 only); it works with decimation and does not depend on the frame order.
- Removed: mode 0 (free running), the text overlay (`show` 2–5; use the frame properties, e.g.
  `text.FrameProps`), `ver`, `revip`, log levels (`lv`, `dv`, `debug`, DebugView), YUY2.
- New: 9–16 bit (thresholds keep their 8-bit units), Gray, all chroma subsamplings, VapourSynth, frame properties,
  `#` comments in override files.

## Building

`meson setup build && ninja -C build` (Windows with MSVC: `meson setup build --vsenv -Db_vscrt=mt`), or `build.bat`
for a local MSVC build. Wheel: `pip install .`.

Tests: `test/test_exblend.py` (VapourSynth, synthetic clips with known film frames; runs in CI),
`test/core_test.cpp` (pair metrics against the v1.04 detector code), and on Windows `test/compare_original.py`
(AviSynth+, port with `compat=true` against the original DLL: decisions, decimated frames file and every output frame)
and `test/compare_hosts.py` (VapourSynth against AviSynth+ output of the same build).

## License

GPL-2.0-or-later. Original ExBlend © 2009–2018 Stephen Jones (StainlessS), GPL.
