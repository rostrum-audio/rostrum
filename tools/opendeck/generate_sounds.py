"""Generate Rostrum's original mic feedback cues using only the standard library."""
import math
from pathlib import Path
import struct
import wave

RATE = 48000
NOTE = 0.09
GAP = 0.02
ROOT = Path(__file__).parent / 'dev.getrostrum.Rostrum.sdPlugin' / 'sounds'


def note(frequency):
    samples = []
    count = round(NOTE * RATE)
    for index in range(count):
        t = index / RATE
        # Smooth attack and release prevent clicks; a quiet harmonic softens the pure tone.
        attack = min(1, t / 0.012)
        release = min(1, (count - 1 - index) / (RATE * 0.035))
        envelope = math.sin(attack * math.pi / 2) ** 2 * math.sin(release * math.pi / 2) ** 2
        phase = 2 * math.pi * frequency * t
        value = (math.sin(phase) + 0.12 * math.sin(2 * phase)) / 1.12
        samples.append(round(32767 * 0.44 * envelope * value))
    return samples


def main():
    ROOT.mkdir(exist_ok=True)
    for name, frequencies in [('mic-muted.wav', (660, 440)), ('mic-live.wav', (440, 660))]:
        samples = note(frequencies[0]) + [0] * round(GAP * RATE) + note(frequencies[1])
        with wave.open(str(ROOT / name), 'wb') as audio:
            audio.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
            audio.writeframes(struct.pack('<' + 'h' * len(samples), *samples))


if __name__ == '__main__':
    main()
