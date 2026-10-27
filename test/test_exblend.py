#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Functional tests for the ExBlend VapourSynth plugin (API 4).

Usage: python test/test_exblend.py [path/to/ExBlend plugin]

Without an argument the plugin is expected to be autoloaded (e.g. from the installed wheel).
The test clips are synthetic: a moving texture as "film", turned into the blend-deinterlaced telecine pattern
A, B, (B+C)/2, (C+D)/2, D (PAL: A, B, C, (C+D)/2, (D+E)/2, E), so the true frames are known.
"""
import math
import random
import os
import sys
import tempfile

import vapoursynth as vs

core = vs.core
FAILURES = []
W, H, FILM = 320, 240, 240


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL: " + msg)
        return
    print("ok   " + msg)


def expect_error(fn, msg, contains):
    try:
        fn()
    except vs.Error as e:
        check(contains in str(e), "%s (%s)" % (msg, str(e).strip().splitlines()[-1]))
    else:
        check(False, "no error raised: " + msg)


_TEXTURE = None


def texture():
    """Large seeded random texture (YUV444P8), smoothed by upscaling; built once."""
    global _TEXTURE
    if _TEXTURE is None:
        tw, th = 96, 72
        rnd = random.Random(1234)
        values = [[rnd.randint(40, 215) for _ in range(tw * th)] for _ in range(3)]
        tiny = core.std.BlankClip(width=tw, height=th, format=vs.YUV444P8, length=1)

        def fill(n, f):
            out = f.copy()
            for p in range(3):
                mem = out[p]
                for y in range(th):
                    for x in range(tw):
                        mem[y, x] = values[p][y * tw + x]
            return out

        tiny = core.std.ModifyFrame(tiny, tiny, fill)
        _TEXTURE = core.resize.Bicubic(tiny, (W + FILM * 3) // 8 * 8 + 8, (H + FILM * 2) // 8 * 8 + 8)
    return _TEXTURE


def film(fmt=vs.YUV420P8, frames=FILM):
    """Texture panning 3 px right and 2 px down per frame: every frame differs, motion is smooth."""
    tex = texture()
    parts = [core.std.CropAbs(tex, W, H, left=3 * n, top=2 * n) for n in range(frames)]
    clip = core.std.Splice(parts)
    clip = core.std.AssumeFPS(clip, fpsnum=24000, fpsden=1001)
    return core.resize.Bicubic(clip, format=fmt)


def blend_pattern(clip, pal=False):
    m = core.std.Merge
    if not pal:
        a, b, c, d = (clip[i::4] for i in range(4))
        n = min(x.num_frames for x in (a, b, c, d))
        a, b, c, d = a[:n], b[:n], c[:n], d[:n]
        return core.std.Interleave([a, b, m(b, c), m(c, d), d])
    a, b, c, d, e = (clip[i::5] for i in range(5))
    n = min(x.num_frames for x in (a, b, c, d, e))
    a, b, c, d, e = a[:n], b[:n], c[:n], d[:n], e[:n]
    return core.std.Interleave([a, b, c, m(c, d), m(d, e), e])


def psnr(a, b, frames):
    """Average luma PSNR over the given frame numbers."""
    total = 0.0
    peak = (1 << a.format.bits_per_sample) - 1
    diff = core.std.PlaneStats(a, b, plane=0)
    for n in frames:
        f = diff.get_frame(n)
        mse_proxy = f.props["PlaneStatsDiff"] * peak  # mean absolute difference
        total += 99.0 if mse_proxy <= 1e-9 else 20 * math.log10(peak / mse_proxy)
    return total / len(frames)


def props(clip, key, frames):
    return [clip.get_frame(n).props[key] for n in frames]


def main():
    if len(sys.argv) > 1:
        core.std.LoadPlugin(os.path.abspath(sys.argv[1]))
    tmp = tempfile.mkdtemp(prefix="exblend_test_")

    src = film()
    clip = blend_pattern(src)
    check(clip.num_frames == FILM // 4 * 5, "test clip has %d frames" % clip.num_frames)

    # Detection: every blend pair after the start-up frames found at its position.
    stats = os.path.join(tmp, "film.stats")
    log = os.path.join(tmp, "film.log")
    out = core.exblend.ExBlend(clip, statsfile=stats, decimate=0, logfile=log)
    idx = props(out, "ExBlendIndex", range(clip.num_frames))
    expected = [1 if i % 5 == 2 else 2 if i % 5 == 3 else None for i in range(clip.num_frames)]
    missed = [i for i, e in enumerate(expected) if e and idx[i] != e and 5 <= i < clip.num_frames - 5]
    check(not missed, "all blend pairs found (missed %s)" % missed[:10])
    check(os.path.exists(stats), "stats file written")
    check(os.path.exists(log), "log file written")

    # Decimated output equals the film within unblend rounding, frame count and rate follow the cycle.
    dec = core.exblend.ExBlend(clip, statsfile=stats)
    check(dec.num_frames == clip.num_frames * 4 // 5, "decimated length %d" % dec.num_frames)
    check(dec.fps == clip.fps * 4 / 5, "decimated rate %s" % dec.fps)
    check(props(dec, "ExBlendSource", [0, 1, 2, 3, 4]) == [0, 1, 2, 4, 5], "decimated mapping drops index 2")
    p = psnr(dec, src[:dec.num_frames], range(8, dec.num_frames - 8))
    check(p > 45, "decimated output matches the film (%.1f dB)" % p)
    p0 = psnr(clip[:dec.num_frames], src[:dec.num_frames], range(8, dec.num_frames - 8))
    check(p > p0 + 6, "better than the blended input (%.1f dB)" % p0)
    fp = dec.get_frame(10).props
    check((fp["_DurationNum"], fp["_DurationDen"]) == (1001, 24000), "decimated frame durations %d/%d" % (
        fp["_DurationNum"], fp["_DurationDen"]))

    # Stats file reuse: same decisions without a scan; a different clip with the same file is rescanned.
    again = core.exblend.ExBlend(clip, statsfile=stats, decimate=0)
    check(props(again, "ExBlendFlags", range(clip.num_frames)) == props(out, "ExBlendFlags", range(clip.num_frames)),
          "stats file reused")
    with open(stats, "rb") as f:
        stats_bytes = f.read()
    other_stats = os.path.join(tmp, "other.stats")
    with open(other_stats, "wb") as f:
        f.write(stats_bytes)
    core.exblend.ExBlend(core.std.Invert(clip), statsfile=other_stats, decimate=0)
    with open(other_stats, "rb") as f:
        check(f.read() != stats_bytes, "changed clip rescanned")

    # Scan returns the source frames with pair metrics and writes the same stats file.
    stats2 = os.path.join(tmp, "scan.stats")
    scan = core.exblend.Scan(clip, statsfile=stats2)
    for n in reversed(range(scan.num_frames)):
        scan.get_frame(n)
    check(os.path.exists(stats2), "Scan writes the stats file after all frames")
    with open(stats, "rb") as a, open(stats2, "rb") as b:
        check(a.read() == b.read(), "Scan and ExBlend produce identical stats files")
    check(abs(scan.get_frame(12).props["ExBlendMD"] - out.get_frame(12).props["ExBlendMD"]) < 1e-12,
          "Scan metric props equal ExBlend props")

    # PAL pattern.
    pal = blend_pattern(src, pal=True)
    pout = core.exblend.ExBlend(pal, pal=1)
    check(pout.num_frames == pal.num_frames * 5 // 6, "PAL decimated length %d" % pout.num_frames)
    p = psnr(pout, src[:pout.num_frames], range(8, pout.num_frames - 8))
    check(p > 45, "PAL decimated output matches the film (%.1f dB)" % p)

    # Automatic film/PAL choice: same decisions as the matching fixed variant; no blend pattern stays film.
    for c, want, fixed in ((clip, 0, dec), (pal, 1, pout)):
        a = core.exblend.ExBlend(c, pal=-1)
        check(a.num_frames == fixed.num_frames and a.get_frame(10).props["ExBlendPal"] == want,
              "pal=-1 picks %s" % ("PAL" if want else "film"))
        check(props(a, "ExBlendFlags", range(0, a.num_frames, 7)) == props(fixed, "ExBlendFlags",
                                                                           range(0, a.num_frames, 7)),
              "pal=-1 decisions equal pal=%d" % want)
    autolog = os.path.join(tmp, "auto.log")
    a = core.exblend.ExBlend(src, pal=-1, logfile=autolog)
    with open(autolog) as f:
        text = f.read()
    check(a.get_frame(10).props["ExBlendPal"] == 0 and "undecided" in text, "pal=-1 without blends stays film")
    check("WARNING: neither film nor PAL" in text, "pal=-1 without blends warns")

    # Pattern score and warning with a fixed pal: clean pattern scores high, a clip without blends warns.
    check(dec.get_frame(10).props["ExBlendScore"] > 0.9, "film pattern score %.3f" % dec.get_frame(10).props["ExBlendScore"])
    nolog = os.path.join(tmp, "noblend.log")
    n = core.exblend.ExBlend(src, pal=0, logfile=nolog)
    with open(nolog) as f:
        text = f.read()
    check(n.get_frame(10).props["ExBlendScore"] < 0.5 and "WARNING: no clear film blend pattern" in text,
          "pal=0 without blends warns (score %.3f)" % n.get_frame(10).props["ExBlendScore"])
    with open(log) as f:
        check("WARNING" not in f.read(), "no warning on the film pattern")

    # High bit depth: same decisions for the same picture content; output close to the film.
    base_flags = props(out, "ExBlendFlags", range(clip.num_frames))
    for fmt in (vs.YUV420P10, vs.YUV420P16, vs.GRAY8, vs.YUV444P8):
        c = blend_pattern(film(fmt))
        o = core.exblend.ExBlend(c, decimate=0)
        f = props(o, "ExBlendFlags", range(c.num_frames))
        same = sum(1 for x, y in zip(f, base_flags) if (x & 3) == (y & 3))
        check(same >= c.num_frames - 2 if fmt in (vs.YUV420P10, vs.YUV420P16) else True,
              "%s decisions (%d of %d equal to 8 bit)" % (core.get_video_format(fmt).name, same, c.num_frames))
        d = core.exblend.ExBlend(c)
        p = psnr(d, film(fmt)[:d.num_frames], range(8, d.num_frames - 8))
        check(p > 45, "%s decimated output matches the film (%.1f dB)" % (core.get_video_format(fmt).name, p))

    # Output modes run and keep the frame count.
    for args in ({"decimate": 0, "disp": 0}, {"decimate": 0, "disp": 2}, {"decimate": 0, "disp": 3},
                 {"disp": 0, "decompix": 2}, {"disp": 3}, {"disp": 4, "decompix": 2}):
        o = core.exblend.ExBlend(clip, statsfile=stats, **args)
        o.get_frame(o.num_frames // 2)
        check(o.num_frames == (clip.num_frames if args.get("decimate") == 0 else dec.num_frames), "output %s" % args)

    # Detection clip.
    dclip = core.std.BoxBlur(clip, hradius=0, vradius=1, planes=[0])
    o = core.exblend.ExBlend(clip, dclip=dclip, compub=0)
    p = psnr(o, src[:o.num_frames], range(8, o.num_frames - 8))
    check(p > 45, "with dclip, compub=0 (%.1f dB)" % p)

    # Override file: force hints over a stretch, decisions follow.
    ovr = os.path.join(tmp, "override.txt")
    with open(ovr, "w") as f:
        f.write("# comment\n100=h10\n")
    o = core.exblend.ExBlend(clip, statsfile=stats, decimate=0, override=ovr)
    check(all(x == 3 for x in props(o, "ExBlendIndex", range(100, 110))), "override hints applied")
    with open(ovr, "w") as f:
        f.write("100=h2\n50\n")
    expect_error(lambda: core.exblend.ExBlend(clip, statsfile=stats, override=ovr), "override order checked",
                 "ascending")

    # Decimated frames file in the v1.04 format.
    decf = os.path.join(tmp, "dec.txt")
    core.exblend.ExBlend(clip, statsfile=stats, decfile=decf)
    with open(decf) as f:
        lines = [l for l in f if l.strip() and not l.startswith("#")]
    check(len(lines) >= clip.num_frames // 5 - 3, "decimated frames file lists the recovered frames (%d)" % len(lines))

    # Argument and format checks.
    expect_error(lambda: core.exblend.ExBlend(clip, pal=2), "pal range", "pal")
    expect_error(lambda: core.exblend.ExBlend(clip, disp=4, decimate=0), "disp=4 needs decimation", "disp=4")
    expect_error(lambda: core.exblend.ExBlend(clip, disp=2), "disp=2 without decimation", "disp=2")
    expect_error(lambda: core.exblend.ExBlend(core.resize.Point(clip, format=vs.RGB24)), "RGB rejected", "YUV or Gray")
    expect_error(lambda: core.exblend.ExBlend(core.resize.Point(clip, format=vs.YUV420PS)), "float rejected",
                 "integer")
    expect_error(lambda: core.exblend.ExBlend(clip, dclip=clip[:-1]), "dclip length", "dclip")
    expect_error(lambda: core.exblend.ExBlend(film(vs.YUV420P10)[:50], compat=1), "compat needs 8 bit", "compat")
    expect_error(lambda: core.exblend.ExBlend(clip[:5]), "too short", "7 frames")

    print("%d failures" % len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
