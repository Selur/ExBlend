# Compare: original | AnimDeblend mode="both" | mode="both" + unghost on a blend-converted cartoon with fast pans.
# Open in vsViewer or render with  vspipe -c y4m example_unghost.vpy out.y4m
import os
import sys

import vapoursynth as vs

core = vs.core
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anim_deblend  # noqa: E402

if not hasattr(core, "bs"):
    core.std.LoadPlugin(os.path.join(os.path.dirname(vs.__file__), "plugins2", "libbestsource.dll"))

source = r"G:/clips/blends/DIExbUgWVySLBCsXEbrm.avi"
clip = core.bs.VideoSource(source)
# the analysis runs once per clip and setting; later loads read it from this file
cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.path.basename(source) + ".animdeblend.json")
both = anim_deblend.AnimDeblend(clip, mode="both", rho=0.2, show=True, cache=cache)
# unghost: frames with a faint copy of a neighbour are unblended instead of replaced ('ghost n f=' on the frame)
unghost = anim_deblend.AnimDeblend(clip, mode="both", rho=0.2, unghost=True, show=True, cache=cache)


def label(c, text):
    return core.text.Text(c, text, alignment=8) if hasattr(core, "text") else c


core.std.StackHorizontal([label(clip, "original"), label(both, "both"), label(unghost, "both + unghost")]).set_output()
