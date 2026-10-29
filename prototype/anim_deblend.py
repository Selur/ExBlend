# SPDX-License-Identifier: GPL-2.0-or-later
"""Prototype: remove blends from animation without knowing a frame rate (ExBlend stage 6, "anchor / time warp" mode).

Every frame is either an anchor (a clean drawing) or a blend of the anchors around it,
O(k) = (1-f) O(a) + f O(b), a < k < b, with f rising from a to b. A dynamic programme over the clip picks the
anchors; a blend frame is then pulled towards the drawing it mostly shows: out = O + strength * (O(dominant) - O).

Meant for cartoons and anime (drawings with holds); flowing live action motion is easily mistaken for blends
(about a third of all frames of clean live action get flagged). There is no automatic switch: the share of held
frames does not separate animation from live action reliably, so use it on animation only.

    import anim_deblend
    clip = anim_deblend.AnimDeblend(clip, strength=1.0)

mode="cadence" looks for blends that recur with a fixed period (2-8 frames) whose phase may change anywhere in the
clip (path search over period/phase states); it catches blends the anchor model rejects, e.g. on VHS material where a
blend is only roughly a mix of its neighbours. mode="both" combines both detections.
unghost=True also removes faint copies of a neighbour that sit on an otherwise new drawing (fast pans where every
frame is blended) by unblending instead of replacing.

The analysis runs per block of frames when a frame of the block is first requested; cache= keeps it in a file.
Frame properties: AnimBlend (0/1), AnimBlendWeight (f), AnimBlendFrom/To (anchor frame numbers).
"""
import hashlib
import json
import threading

import numpy as np
import vapoursynth as vs

core = vs.core


class _Frames:
    """Analysis-size luma frames held in memory; the analyses accept it in place of a clip (read once, used often)."""

    def __init__(self, frames):
        self.frames = frames
        self.num_frames = len(frames)


def _analysis_frames(clip, width):
    """Generator over small float32 luma frames of the whole clip."""
    if isinstance(clip, _Frames):
        yield from clip.frames
        return
    height = max(16, round(clip.height * width / clip.width / 2) * 2)
    small = core.resize.Bilinear(clip, width, height, format=vs.GRAYS)
    for f in small.frames():
        yield np.asarray(f[0], dtype=np.float32) * 255.0


def _blend_fit(o, a, b):
    d = b - a
    dd = float((d * d).sum())
    if dd < 1e-6 * d.size:
        return 0.0, float(((o - a) ** 2).mean())
    f = min(max(float(((o - a) * d).sum()) / dd, 0.0), 1.0)
    r = o - (a + f * d)
    return f, float((r * r).mean())


def _check_minweight(minweight):
    if not 0.01 <= minweight <= 0.49:
        raise ValueError("AnimDeblend: minweight must be between 0.01 and 0.49")


