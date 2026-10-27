#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compares the VapourSynth and the AviSynth+ interface of the same ExBlend build (Windows).

Both read the same y4m files with BestSource; the stats files and the MD5 of every output frame must be equal.

Usage (VapourSynth Python): python compare_hosts.py --plugin ExBlend.dll --work DIR CLIP.y4m [DCLIP.y4m]
"""
import argparse
import hashlib
import os
import subprocess
import sys

import vapoursynth as vs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_original import AVS2YUV, BESTSOURCE, fwd, run_avs  # noqa: E402

core = vs.core
FAIL = []


def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        FAIL.append(msg)


def vs_hashes(clip):
    out = []
    for n in range(clip.num_frames):
        f = clip.get_frame(n)
        h = hashlib.md5()
        for p in range(f.format.num_planes):
            h.update(bytes(f[p]))
        out.append(h.hexdigest())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("clip")
    ap.add_argument("dclip", nargs="?")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    core.std.LoadPlugin(os.path.abspath(a.plugin))
    if not hasattr(core, "bs"):
        core.std.LoadPlugin(os.path.join(os.path.dirname(vs.__file__), "plugins2", "libbestsource.dll"))
    src = core.bs.VideoSource(a.clip)
    d = core.bs.VideoSource(a.dclip) if a.dclip else None
    cases = [{}, {"decimate": 0}, {"compat": 1}, {"compat": 1, "decimate": 0}, {"pal": 1}, {"pal": -1}, {"fullscan": 0},
             {"disp": 3, "decompix": 2}, {"decimate": 0, "disp": 2}, {"show": 1}]
    if d is not None:
        cases += [{"dclip": True, "compub": c} for c in (0, 1, 2)]
    vs_stats = os.path.join(a.work, "vs.stats")
    avs_stats = os.path.join(a.work, "avs.stats")
    for case in cases:
        kw = dict(case)
        use_d = kw.pop("dclip", False)
        tag = "_d" if use_d else ""
        vs_out = core.exblend.ExBlend(src, statsfile=vs_stats + tag, dclip=d if use_d else None, **kw)
        avs_args = ", ".join("%s=%s" % (k, ("true" if v else "false") if k in ("compat", "decimate", "fullscan", "show")
                                       else v) for k, v in kw.items())
        script = 'LoadPlugin("%s")\nLoadPlugin("%s")\nsrc = BSVideoSource("%s")\n' % (fwd(a.plugin), BESTSOURCE,
                                                                                     fwd(a.clip))
        if use_d:
            script += 'd = BSVideoSource("%s")\n' % fwd(a.dclip)
        script += 'ExBlend(src, statsfile="%s"%s%s)\n' % (fwd(avs_stats + tag), ", dclip=d" if use_d else "",
                                                       ", " + avs_args if avs_args else "")
        hv = vs_hashes(vs_out)
        ha = run_avs(os.path.join(a.work, "host_case.avs"), script)
        diff = [i for i in range(min(len(hv), len(ha))) if hv[i] != ha[i]]
        check(len(hv) == len(ha) and not diff, "%s: %d/%d frames, %d differ" % (case, len(hv), len(ha), len(diff)))
    for tag in ("", "_d") if d is not None else ("",):
        with open(vs_stats + tag, "rb") as x, open(avs_stats + tag, "rb") as y:
            check(x.read() == y.read(), "stats files identical%s" % tag)
    print("%d failures" % len(FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
