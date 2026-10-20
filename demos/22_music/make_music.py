# Generates the demo 22 assets (deterministic, committed):
#   lead.wav, bass.wav   looped tones (VADPCM), one exact period grid so the loops are seamless
#   kick.wav, snare.wav, hat.wav   one-shot drums (VADPCM)
#   pad.pcm              looped tone as raw big-endian s16 + guard sample (the s16 path)
#   song.mid             type 1 MIDI: 1 bar intro, then 8 looped bars (loopStart / loopEnd
#                        markers) of lead, bass, pad and drums with volume, pan, expression,
#                        sustain pedal, pitch bend and a tempo change
import math
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools'))
import k64music  # noqa: E402

TONE_RATE = 28160   # 440 Hz x 64: periods of 64 / 128 / 256 samples are A4 / A3 / A2
DRUM_RATE = 32000


def clip(v):
    return max(-32768, min(32767, int(round(v))))


def write_wav(name, rate, x):
    with wave.open(os.path.join(HERE, name), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(x), *[clip(v) for v in x]))


def tone(period, harmonics, attack_len, loop_len):
    """attack_len samples with a short fade-in, then loop_len samples (whole periods)."""
    out = []
    for i in range(attack_len + loop_len):
        t = 2 * math.pi * (i % period) / period
        v = sum(a * math.sin(k * t) for k, a in harmonics)
        out.append(v * min(1.0, i / 48.0))
    return out


def noise(seed):
    while True:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        yield ((seed >> 8) & 0xFFFF) / 32768.0 - 1.0


def kick(n=5000):
    out, ph = [], 0.0
    for i in range(n):
        f = 45 + 110 * math.exp(-i / 500.0)
        ph += 2 * math.pi * f / DRUM_RATE
        out.append(30000 * math.sin(ph) * math.exp(-i / 1800.0))
    return out


def snare(n=6000):
    out, g, ph = [], noise(7), 0.0
    for i in range(n):
        ph += 2 * math.pi * 190 / DRUM_RATE
        out.append(14000 * next(g) * math.exp(-i / 1300.0) + 9000 * math.sin(ph) * math.exp(-i / 500.0))
    return out


def hat(n=2400):
    out, g, prev = [], noise(99), 0.0
    for i in range(n):
        x = next(g)
        out.append(16000 * (x - prev) * math.exp(-i / 350.0))   # first difference = high-pass
        prev = x
    return out


# lead: A4, square-ish (odd harmonics); bass: A2, saw-ish; pad: A3, soft
write_wav('lead.wav', TONE_RATE, tone(64, [(1, 9000), (3, 3000), (5, 1800), (7, 900)], 1024, 1024))
write_wav('bass.wav', TONE_RATE, tone(256, [(k, 11000 / k) for k in range(1, 12)], 1024, 1024))
write_wav('kick.wav', DRUM_RATE, kick())
write_wav('snare.wav', DRUM_RATE, snare())
write_wav('hat.wav', DRUM_RATE, hat())
pad = tone(128, [(1, 7000), (2, 3000), (3, 1500), (4, 700)], 0, 2048)
with open(os.path.join(HERE, 'pad.pcm'), 'wb') as f:
    f.write(struct.pack('>%dh' % (len(pad) + 1), *[clip(v) for v in pad + pad[:1]]))

# ---- song --------------------------------------------------------------------------------
PPQ = 96
BAR = PPQ * 4
E = PPQ // 2                      # eighth note


def on(ch, key, vel):
    return bytes([0x90 | ch, key, vel])


def off(ch, key):
    return bytes([0x80 | ch, key, 0])


def cc(ch, c, v):
    return bytes([0xB0 | ch, c, v])


def note(tr, t, ch, key, vel, dur):
    tr.append((t, on(ch, key, vel)))
    tr.append((t + dur, off(ch, key)))


