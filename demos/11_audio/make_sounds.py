# Generates the demo 11 sounds (deterministic, committed): pluck.wav, a Karplus-Strong string
# (one-shot), and organ.wav, a short attack then a 120-sample loop (looped from sample 600).
import math
import struct
import wave
import os

RATE = 32000
HERE = os.path.dirname(os.path.abspath(__file__))


def write(name, x):
    with wave.open(os.path.join(HERE, name), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(struct.pack('<%dh' % len(x), *[max(-32768, min(32767, int(v))) for v in x]))


def pluck(n=12000, period=122):
    seed = 12345
    buf = []
    for _ in range(period):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        buf.append(((seed >> 8) & 0xFFFF) - 32768)
    out = []
    for i in range(n):
        j = i % period
        v = buf[j]
        out.append(v * 0.8)
        buf[j] = 0.497 * (v + buf[(j + 1) % period])
    return out


def organ(attack=600, period=120, loops=1):
    out = []
    for i in range(attack + period * loops):
        t = 2 * math.pi * (i % period) / period
        tone = 9000 * math.sin(t) + 5000 * math.sin(2 * t) + 3000 * math.sin(3 * t)
        env = min(1.0, i / 200.0)
        out.append(tone * env)
    return out


write('pluck.wav', pluck())
write('organ.wav', organ())