def _noise_var(clip, width):
    """Temporal noise: half the 10 % quantile of frame-to-frame differences (holds and calm stretches)."""
    steps, prev = [], None
    for f in _analysis_frames(clip, width):
        if prev is not None:
            steps.append(float(((f - prev) ** 2).mean()))
        prev = f
    steps.sort()
    return 0.5 * steps[len(steps) // 10] if steps else 0.0


def _noise_var_sampled(clip, width, count=1000):
    """_noise_var from evenly spaced frame pairs: close to the full estimate without reading the whole clip."""
    height = max(16, round(clip.height * width / clip.width / 2) * 2)
    small = core.resize.Bilinear(clip, width, height, format=vs.GRAYS)
    steps = []
    for k in np.linspace(1, clip.num_frames - 1, min(count, clip.num_frames - 1)).astype(int):
        a = np.asarray(small.get_frame(int(k))[0], dtype=np.float32)
        b = np.asarray(small.get_frame(int(k) - 1)[0], dtype=np.float32)
        steps.append(float(((a - b) ** 2).mean()) * 255.0 * 255.0)
    steps.sort()
    return 0.5 * steps[len(steps) // 10] if steps else 0.0


def analyse(clip, maxgap=4, rho=0.1, kappa=3.0, width=160, minweight=0.1, var=None):
    """Blend frames of the clip: {frame: (anchor_before, anchor_after, weight)}; var: noise from _noise_var."""
    _check_minweight(minweight)
    if var is None:
        var = _noise_var(clip, width)
    tau = kappa * max(var, 0.25)
    # pass 2: streaming dynamic programme, an anchor costs tau, a blend frame its residual
    n = clip.num_frames
    inf = float("inf")
    dp, back, weights = [inf] * n, [-1] * n, [None] * n
    window = {}
    for b, fb in enumerate(_analysis_frames(clip, width)):
        window[b] = fb
        window.pop(b - maxgap - 1, None)
        if b == 0:
            dp[0] = tau
            continue
        for a in range(max(0, b - maxgap), b):
            if dp[a] == inf:
                continue
            cost, fs, ok = 0.0, [], True
            for k in range(a + 1, b):
                f, r = _blend_fit(window[k], window[a], fb)
                copy = min(float(((window[k] - window[a]) ** 2).mean()), float(((window[k] - fb) ** 2).mean()))
                if f < minweight or f > 1.0 - minweight or (fs and f < fs[-1]) or r - 1.5 * var > rho * max(copy - 2.0 * var, 0.0):
                    ok = False
                    break
                fs.append(f)
                cost += r
            if ok and dp[a] + cost + tau < dp[b]:
                dp[b], back[b], weights[b] = dp[a] + cost + tau, a, fs
    blends = {}
    b = n - 1
    while b > 0:
        a = back[b]
        for i, k in enumerate(range(a + 1, b)):
            blends[k] = (a, b, weights[b][i])
        b = a
    return blends


_CADENCE_PAIRS = ((-1, 1), (-1, 2), (-2, 1))
_CADENCE_PERIODS = range(2, 9)
_CADENCE_THETA = 0.6  # blend ratio below which a frame on the cadence pays off
_CADENCE_ACCEPT = 0.8  # blend ratio a frame on the cadence must reach to be treated
_CADENCE_CLEAN = 0.75  # blend ratio the clean frames of a two-blend cadence must exceed
_ISOLATED_Q = 0.5  # off any cadence: a single blend ...
_ISOLATED_NEIGHBOUR = 0.45  # ... whose neighbours do not look like blends (not a dissolve) ...
_ISOLATED_COPY = 8.0  # ... between different drawings (not flickering lights) ...
_ISOLATED_SKEW = 0.1  # ... that mostly shows one of them (smooth motion sits near f = 0.5)


def analyse_cadence(clip, switch=1.5, width=160, minweight=0.1, var=None):
    """Blend frames on a local cadence: {frame: (anchor_before, anchor_after, weight)}.

    Per frame the best anchor pair among (k-1, k+1), (k-1, k+2), (k-2, k+1) gives the blend ratio
    q = (resid - 1.5 var) / (copy - 2 var); a Viterbi path over 'none' and (period, phase) states then picks where a
    cadence holds. Off a cadence, an isolated clear blend between clearly different drawings is taken as well.
    switch: cost of entering, leaving or changing a cadence (smaller = shorter stretches accepted).
    """
    _check_minweight(minweight)
    if var is None:
        var = _noise_var(clip, width)
    n = clip.num_frames
    q = np.full(n, np.inf)
    copies = np.zeros(n)
    pick = [None] * n
    window = {}
    for i, fr in enumerate(_analysis_frames(clip, width)):
        window[i] = fr
        window.pop(i - 5, None)
        k = i - 2  # every pair of frame k is complete once frame k+2 is in
        if k >= 2:
            _cadence_measure(window, k, var, minweight, q, copies, pick)
    for k in range(max(2, n - 2), n):  # pairs that reach past the clip end are skipped
        _cadence_measure(window, k, var, minweight, q, copies, pick)
    blends = {}
    for k, state in enumerate(_cadence_path(q, switch)):
        if _cadence_on(k, state) and q[k] <= _CADENCE_ACCEPT:
            blends[k] = pick[k]
        elif (0 < k < n - 1 and q[k] <= _ISOLATED_Q and q[k - 1] > _ISOLATED_NEIGHBOUR
              and q[k + 1] > _ISOLATED_NEIGHBOUR and copies[k] - 2.0 * var >= _ISOLATED_COPY
              and abs(pick[k][2] - 0.5) >= _ISOLATED_SKEW):
            blends[k] = pick[k]
    return blends


def _cadence_measure(window, k, var, minweight, q, copies, pick):
    o = window[k]
    for pa, pb in _CADENCE_PAIRS:
        if k + pa not in window or k + pb not in window:
            continue
        a, b = window[k + pa], window[k + pb]
        f, r = _blend_fit(o, a, b)
        copy = min(float(((o - a) ** 2).mean()), float(((o - b) ** 2).mean()))
        if f < minweight or f > 1.0 - minweight or copy - 2.0 * var <= 3.0 * max(var, 0.25):
            continue
        ratio = (r - 1.5 * var) / (copy - 2.0 * var)
        if ratio < q[k]:
            q[k], copies[k], pick[k] = ratio, copy, (k + pa, k + pb, f)


def _cadence_on(k, state):
    period, phase, length = state
    return period > 0 and (k - phase) % period < length


def _cadence_path(q, switch):
    """Cheapest state per frame; (0, 0, 0) is 'no cadence', (P, p, L) blends on L consecutive frames per period P
    starting at phase p (e.g. two blends and one clean frame in every three)."""
    # at most two blends per period: with longer groups the path could hop phases to dodge the few clean frames
    st = [(0, 0, 0)] + [(period, phase, length) for period in _CADENCE_PERIODS for phase in range(period)
                        for length in range(1, min(period, 3))]
    n, m = len(q), len(st)
    qq = np.where(np.isfinite(q), np.minimum(q, 1.5), 1.5)
    idx = np.arange(n)
    cost = np.zeros((n, m))
    for j, (period, phase, length) in enumerate(st[1:], 1):
        on = (idx - phase) % period < length
        # on the cadence a clear blend pays off; off the cadence a frame that looks like a blend argues against it
        off = np.maximum(0.0, _CADENCE_THETA - qq)
        if length > 1:  # two blends per period need clean frames that are clearly no mix (smooth motion is);
            # a frame without a measurement (near hold) is no such proof
            measured = np.where(np.isfinite(q), qq, _CADENCE_THETA)
            off = np.maximum(0.0, _CADENCE_CLEAN - measured) * 3.0 * length / (period - length)
        cost[:, j] = np.where(on, qq - _CADENCE_THETA, off)
    acc = cost[0].copy()
    back = np.zeros((n, m), np.int32)
    for k in range(1, n):
        best = int(np.argmin(acc))
        use_switch = acc[best] + switch < acc
        back[k] = np.where(use_switch, best, np.arange(m))
        acc = np.where(use_switch, acc[best] + switch, acc) + cost[k]
    path = np.zeros(n, np.int32)
    path[-1] = int(np.argmin(acc))
    for k in range(n - 1, 0, -1):
        path[k - 1] = back[k, path[k]]
    return [st[j] for j in path]


_FILL_PERIODS = range(2, 5)
_FILL_MAXHOLE = 3  # missing frames on a run's lattice that are filled from context
_FILL_SIDE = 2  # detections needed on each side of a gap ...
_FILL_END = 3  # ... and before a run end that is extended by one frame
_FILL_RATIO = 0.75  # a gap frame's mix residual against copying, noise-corrected ...
_FILL_NOISE = 4.0  # ... or a residual within this many noise variances (a near-copy of a neighbour)


def _on_run(k, blends, n):
    """Period of the regular run of blends (2-4) whose gap k fills or whose end k extends, or 0."""
    for period in _FILL_PERIODS:
        def count(start, step):
            c, j = 0, start
            while 0 <= j < n and j in blends:
                c, j = c + 1, j + step
            return c

        holes_before = 0
        while holes_before < _FILL_MAXHOLE - 1 and 0 <= k - period * (holes_before + 1) not in blends:
            holes_before += 1
        if k - period in blends:
            holes_before = 0
        holes_after = 0
        while holes_after < _FILL_MAXHOLE - 1 and k + period * (holes_after + 1) < n and \
                k + period * (holes_after + 1) not in blends:
            holes_after += 1
        if k + period in blends:
            holes_after = 0
        before = count(k - period * (holes_before + 1), -period)
        after = count(k + period * (holes_after + 1), period)
        if before >= _FILL_SIDE and after >= _FILL_SIDE and holes_before + holes_after + 1 <= _FILL_MAXHOLE:
            return period
        if holes_before == 0 and before >= _FILL_END and k + period not in blends:
            return period
        if holes_after == 0 and after >= _FILL_END and k - period not in blends:
            return period
    return 0


def fill_gaps(clip, blends, width=160, var=None):
    """Blends with the gaps of regular runs filled: frames inside a run (or one past its end) that were missed.

    A blend next to a gap that took the gap frame as its anchor and has no partner on the run's lattice is dropped
    (it sits off the run; one that belongs to a run of its own, e.g. after a phase change, stays). A candidate must
    still be a mix of its two neighbours (clearly better than copying either one, or within noise of a copy); f comes
    from its own pair fit. Run frames whose anchor turned out to be a blend are fitted again against their direct
    neighbours.
    """
    if isinstance(clip, _Frames):
        def frame(i):
            return clip.frames[i]
    else:
        small = core.resize.Bilinear(clip, width, max(16, round(clip.height * width / clip.width / 2) * 2),
                                     format=vs.GRAYS)

        def frame(i):
            return np.asarray(small.get_frame(i)[0], dtype=np.float32) * 255.0
    n = clip.num_frames
    out = dict(blends)
    var = max(_noise_var(clip, width) if var is None else var, 0.25)

    def pair_fit(k):
        """(f, residual / copy) of k against its direct neighbours, or None if no usable mix."""
        o, a, b = (frame(i) for i in (k, k - 1, k + 1))
        f, r = _blend_fit(o, a, b)
        copy = min(float(((o - a) ** 2).mean()), float(((o - b) ** 2).mean()))
        if not 0.02 <= f <= 0.98 or r >= copy:
            return None
        if r - 1.5 * var > _FILL_RATIO * max(copy - 2.0 * var, 0.0) and r > _FILL_NOISE * var:
            return None
        return f, r / copy

    for k in range(1, n - 1):
        period = _on_run(k, blends, n) if k not in blends else 0
        if not period:
            continue
        fit = pair_fit(k)
        if fit is None:
            continue
        for j in (k - 1, k + 1):
            if j in out and k in out[j][:2] and j - period not in blends and j + period not in blends:
                other = pair_fit(j) if 0 < j < n - 1 else None
                if other is None or fit[1] < other[1]:
                    del out[j]
        if k - 1 not in out and k + 1 not in out:
            out[k] = (k - 1, k + 1, fit[0])
    for k, (a, b, _) in list(out.items()):
        if (a in out or b in out) and 0 < k < n - 1 and k - 1 not in out and k + 1 not in out:
            fit = pair_fit(k)
            if fit is not None:
                out[k] = (k - 1, k + 1, fit[0])
    return out


_GHOST_BINS = np.arange(-0.3, 0.71, 0.02)
_GHOST_FMIN, _GHOST_FMAX = 0.08, 0.6
_GHOST_SHARP = 1.8  # ratio peak against the histogram median
_GHOST_PROMINENCE = 1.4  # ratio peak against the histogram at 0 (neighbour's edges missing from the frame)
_GHOST_GONE = 0.05  # after unblending, the neighbour's ratio peak must sit at 0
_GHOST_MAX_R2 = 0.95  # a frame explained by a N + b (fade, near hold) has no new drawing to recover


class _GhostFrame:
    """Per-frame data of the ghost analysis, computed once: raw statistics and the smoothed gradients."""

    def __init__(self, raw, smooth):
        self.raw = raw
        self.mean, self.var = float(raw.mean()), float(raw.var())
        self.grads = (np.diff(smooth, axis=0), np.diff(smooth, axis=1))
        self.masks = []
        for g in self.grads:
            a = np.abs(g)
            self.masks.append(a > np.percentile(a, 95))  # the strongest 5 % of the edges


def _ghost_hist(r):
    h, _ = np.histogram(r, _GHOST_BINS)
    return np.convolve(h, [1, 2, 3, 2, 1], mode="same").astype(np.float64)


def _ghost_of(o, n):
    """(f, sharpness) of n's ghost in o, or None. Where o's own drawing is flat, o's edges are exactly f times n's."""
    if n.var < 1e-6 or o.var < 1e-6:
        return None  # flat frame (black): nothing to unblend
    cov = float((o.raw * n.raw).mean()) - o.mean * n.mean
    if cov * cov / (n.var * o.var) >= _GHOST_MAX_R2:
        return None
    r = np.concatenate([go[m] / gn[m] for go, gn, m in zip(o.grads, n.grads, n.masks)])
    c = 0.5 * (_GHOST_BINS[1:] + _GHOST_BINS[:-1])
    h = _ghost_hist(r)
    i = int(np.argmax(h))
    f = float(c[i])
    if not _GHOST_FMIN <= f <= _GHOST_FMAX:
        return None
    sharp = h[i] / max(float(np.median(h)), 1.0)
    prominence = h[i] / max(float(h[np.abs(c) < 0.03].max()), 1.0)
    if sharp < _GHOST_SHARP or prominence < _GHOST_PROMINENCE:
        return None
    # after unblending, X's edges at n's edges have the ratio (r - f) / (1 - f); the ghost must be gone
    if abs(float(c[int(np.argmax(_ghost_hist((r - f) / (1.0 - f))))])) > _GHOST_GONE:
        return None
    return f, sharp


def analyse_ghosts(clip, width=720, skip=()):
    """Frames that carry a faint copy of a neighbour at the neighbour's own position: {frame: (neighbour, f)}.

    The frame is O = (1-f) X + f N with an unknown drawing X (a new pan position, say); X = (O - f N) / (1-f).
    Reads the whole clip once at the given width; frames in skip (e.g. blends that are replaced anyway) are not
    examined.
    """
    height = max(16, round(clip.height * width / clip.width / 2) * 2)
    raw = core.resize.Bilinear(clip, width, height, format=vs.GRAYS)
    smooth = core.std.BoxBlur(raw, hradius=1, hpasses=2, vradius=1, vpasses=2)
    ghosts = {}
    window = {}
    for i, (fr, fs) in enumerate(zip(raw.frames(), smooth.frames())):
        window[i] = _GhostFrame(np.asarray(fr[0], dtype=np.float32) * 255.0,
                                np.asarray(fs[0], dtype=np.float32) * 255.0)
        window.pop(i - 3, None)
        k = i - 1
        if k < 1 or k in skip:
            continue
        best = None
        for nb in (k - 1, k + 1):
            r = _ghost_of(window[k], window[nb])
            if r and (best is None or r[1] > best[2]):
                best = (nb, r[0], r[1])
        if best:
            ghosts[k] = (best[0], best[1])
    return ghosts


_ANALYSIS_VERSION = 2  # part of the cache key: raise it whenever a detection rule changes


def _cache_key(clip, **params):
    props = "v%d|%dx%d|%d|%d/%d|%s" % (_ANALYSIS_VERSION, clip.width, clip.height, clip.num_frames, clip.fps.numerator,
                                       clip.fps.denominator, clip.format.name)
    return props + "|" + "|".join("%s=%s" % kv for kv in sorted(params.items()))


def _analyse_part(clip, start, end, keep, p):
    """Blends of frames keep[0]..keep[1]-1, analysed on clip[start:end] (the part plus its context)."""
    sub = _Frames(list(_analysis_frames(clip[start:end], p["width"])))  # one read of the part (seeking is slow)
    var = p["var"]  # one estimate for the whole clip: a part without holds would measure motion instead of noise
    found = {}
    if p["mode"] in ("anchor", "both"):
        found.update(analyse(sub, p["maxgap"], p["rho"], p["kappa"], p["width"], p["minweight"], var))
    if p["mode"] in ("cadence", "both"):
        found.update(analyse_cadence(sub, p["switch"], p["width"], p["minweight"], var))
    if p["fill"]:
        found = fill_gaps(sub, found, p["width"], var)
    return {k + start: (a + start, b + start, f) for k, (a, b, f) in found.items() if keep[0] <= k + start < keep[1]}


def _ghost_part(clip, keep):
    """Ghosts of frames keep[0]..keep[1]-1 (all examined, so the result fits any blend setting)."""
    start, end = max(0, keep[0] - 1), min(clip.num_frames, keep[1] + 1)
    found = analyse_ghosts(clip[start:end])
    return {k + start: (nb + start, f) for k, (nb, f) in found.items() if keep[0] <= k + start < keep[1]}


def _fingerprint(clip):
    """Short digest of the first tiny frames (read in order: seeking can take seconds), so that cached results of two
    clips with the same format and length do not mix."""
    tiny = core.resize.Bilinear(clip[:8], 32, 18, format=vs.GRAY8)
    data = b"".join(bytes(np.asarray(f[0])) for f in tiny.frames())
    return hashlib.sha1(data).hexdigest()[:16]


_MEMO, _MEMO_LOCKS, _MEMO_GUARD = {}, {}, threading.Lock()


def _decode(value):
    return {int(k): tuple(v) for k, v in value.items()} if isinstance(value, dict) else value


def _memo(key, compute, cache):
    """Value under key from memory, the cache file or compute(); computed once, also across AnimDeblend calls."""
    if key in _MEMO:
        return _MEMO[key]
    with _MEMO_GUARD:
        lock = _MEMO_LOCKS.setdefault(key, threading.Lock())
    with lock:
        if key not in _MEMO:
            value = None
            if cache:
                try:
                    with open(cache, encoding="utf-8") as fh:
                        value = _decode(json.load(fh).get(key))
                except (OSError, ValueError):
                    value = None
            if value is None:
                value = compute()
                if cache:
                    with _MEMO_GUARD:
                        try:
                            with open(cache, encoding="utf-8") as fh:
                                data = json.load(fh)
                        except (OSError, ValueError):
                            data = {}
                        data[key] = {str(k): list(v) for k, v in value.items()} if isinstance(value, dict) else value
                        with open(cache, "w", encoding="utf-8") as fh:
                            json.dump(data, fh)
            _MEMO[key] = value
    return _MEMO[key]


def AnimDeblend(clip, strength=1.0, maxgap=4, rho=0.1, kappa=3.0, width=160, show=False, blends=None, minweight=0.1,
                mode="anchor", switch=1.5, unghost=False, ghosts=None, fill=True, cache=None, block=512, overlap=128):
    """Pull blend frames towards their dominant drawing; strength 0 leaves the clip unchanged, 1 replaces the blend.

    rho: how much better the blend explanation must be than copying an anchor (smaller = more cautious).
    minweight: smallest share of the weaker drawing that counts as a blend (0.01-0.49; smaller = fainter ghosts too).
    mode: "anchor", "cadence" or "both" (see the module text); switch: cadence change cost, only for cadence.
    unghost: also remove faint copies of a neighbour by unblending (see analyse_ghosts) on frames the blend modes do
    not treat; a mix of both neighbours is better replaced (unblending it only adds noise and a faint residual).
    fill: also treat frames missing inside a regular run of blends, or one past its end (see fill_gaps).
    block, overlap: the analysis runs per block of frames (with overlap frames of context on each side) when a frame
    of the block is first requested; block=0 analyses the whole clip at once.
    cache: path of a JSON file that keeps the analysis per block, clip properties and parameters; later calls with
    the same clip and settings skip it (delete the file when the source changes but keeps its properties).
    blends / ghosts: precomputed results (then nothing is analysed; fill still applies to blends).
    show: writes 'blend a-b f' / 'ghost n f' on changed frames (needs the text plugin).
    """
    if mode not in ("anchor", "cadence", "both"):
        raise ValueError('AnimDeblend: mode must be "anchor", "cadence" or "both"')
    n_frames = clip.num_frames
    block = block if block and block > 0 else n_frames
    p = dict(maxgap=maxgap, rho=rho, kappa=kappa, width=width, minweight=minweight, mode=mode, switch=switch,
             fill=fill, unghost=unghost)
    static = None
    if blends is not None or ghosts is not None:
        blends = dict(blends or {})
        if fill and blends:
            blends = fill_gaps(clip, blends, width)
        static = (blends, {k: v for k, v in (ghosts or {}).items() if k not in blends})
    guard = threading.Lock()
    clip_id = _cache_key(clip, fingerprint=_fingerprint(clip)) if static is None else ""
    sampled = block < n_frames
    blend_base = "blends|%s|block=%d|overlap=%d|%s" % (clip_id, block, overlap, "|".join(
        "%s=%s" % kv for kv in sorted(p.items()) if kv[0] != "unghost"))

    def part(i):
        """(blends, ghosts) of block i, analysed on first use; different blocks may run in parallel."""
        p["var"] = _memo("var|%s|width=%d|sampled=%s" % (clip_id, width, sampled),
                         lambda: _noise_var_sampled(clip, width) if sampled else _noise_var(clip, width), cache)
        s, e = i * block, min(n_frames, (i + 1) * block)
        found = _memo("%s|part=%d" % (blend_base, i),
                      lambda: _analyse_part(clip, max(0, s - overlap), min(n_frames, e + overlap), (s, e), p), cache)
        ghosted = {}
        if unghost:
            ghosted = _memo("ghosts|%s|block=%d|part=%d" % (clip_id, block, i), lambda: _ghost_part(clip, (s, e)), cache)
        return found, {k: v for k, v in ghosted.items() if k not in found}

    def neighbour(d):
        return (clip[0] * -d + clip)[:n_frames] if d < 0 else clip[d:] + clip[-1] * d

    nodes = {}

    def node(kind, d, f=0.0):
        """Shifted neighbour (copy) or unblended frame, built once per offset (and weight)."""
        key = (kind, d, round(f, 2))
        if key not in nodes:
            with guard:
                if key not in nodes:
                    if kind == "copy":
                        src = neighbour(d)
                        nodes[key] = core.std.Merge(clip, src, weight=strength) if strength < 1.0 else src
                    else:
                        g = key[2] * min(strength, 1.0)  # strength scales how much of the ghost is taken out
                        nodes[key] = core.std.Expr([clip, neighbour(d)], "x %f y * - %f /" % (g, 1.0 - g))
        return nodes[key]

    plain = core.std.SetFrameProps(clip, AnimBlend=0, AnimGhost=0)
    show = show and hasattr(core, "text")

    def pick(n):
        found, ghosted = static if static else part(n // block)
        if n in ghosted:
            nb, f = ghosted[n]
            out = node("ghost", nb - n, f)
            if show:
                out = core.text.Text(out, "ghost %d f=%.2f" % (nb, f), alignment=7)
            return core.std.SetFrameProps(out, AnimBlend=0, AnimGhost=1, AnimGhostFrom=nb, AnimGhostWeight=f)
        if n not in found:
            return plain
        a, b, f = found[n]
        out = node("copy", (a if f < 0.5 else b) - n)
        if show:
            out = core.text.Text(out, "blend %d-%d f=%.2f" % (a, b, f), alignment=7)
        return core.std.SetFrameProps(out, AnimBlend=1, AnimGhost=0, AnimBlendWeight=f, AnimBlendFrom=a, AnimBlendTo=b)

    return core.std.FrameEval(clip, pick)
