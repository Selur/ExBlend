#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Functional tests for exblend.AnimDeblend (VapourSynth API 4).

Usage: python test/test_animdeblend.py [path/to/ExBlend plugin]

Without an argument the plugin is expected to be autoloaded (e.g. from the installed wheel).
The "drawings" are cartoon-like: flat blocks with hard edges (a small random image enlarged with Point), panned by
large steps, so blends and ghosts are known exactly.
"""
import math
import random
import sys

import vapoursynth as vs

core = vs.core
FAILURES = []
W, H, DRAWINGS = 320, 240, 60
STILL = 16  # held frames in front of each clip: the noise estimate needs holds, as real animation has


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


def sheet():
    """Blocky random image (YUV444P8), 16 px blocks, large enough for all pan positions."""
    tw, th = 132, 40
    rnd = random.Random(4321)
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
    return core.resize.Point(tiny, tw * 16, th * 16)


def drawings(count=DRAWINGS):
    """Distinct drawings: the sheet panned 29 px right and 5 px down per drawing."""
    s = sheet()
    parts = [core.std.CropAbs(s, W, H, left=29 * i, top=5 * i) for i in range(count)]
    clip = core.std.Splice(parts)
    clip = core.std.AssumeFPS(clip, fpsnum=25, fpsden=1)
    return core.resize.Bicubic(clip, format=vs.YUV420P8)


def with_still(clip):
    return core.std.Loop(clip[0], STILL) + clip


def props(clip, key, frames):
    return [clip.get_frame(n).props.get(key, 0) for n in frames]


def psnr(a, b):
    diff = core.std.Expr([a, b], "x y - abs")
    mse = 0.0
    for f in core.std.PlaneStats(core.std.Expr([diff], "x x *")).frames():
        mse += f.props["PlaneStatsAverage"] * 255.0
    mse /= a.num_frames
    return 99.0 if mse < 1e-9 else 10 * math.log10(255.0 ** 2 / mse)


def identical(a, b):
    for f in core.std.PlaneStats(a, b).frames():
        if f.props["PlaneStatsDiff"] != 0:
            return False
    return True


def main():
    if len(sys.argv) > 1:
        core.std.LoadPlugin(sys.argv[1])
    d = drawings()

    # Clean animation on twos: every drawing held twice, nothing to do.
    held = core.std.Interleave([d, d])
    o = core.exblend.AnimDeblend(held, mode="both")
    hits = sum(props(o, "AnimBlend", range(o.num_frames)))
    check(hits == 0, "held drawings: no blend found (%d)" % hits)
    check(identical(o, held), "held drawings: output unchanged")

    # Blend conversion: drawing, 30 % mix towards the next drawing, next drawing, ...
    mixes = core.std.Merge(d[:-1], d[1:], weight=0.3)
    blended = with_still(core.std.Interleave([d[:-1], mixes]))
    n = blended.num_frames
    mix_frames = range(STILL + 3, n - 3, 2)
    for mode in ("anchor", "cadence", "both"):
        o = core.exblend.AnimDeblend(blended, mode=mode)
        flags = props(o, "AnimBlend", range(n))
        odd = [flags[k] for k in mix_frames]
        even = [flags[k] for k in range(STILL + 2, n - 2, 2)]
        check(all(odd) and not any(even), "%s: every mix found, no drawing touched (%d of %d, %d false)"
              % (mode, sum(odd), len(odd), sum(even)))
    o = core.exblend.AnimDeblend(blended, mode="both")
    fixed = core.std.Splice([o[k] for k in mix_frames])
    want = core.std.Splice([d[(k - STILL) // 2] for k in mix_frames])
    check(identical(fixed, want), "a mix is replaced by the drawing it mostly shows")
    k = STILL + 11
    w = props(o, "AnimBlendWeight", [k])[0]
    check(abs(w - 0.3) < 0.02, "weight of the mix (%.3f)" % w)
    check(props(o, "AnimBlendFrom", [k]) == [k - 1] and props(o, "AnimBlendTo", [k]) == [k + 1], "anchors of the mix")
    half = core.exblend.AnimDeblend(blended, mode="both", strength=0.5)
    check(psnr(half[k], core.std.Merge(blended[k], d[5], 0.5)) > 45, "strength 0.5 moves half way")
    check(identical(core.exblend.AnimDeblend(blended, mode="both", strength=0.0), blended), "strength 0: unchanged")

    # Fast pan: every third frame carries 20 % of the previous drawing at that drawing's own position.
    frames = []
    for k in range(DRAWINGS):
        frames.append(core.std.Merge(d[k], d[k - 1], 0.2) if k % 3 == 1 and k > 0 else d[k])
    ghosted = with_still(core.std.Splice(frames))
    o = core.exblend.AnimDeblend(ghosted, mode="both", unghost=1)
    targets = [STILL + k for k in range(1, DRAWINGS - 2) if k % 3 == 1]
    flags = props(o, "AnimGhost", range(ghosted.num_frames))
    found = [k for k in targets if flags[k]]
    false = [k for k in range(2, ghosted.num_frames - 2) if flags[k] and k not in targets]
    check(len(found) == len(targets) and not false, "ghosts found (%d of %d, %d false)" % (len(found), len(targets),
                                                                                          len(false)))
    check(all(p == k - 1 for p, k in zip(props(o, "AnimGhostFrom", found), found)), "ghost source is the previous frame")
    p_in = psnr(core.std.Splice([ghosted[k] for k in targets]), core.std.Splice([d[k - STILL] for k in targets]))
    p_out = psnr(core.std.Splice([o[k] for k in targets]), core.std.Splice([d[k - STILL] for k in targets]))
    check(p_out > 40 and p_out > p_in + 15, "unghosted frames close to the drawing (%.1f -> %.1f dB)" % (p_in, p_out))
    plain = core.exblend.AnimDeblend(ghosted, mode="both")
    check(sum(props(plain, "AnimGhost", range(ghosted.num_frames))) == 0, "unghost off: no ghost treated")

    # Blocks give the same result as one analysis of the whole clip.
    a = props(core.exblend.AnimDeblend(blended, mode="both", block=16, overlap=16), "AnimBlend", range(n))
    b = props(core.exblend.AnimDeblend(blended, mode="both", block=0), "AnimBlend", range(n))
    check(a == b, "block=16 equals the whole-clip analysis")

    # Formats.
    for fmt in (vs.YUV420P10, vs.YUV444P16, vs.GRAY8, vs.YUV420PS):
        c = core.resize.Bicubic(blended, format=fmt)
        flags = props(core.exblend.AnimDeblend(c, mode="both"), "AnimBlend", mix_frames)
        check(all(flags), "%s: mixes found" % c.format.name)

    # Argument and format checks.
    expect_error(lambda: core.exblend.AnimDeblend(blended, mode="film"), "mode checked", "mode")
    expect_error(lambda: core.exblend.AnimDeblend(blended, strength=1.5), "strength range", "strength")
    expect_error(lambda: core.exblend.AnimDeblend(blended, minweight=0.5), "minweight range", "minweight")
    expect_error(lambda: core.exblend.AnimDeblend(blended, maxgap=1), "maxgap range", "maxgap")
    expect_error(lambda: core.exblend.AnimDeblend(core.resize.Point(blended, format=vs.RGB24, matrix_s="709")),
                 "RGB rejected", "YUV or Gray")
    expect_error(lambda: core.exblend.AnimDeblend(blended[:2]), "too short", "3 frames")

    print("%d failures" % len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
