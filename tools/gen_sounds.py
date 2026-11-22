#!/usr/bin/env python3
"""Synthesizes the client's placeholder sound effects and the login ambience (Phase 8).

Every sound is generated here from sine/noise envelopes, so the files are original to the project and
released CC0 (see ASSETS.md). The Phase 9 art pass replaces them; the event names in
client/data/sounds.json stay.

  python3 tools/gen_sounds.py           write client/assets/audio/*.wav
  python3 tools/gen_sounds.py --check   fail if any file differs from what this script makes
"""
import io
import math
import os
import random
import struct
import sys
import wave

RATE = 22050
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "client", "assets", "audio")


def env(t, attack, decay, total):
    if t < attack:
        return t / attack
    return max(0.0, 1.0 - (t - attack) / max(1e-6, decay)) if t < total else 0.0


def tone(freqs, dur, attack=0.005, decay=None, vol=0.5, noise=0.0, slide=0.0, seed=1):
    rng = random.Random(seed)
    decay = dur - attack if decay is None else decay
    out = []
    for i in range(int(dur * RATE)):
        t = i / RATE
        e = env(t, attack, decay, dur)
        s = 0.0
        for k, f in enumerate(freqs):
            f2 = f * (1.0 + slide * t / dur)
            s += math.sin(2 * math.pi * f2 * t) / (k + 1)
        s = s / max(1, len(freqs)) * (1.0 - noise) + (rng.random() * 2 - 1) * noise
        out.append(s * e * vol)
    return out


def concat(*parts):
    out = []
    for p in parts:
        out += p
    return out


def mix(a, b):
    n = max(len(a), len(b))
    return [(a[i] if i < len(a) else 0.0) + (b[i] if i < len(b) else 0.0) for i in range(n)]


def silence(dur):
    return [0.0] * int(dur * RATE)


def pad_loop(dur=8.0):
    # A soft drone on a minor chord with slow tremolo; loops cleanly because every partial completes
    # a whole number of cycles over `dur`.
    chord = [110.0, 130.8125, 164.8125, 220.0]
    chord = [round(f * dur) / dur for f in chord]
    out = []
    for i in range(int(dur * RATE)):
        t = i / RATE
        s = sum(math.sin(2 * math.pi * f * t) * (0.6 if k else 1.0) for k, f in enumerate(chord)) / len(chord)
        trem = 0.75 + 0.25 * math.sin(2 * math.pi * t / dur * 2)
        out.append(s * trem * 0.25)
    return out


SOUNDS = {
    "ui_click": lambda: tone([1800, 2400], 0.04, vol=0.3),
    "ui_open": lambda: tone([660, 990], 0.09, vol=0.25, slide=0.3),
    "ui_close": lambda: tone([990, 660], 0.09, vol=0.25, slide=-0.3),
    "ui_error": lambda: concat(tone([220, 233], 0.12, vol=0.35), tone([196, 208], 0.16, vol=0.35)),
    "login_ok": lambda: concat(tone([523.25], 0.1, vol=0.3), tone([659.25], 0.1, vol=0.3), tone([783.99], 0.22, vol=0.3)),
    "enter_world": lambda: mix(tone([261.6, 392.0], 0.9, attack=0.2, vol=0.3), tone([523.25], 0.9, attack=0.4, vol=0.15)),
    "melee_hit": lambda: tone([140, 90], 0.12, attack=0.002, vol=0.5, noise=0.55, seed=3),
    "melee_miss": lambda: tone([900], 0.1, attack=0.01, vol=0.2, noise=0.8, slide=-0.5, seed=4),
    "hit_taken": lambda: tone([110, 70], 0.16, attack=0.002, vol=0.55, noise=0.45, seed=5),
    "spell_cast": lambda: tone([440, 660, 880], 0.5, attack=0.15, vol=0.25, slide=0.6),
    "spell_land": lambda: mix(tone([880, 1320], 0.35, attack=0.005, vol=0.3, slide=-0.4), tone([0], 0.2, vol=0.15, noise=1.0, seed=6)),
    "spell_fizzle": lambda: tone([300], 0.25, vol=0.25, noise=0.6, slide=-0.6, seed=7),
    "heal": lambda: concat(tone([587.3, 880], 0.18, vol=0.25), tone([698.5, 1046.5], 0.3, vol=0.25)),
    "level_up": lambda: concat(*[tone([f, f * 1.5], 0.12, vol=0.3) for f in (392.0, 523.25, 659.25)], tone([783.99, 1174.7], 0.5, vol=0.3)),
    "death": lambda: concat(tone([220, 110], 0.4, vol=0.35, slide=-0.3), tone([164.8, 82.4], 0.8, vol=0.35, slide=-0.3)),
    "loot_coin": lambda: concat(tone([2093, 3136], 0.06, vol=0.25), silence(0.03), tone([2349, 3520], 0.09, vol=0.25)),
    "loot_item": lambda: tone([520, 780], 0.14, vol=0.3, noise=0.15, seed=8),
    "quest_done": lambda: concat(tone([523.25, 784], 0.15, vol=0.3), tone([659.25, 987.8], 0.15, vol=0.3), tone([1046.5, 1568], 0.4, vol=0.3)),
    "tell": lambda: concat(tone([1174.7], 0.07, vol=0.25), tone([1568], 0.12, vol=0.25)),
    "invite": lambda: concat(tone([784], 0.1, vol=0.25), tone([1046.5], 0.1, vol=0.25), tone([784], 0.1, vol=0.25)),
    "zone_ambience": pad_loop,
}


def encode(samples):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples))
    return buf.getvalue()


def same_audio(a, b):
    if len(a) != len(b):
        return False
    if a == b:
        return True
    if a[:44] != b[:44]:
        return False
    n = (len(a) - 44) // 2
    sa = struct.unpack("<%dh" % n, a[44:44 + 2 * n])
    sb = struct.unpack("<%dh" % n, b[44:44 + 2 * n])
    return all(abs(x - y) <= 2 for x, y in zip(sa, sb))


def main():
    check = "--check" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    stale = []
    for name, make in sorted(SOUNDS.items()):
        data = encode(make())
        path = os.path.join(OUT, name + ".wav")
        if check:
            # Within a couple of LSBs: libm's sin() may round differently on another platform.
            if not os.path.exists(path) or not same_audio(open(path, "rb").read(), data):
                stale.append(name)
            continue
        with open(path, "wb") as f:
            f.write(data)
    if check:
        if stale:
            print("stale or missing sounds (run tools/gen_sounds.py): " + ", ".join(stale))
            return 1
        print("sounds are up to date")
        return 0
    print("wrote %d sounds to %s" % (len(SOUNDS), os.path.normpath(OUT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
