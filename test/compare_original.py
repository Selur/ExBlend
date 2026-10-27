#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compares the port (compat=true) with the original ExBlend v1.04 DLL inside AviSynth+.

Every clip is decoded once to y4m (ffmpeg), so both filters see identical frames. Checked per clip:
  - decision bytes: original mode=1 XBD (show=5 and show=0) against the port's log (fullscan=1 / 0)
  - the decimated frames file of mode 1 against decfile
  - MD5 of every output frame of mode 2 against the port, for a set of disp/decimate/compub/decompix cases

Usage: python compare_original.py --work DIR --original ExBlend_x64.dll --plugin ExBlend.dll [--full N] CLIP...
"""
import argparse
import hashlib
import os
import subprocess
import sys

AVS2YUV = r"F:/Hybrid/64bit/Avisynth/avs2yuv64.exe"
BESTSOURCE = r"F:/Hybrid/64bit/Avisynth/avisynthPlugins/BestSource.dll"


def fwd(p):
    return os.path.abspath(p).replace("\\", "/")


def run_avs(script_path, text, want_hashes=True):
    with open(script_path, "w", encoding="utf-8") as f:
        f.write(text)
    if not want_hashes:
        r = subprocess.run([AVS2YUV, script_path, "-o", "NUL"], capture_output=True, text=True)
        if r.returncode != 0 or "Error" in r.stderr:
            raise RuntimeError("%s: %s" % (script_path, r.stderr.strip()[-600:]))
        return None
    p = subprocess.Popen([AVS2YUV, "-nstdr", script_path, "-o", "-"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    hashes = read_y4m_hashes(p.stdout)
    err = p.stderr.read().decode(errors="replace")
    p.wait()
    if p.returncode != 0 or not hashes:
        raise RuntimeError("%s: %s" % (script_path, err.strip()[-600:]))
    return hashes


def read_y4m_hashes(stream):
    header = stream.readline()
    if not header.startswith(b"YUV4MPEG2"):
        return []
    fields = header.split()
    w = h = 0
    chroma = b"420"
    for t in fields[1:]:
        if t[:1] == b"W":
            w = int(t[1:])
        elif t[:1] == b"H":
            h = int(t[1:])
        elif t[:1] == b"C":
            chroma = t[1:]
    if chroma.startswith(b"420"):
        size = w * h * 3 // 2
    elif chroma.startswith(b"444"):
        size = w * h * 3
    elif chroma.startswith(b"422"):
        size = w * h * 2
    else:
        size = w * h
    out = []
    while True:
        line = stream.readline()
        if not line:
            break
        data = stream.read(size)
        if len(data) < size:
            break
        out.append(hashlib.md5(data).hexdigest())
    return out


def prepare(clip, work, frames):
    name = os.path.splitext(os.path.basename(clip))[0].replace(" ", "_")
    y4m = os.path.join(work, "clips", name + ".y4m")
    dy4m = os.path.join(work, "clips", name + "_d.y4m")
    os.makedirs(os.path.dirname(y4m), exist_ok=True)
    if not os.path.exists(y4m):
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", clip, "-map", "0:v:0", "-frames:v", str(frames),
                        "-pix_fmt", "yuv420p", "-f", "yuv4mpegpipe", y4m], check=True)
    if not os.path.exists(dy4m):
        # The detection clip of the forum recipe: vertical blur.
        s = 'LoadPlugin("%s")\nBSVideoSource("%s")\nBlur(0, 1.0)\n' % (BESTSOURCE, fwd(y4m))
        sp = os.path.join(work, "scripts", name + "_dclip.avs")
        os.makedirs(os.path.dirname(sp), exist_ok=True)
        with open(sp, "w", encoding="utf-8") as f:
            f.write(s)
        subprocess.run([AVS2YUV, "-nstdr", sp, "-o", dy4m], check=True, capture_output=True)
    return name, y4m, dy4m


def source_lines(y4m, dy4m, use_d):
    s = 'LoadPlugin("%s")\nsrc = BSVideoSource("%s")\n' % (BESTSOURCE, fwd(y4m))
    if use_d:
        s += 'd = BSVideoSource("%s")\n' % fwd(dy4m)
    return s


def read_xbd(path):
    with open(path, "rb") as f:
        data = f.read()
    return list(data[24:])


def read_log_flags(path):
    flags = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 2 and parts[1].startswith("0x"):
                flags.append(int(parts[1], 16))
    return flags


def decfile_lines(path):
    with open(path, encoding="latin-1") as f:
        return [l.rstrip("\r\n") for l in f]


FAIL = []


def check(cond, msg):
    if cond:
        print("ok   " + msg)
    else:
        print("FAIL " + msg)
        FAIL.append(msg)


def output_cases(full, use_d):
    """(label, original args, port args) of mode 2 output checks."""
    cases = [("dec disp1", "", ""), ("nodec disp1", "decimate=false", "decimate=false")]
    if full:
        for disp in (0, 2, 3):
            cases.append(("nodec disp%d" % disp, "decimate=false, disp=%d" % disp, "decimate=false, disp=%d" % disp))
        for disp in (0, 3, 4):
            for ix in (1, 2):
                a = "disp=%d, decompix=%d" % (disp, ix)
                cases.append(("dec " + a, a, a))
    if use_d:
        out = []
        for label, o, p in cases:
            for cu in ((0, 1, 2) if full else (2,)):
                extra = "CompUB=%d" % cu
                out.append(("%s compub%d" % (label, cu), ", ".join(x for x in (o, extra) if x),
                            ", ".join(x for x in (p, "compub=%d" % cu) if x)))
        return out
    return cases


def compare_clip(clip, args, full):
    name, y4m, dy4m = prepare(clip, args.work, args.frames)
    sdir = os.path.join(args.work, "scripts")
    rdir = os.path.join(args.work, "ref")
    odir = os.path.join(args.work, "out")
    os.makedirs(rdir, exist_ok=True)
    os.makedirs(odir, exist_ok=True)
    orig = 'LoadPlugin("%s")\n' % fwd(args.original)
    port = 'LoadPlugin("%s")\n' % fwd(args.plugin)
    for use_d in (False, True):
        tag = name + ("_d" if use_d else "")
        dpart = ", dclip=d" if use_d else ""
        stats = fwd(os.path.join(odir, tag + ".stats"))
        for show, fullscan in ((5, 1), (0, 0)):
            exb = fwd(os.path.join(rdir, "%s_s%d" % (tag, show)))
            s = orig + source_lines(y4m, dy4m, use_d) + 'ExBlend(src, mode=1, show=%d, exbfile="%s"%s)\n' % (show, exb, dpart)
            run_avs(os.path.join(sdir, tag + "_orig_m1_s%d.avs" % show), s, want_hashes=False)
            log = fwd(os.path.join(odir, "%s_f%d.log" % (tag, fullscan)))
            dec = fwd(os.path.join(odir, "%s_f%d_Decimated.TXT" % (tag, fullscan)))
            s = port + source_lines(y4m, dy4m, use_d) + (
                'ExBlend(src, statsfile="%s", compat=true, fullscan=%s, logfile="%s", decfile="%s"%s)\n'
                % (stats, "true" if fullscan else "false", log, dec, dpart))
            run_avs(os.path.join(sdir, tag + "_port_f%d.avs" % fullscan), s, want_hashes=False)
            xbd = read_xbd(exb + ".XBD")
            flags = read_log_flags(log)
            diff = [i for i in range(min(len(xbd), len(flags))) if xbd[i] != flags[i]]
            check(len(xbd) == len(flags) and not diff,
                  "%s show=%d/fullscan=%d decisions (%d frames, %d differ%s)" % (
                      tag, show, fullscan, len(xbd), len(diff),
                      "" if not diff else ", first %d: 0x%02X vs 0x%02X" % (diff[0], xbd[diff[0]], flags[diff[0]])))
            check(decfile_lines(exb + "_Decimated.TXT") == decfile_lines(dec),
                  "%s show=%d/fullscan=%d decimated frames file" % (tag, show, fullscan))
            if show == 5:
                ix1 = sum(1 for f in xbd if f & 3 == 1)
                print("     %s: %d blend pairs in %d frames (ratio %.2f)" % (
                    tag, ix1, len(xbd), (len(xbd) - 2 * ix1) / ix1 if ix1 else 0.0))
        exb = fwd(os.path.join(rdir, "%s_s5" % tag))
        for label, oargs, pargs in output_cases(full, use_d):
            o = orig + source_lines(y4m, dy4m, use_d) + 'ExBlend(src, mode=2, exbfile="%s"%s%s)\n' % (
                exb, dpart, ", " + oargs if oargs else "")
            p = port + source_lines(y4m, dy4m, use_d) + 'ExBlend(src, statsfile="%s", compat=true%s%s)\n' % (
                stats, dpart, ", " + pargs if pargs else "")
            safe = label.replace(" ", "_")
            ho = run_avs(os.path.join(sdir, "%s_orig_%s.avs" % (tag, safe)), o)
            hp = run_avs(os.path.join(sdir, "%s_port_%s.avs" % (tag, safe)), p)
            diff = [i for i in range(min(len(ho), len(hp))) if ho[i] != hp[i]]
            check(len(ho) == len(hp) and not diff, "%s %s output (%d/%d frames, %d differ%s)" % (
                tag, label, len(ho), len(hp), len(diff), "" if not diff else ", first %d" % diff[0]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--original", required=True)
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--full", type=int, default=1, help="number of clips (from the start) with all output cases")
    ap.add_argument("clips", nargs="+")
    args = ap.parse_args()
    for i, c in enumerate(args.clips):
        print("== " + c)
        try:
            compare_clip(c, args, i < args.full)
        except Exception as e:  # noqa: BLE001 - report and continue with the next clip
            FAIL.append("%s: %s" % (c, e))
            print("FAIL %s: %s" % (c, e))
    print("%d failures" % len(FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
