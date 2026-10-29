# Compare: original | ExBlend | AnimDeblend on a blend-converted cartoon. Open in vsViewer or render with
#   vspipe -c y4m example_blended.vpy out.y4m
import os
import sys

import vapoursynth as vs

core = vs.core
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anim_deblend  # noqa: E402

if not hasattr(core, "bs"):
    core.std.LoadPlugin(os.path.join(os.path.dirname(vs.__file__), "plugins2", "libbestsource.dll"))
if not hasattr(core, "exblend"):
    core.std.LoadPlugin(r"F:/Hybrid/64bit/vsfilters/FrameFilter/ExBlend/ExBlend.dll")

clip = core.bs.VideoSource(r"G:/clips/blends/DIExbUgWVySLBCsXEbrm.avi")

# ExBlend without decimation, so all three columns show the same frame numbers; pal=-1 picks film/PAL itself
# (show=1 marks recovered frames with a block in the top left corner)
exblend = core.exblend.ExBlend(clip, pal=-1, decimate=0, show=1)
anim = anim_deblend.AnimDeblend(clip, strength=1, show=True, rho=0.2, minweight=0.05)


def label(c, text):
    return core.text.Text(c, text, alignment=8) if hasattr(core, "text") else c


core.std.StackHorizontal([label(clip, "original"), label(exblend, "ExBlend"), label(anim, "AnimDeblend")]).set_output()
