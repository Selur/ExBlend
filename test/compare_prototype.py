#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""exblend.AnimDeblend against prototype/anim_deblend.py: per-frame decisions with the same blocks and noise estimate.

Usage (Hybrid's VapourSynth Python, Windows): python -I test/compare_prototype.py build/ExBlend.dll [clip ...]
Default clips are from G:\\clips\\blends and G:\\TestClips&Co (first 6000 / 3000 frames). Differences in borderline
frames come from rounding (double sums here, float32 in numpy); anything more is a porting error.
"""
import os
import sys
import time

import vapoursynth as vs

core = vs.core
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "prototype"))
import anim_deblend as proto  # noqa: E402

BLOCK, OVERLAP = 512, 128
CLIPS = [
    ("G:/clips/blends/Blended.avi", None),
    ("G:/clips/blends/DIExbUgWVySLBCsXEbrm.avi", 6000),
    ("G:/TestClips&Co/files/MPEG-4 H.264/[Vivid-Asenshi] Violet Evergarden - 01 [B58CEB69] (1)-001.mkv", 3000),
]


def prototype(c):
    n = c.num_frames
    block = BLOCK if BLOCK < n else n
    p = dict(maxgap=4, rho=0.2, kappa=3.0, width=160, minweight=0.1, mode="both", switch=1.5, fill=True,
             var=proto._noise_var_sampled(c, 160) if block < n else proto._noise_var(c, 160))
    blends, ghosts = {}, {}
    for s in range(0, n, block):
        e = min(n, s + block)
        found = proto._analyse_part(c, max(0, s - OVERLAP), min(n, e + OVERLAP), (s, e), p)
        blends.update(found)
        ghosts.update({k: v for k, v in proto._ghost_part(c, (s, e)).items() if k not in found})
    return blends, ghosts


def plugin(c):
    out = core.exblend.AnimDeblend(c, mode="both", rho=0.2, unghost=1, block=BLOCK, overlap=OVERLAP)
    blends, ghosts = {}, {}
    for k, f in enumerate(out.frames()):
        pr = f.props
        if pr["AnimBlend"]:
            blends[k] = (pr["AnimBlendFrom"], pr["AnimBlendTo"], pr["AnimBlendWeight"])
        if pr["AnimGhost"]:
            ghosts[k] = (pr["AnimGhostFrom"], pr["AnimGhostWeight"])
    return blends, ghosts


def main():
    core.std.LoadPlugin(sys.argv[1])
    if not hasattr(core, "bs"):
        core.std.LoadPlugin(os.path.join(os.path.dirname(vs.__file__), "plugins2", "libbestsource.dll"))
    clips = [(p, None) for p in sys.argv[2:]] or CLIPS
    for path, count in clips:
        c = core.bs.VideoSource(path)
        if count:
            c = c[:count]
        t = time.time()
        pb, pg = prototype(c)
        tp = time.time() - t
        t = time.time()
        cb, cg = plugin(c)
        tc = time.time() - t
        db = sorted(k for k in set(pb) | set(cb) if k not in pb or k not in cb or tuple(pb[k][:2]) != tuple(cb[k][:2])
                    or (pb[k][2] < 0.5) != (cb[k][2] < 0.5))
        dg = sorted(k for k in set(pg) | set(cg) if k not in pg or k not in cg or pg[k][0] != cg[k][0])
        print("%s: prototype %d blends %d ghosts (%.0f s), plugin %d blends %d ghosts (%.0f s); differing blends %d %s,"
              " ghosts %d %s" % (os.path.basename(path), len(pb), len(pg), tp, len(cb), len(cg), tc, len(db), db[:15],
                                 len(dg), dg[:10]), flush=True)


if __name__ == "__main__":
    main()
