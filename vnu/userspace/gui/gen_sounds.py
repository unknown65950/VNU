#!/usr/bin/env python3
"""Regenerates the demo WAV clips under vnu/userspace/gui/sounds/.

    python3 vnu/userspace/gui/gen_sounds.py

The clips are source data (like the pictures in gui/pics/) and are
committed to the repo; at boot the kernel embeds and mounts them into
the VFS under /sounds (see vnu/kernel/gui/apps.cpp install_demo_sounds).
The play desktop app lists and plays them.

Each clip must stay well inside the VFS per-file cap of 65536 bytes
(vnu/kernel/fs/vfs.cpp DATA_CAP), and the formats below deliberately
cover all three shapes the kernel's audio driver converts from:
16-bit mono (chime), 16-bit mono at a slower rate (melody) and 8-bit
mono (beep). Pure stdlib, no numpy/wave module tricks beyond `wave`.
"""
import math
import os
import wave

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sounds")

A4 = 440.0


def freq(note):
    """Equal-temperament frequency for a note name like 'C5' or 'Eb4'."""
    names = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4,
             "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8, "Ab": 8, "A": 9,
             "A#": 10, "Bb": 10, "B": 11}
    if note == "A4":
        return A4
    name = note[:-1]
    octv = int(note[-1])
    semis = names[name] - 9 + (octv - 4) * 12
    return A4 * 2.0 ** (semis / 12.0)


def write_wav(path, rate, channels, bits, samples):
    """samples: list of ints (mono) or list of (l, r) tuples (stereo)."""
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        frames = bytearray()
        if channels == 1:
            if bits == 8:
                frames += bytes(max(0, min(255, int(s * 127.5 + 127.5)))
                                for s in samples)
            else:
                for s in samples:
                    v = max(-32768, min(32767, int(round(s))))
                    frames += v.to_bytes(2, "little", signed=True)
        else:
            for l, r in samples:
                if bits == 8:
                    frames += bytes((max(0, min(255, int(l * 127.5 + 127.5))),
                                     max(0, min(255, int(r * 127.5 + 127.5)))))
                else:
                    frames += max(-32768, min(32767, int(round(l)))).to_bytes(
                        2, "little", signed=True)
                    frames += max(-32768, min(32767, int(round(r)))).to_bytes(
                        2, "little", signed=True)
        w.writeframes(bytes(frames))
    print(f"{os.path.basename(path)}: {len(frames)} bytes, "
          f"{rate} Hz, {channels}ch, {bits}-bit")


def note_tone(t, f, harmonics=(1.0, 0.0, 0.0), decay=0.0, attack=0.008,
              release=0.04, dur=None):
    """Sample value of one note at time t in seconds (0 if outside)."""
    if dur is not None and (t < 0 or t >= dur):
        return 0.0
    env = 1.0
    if attack > 0 and t < attack:
        env = t / attack
    if dur is not None and release > 0 and dur - t < release:
        env = min(env, (dur - t) / release)
    if decay > 0:
        env *= math.exp(-decay * t)
    v = 0.0
    for k, amp in enumerate(harmonics):
        if amp:
            v += amp * math.sin(2.0 * math.pi * f * (k + 1) * t)
    return env * v


def make_chime():
    """Bell-like C major arpeggio, 16-bit mono @ 22050. ~1.35 s."""
    rate = 22050
    dur = 1.35
    n = int(rate * dur)
    out = [0.0] * n
    notes = [("C5", 0.00), ("E5", 0.34), ("G5", 0.68), ("C6", 1.02)]
    for name, start in notes:
        f = freq(name)
        for i in range(n):
            t = i / rate - start
            if t < 0:
                continue
            v = note_tone(t, f,
                          harmonics=(1.0, 0.45, 0.18), decay=9.0, dur=0.36)
            out[i] += v
    peak = max(abs(v) for v in out) or 1.0
    return rate, [v * 0.85 / peak for v in out]


def make_melody():
    """Ode to Joy, first phrase, 16-bit mono @ 11025. ~2.4 s."""
    rate = 11025
    quarter = 0.15
    seq = [(freq("E4"), 1), (freq("E4"), 1), (freq("F4"), 1), (freq("G4"), 1),
           (freq("G4"), 1), (freq("F4"), 1), (freq("E4"), 1), (freq("D4"), 1),
           (freq("C4"), 1), (freq("C4"), 1), (freq("D4"), 1), (freq("E4"), 1),
           (freq("E4"), 1), (freq("D4"), 1), (freq("D4"), 2)]
    total = sum(beats for _, beats in seq) * quarter
    n = int(rate * total)
    out = [0.0] * n
    t0 = 0.0
    for f, beats in seq:
        d = beats * quarter
        for i in range(n):
            t = i / rate - t0
            if t < 0:
                continue
            v = note_tone(t, f, harmonics=(1.0, 0.25), dur=d, release=0.05)
            out[i] += v
        t0 += d
    peak = max(abs(v) for v in out) or 1.0
    return rate, [v * 0.85 / peak for v in out]


def make_beep():
    """Two square-wave boops, 8-bit mono @ 8000. ~0.6 s."""
    rate = 8000
    n = int(rate * 0.6)
    out = [0.0] * n
    for i in range(n):
        t = i / rate
        if t < 0.02 or t >= 0.58:           # tiny silence at each end
            continue
        f = 880.0 if t < 0.28 else 660.0
        # square-ish: soft-clipped triangle/square blend
        raw = 1.0 if math.sin(2.0 * math.pi * f * t) >= 0 else -1.0
        env = min(1.0, (t - 0.02) / 0.02, (0.58 - t) / 0.02)
        out[i] = raw * env
    return rate, out


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for maker, name in ((make_chime, "chime.wav"),
                        (make_melody, "melody.wav"),
                        (make_beep, "beep.wav")):
        rate, samples = maker()
        write_wav(os.path.join(OUT_DIR, name), rate, 1, 16 if "beep" not in name else 8, samples)


if __name__ == "__main__":
    main()