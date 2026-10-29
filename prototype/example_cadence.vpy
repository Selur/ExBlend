# Compare: original | AnimDeblend mode="cadence" on a VHS anime with a recurring blend. Open in vsViewer or render with
#   vspipe -c y4m example_cadence.vpy out.y4m
import os
import sys

import vapoursynth as vs

core = vs.core
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anim_deblend  # noqa: E402

if not hasattr(core, "bs"):
    core.std.LoadPlugin(os.path.join(os.path.dirname(vs.__file__), "plugins2", "libbestsource.dll"))

clip = core.bs.VideoSource(r"G:/clips/blends/Blended.avi")
# show=True writes 'blend a-b f=' on every replaced frame; switch: smaller accepts shorter cadence stretches
cadence = anim_deblend.AnimDeblend(clip, mode="cadence", switch=1.5, show=True)


def label(c, text):
    return core.text.Text(c, text, alignment=8) if hasattr(core, "text") else c


core.std.StackHorizontal([label(clip, "original"), label(cadence, "AnimDeblend cadence"),label(core.std.MakeDiff(clip, cadence), "Diff")]).set_output()