meta = [(0, b'\xFF\x51\x03' + (500000).to_bytes(3, 'big')),   # 120 bpm
        (BAR, b'\xFF\x06\x09loopStart'),
        (BAR + 4 * BAR, b'\xFF\x51\x03' + (461538).to_bytes(3, 'big')),   # 130 bpm, 2nd half
        (9 * BAR, b'\xFF\x06\x07loopEnd')]

# chords Am F C G (roots A2 F2 C3 G2), two bars each over the 8 looped bars
roots = [45, 41, 48, 43]
chords = [[57, 60, 64], [53, 57, 60], [55, 60, 64], [55, 59, 62]]

lead, bass, pad_t, drums = [], [], [], []
lead += [(0, bytes([0xC0, 0])), (0, cc(0, 7, 110)), (0, cc(0, 10, 50)),
         (0, cc(0, 101, 0)), (0, cc(0, 100, 0)), (0, cc(0, 6, 12))]       # bend range 12
bass += [(0, bytes([0xC1, 1])), (0, cc(1, 7, 120)), (0, cc(1, 10, 64))]
pad_t += [(0, bytes([0xC2, 2])), (0, cc(2, 7, 90)), (0, cc(2, 10, 84)), (0, cc(2, 11, 40))]
drums += [(0, cc(9, 7, 115))]

# intro bar: pad swell (expression) on Am
for k in chords[0]:
    note(pad_t, 0, 2, k, 80, BAR - 8)
for i in range(16):
    pad_t.append((i * BAR // 16, cc(2, 11, 40 + i * 5)))

pent = [69, 72, 74, 76, 79, 81]
seed = 5
for bar in range(8):
    t0 = BAR + bar * BAR
    c = (bar // 2) % 4
    # bass: eighth notes on the root, octave jumps on the off-beats
    for i in range(8):
        note(bass, t0 + i * E, 1, roots[c] + (12 if i % 2 else 0), 100 if i % 2 == 0 else 80, E - 6)
    # pad: chord held with the sustain pedal down for the bar (note ends early, pedal holds it)
    pad_t.append((t0, cc(2, 64, 127)))
    for k in chords[c]:
        note(pad_t, t0, 2, k, 70, PPQ)
    pad_t.append((t0 + BAR - 4, cc(2, 64, 0)))
    # drums: kick 1 and 3 (+ the "and" of 3 in odd bars), snare 2 and 4, hats on eighths
    for b in (0, 2):
        note(drums, t0 + b * PPQ, 9, 36, 120, E)
    if bar % 2:
        note(drums, t0 + 2 * PPQ + E, 9, 36, 100, E)
    for b in (1, 3):
        note(drums, t0 + b * PPQ, 9, 38, 110, E)
    for i in range(8):
        note(drums, t0 + i * E, 9, 42, 90 if i % 2 == 0 else 60, E // 2)
    # lead: a pentatonic line, quarter + eighth notes, a bend up into the last note of odd bars
    t = t0
    while t < t0 + BAR:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        d = PPQ if (seed >> 16) % 3 else E
        if t + d > t0 + BAR:
            d = t0 + BAR - t
        k = pent[(seed >> 8) % len(pent)]
        note(lead, t, 0, k, 90 + (seed >> 4) % 30, d - 8)
        t += d
    if bar % 2:
        for i in range(8):                    # bend up 2 semitones (range 12) over an eighth
            v = 8192 + i * 1365 // 8 * 1      # 0 .. +2 semitones
            lead.append((t0 + BAR - E + i * 6, bytes([0xE0, v & 0x7F, v >> 7])))
        lead.append((t0 + BAR, bytes([0xE0, 0, 0x40])))   # back to center at the next bar

k64music.write_smf(os.path.join(HERE, 'song.mid'), PPQ, [meta, lead, bass, pad_t, drums])
print('make_music: wrote lead/bass/kick/snare/hat.wav, pad.pcm, song.mid')
