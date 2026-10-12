"""Generates the Showcase project's sounds (Assets/Audio/*.wav): a music loop, forest (day), crickets (night), campfire, fountain
and neon loops, the impact sounds, the sounds of the exhibits (goal chime, cannon, gate, the neon sign lighting up) and the
HUD's button sounds.

Everything is synthesized here, so the sounds have no license of their own and regenerate identically (fixed seeds):

    python Projects/Showcase/Tools/GenerateAudio.py

16-bit mono WAV at 22050 Hz keeps the files small; the engine plays them at the device's rate.
"""
import math
import os
import random
import struct
import wave

RATE = 22050
OUTPUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Assets", "Audio")


def write_wav(name, samples, peak=0.89):
    """Normalizes to the peak and writes 16-bit mono."""
    loudest = max(1e-9, max(abs(sample) for sample in samples))
    scale = peak / loudest
    os.makedirs(OUTPUT, exist_ok=True)
    with wave.open(os.path.join(OUTPUT, name), "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(RATE)
        file.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, sample * scale)) * 32767)) for sample in samples))


def note(midi):
    return 440.0 * 2.0 ** ((midi - 69) / 12.0)


class Biquad:
    """RBJ band-pass (constant peak gain) or low-pass filter."""

    def __init__(self, kind, frequency, q):
        self.Set(kind, frequency, q)
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0

    def Set(self, kind, frequency, q):
        w = 2.0 * math.pi * frequency / RATE
        alpha = math.sin(w) / (2.0 * q)
        cosine = math.cos(w)
        if kind == "band":
            b0, b1, b2 = alpha, 0.0, -alpha
        else:
            b0, b1, b2 = (1.0 - cosine) / 2.0, 1.0 - cosine, (1.0 - cosine) / 2.0
        a0, a1, a2 = 1.0 + alpha, -2.0 * cosine, 1.0 - alpha
        self.b0, self.b1, self.b2, self.a1, self.a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0

    def __call__(self, x):
        y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2 - self.a1 * self.y1 - self.a2 * self.y2
        self.x2, self.x1, self.y2, self.y1 = self.x1, x, self.y1, y
        return y


def reverb(samples, mix=0.3, seconds=2.0):
    """A small Schroeder reverb: parallel feedback combs into series all-passes."""
    combs = [(1557, 0.84), (1617, 0.83), (1491, 0.85), (1422, 0.86)]
    scale = RATE / 44100.0
    wet = [0.0] * len(samples)
    for delay, feedback in combs:
        delay = int(delay * scale)
        feedback = feedback ** (2.0 / seconds)
        buffer = [0.0] * delay
        index = 0
        for i, x in enumerate(samples):
            y = buffer[index]
            buffer[index] = x + y * feedback
            index = (index + 1) % delay
            wet[i] += y * 0.25
    for delay in (int(225 * scale), int(556 * scale)):
        buffer = [0.0] * delay
        index = 0
        for i, x in enumerate(wet):
            delayed = buffer[index]
            y = -0.5 * x + delayed
            buffer[index] = x + 0.5 * y
            index = (index + 1) % delay
            wet[i] = y
    return [dry * (1.0 - mix) + w * mix for dry, w in zip(samples, wet)]


def make_loop(samples, loop_length):
    """Folds what rings past the loop's end back onto its start, so the loop repeats without a seam."""
    loop = samples[:loop_length]
    for i, sample in enumerate(samples[loop_length:]):
        loop[i % loop_length] += sample
    return loop


def music():
    """Four chords, two bars each at 70 BPM: a slow pad, a plucked arpeggio and a soft bass."""
    random.seed(1)
    beat = 60.0 / 70.0
    bar = 4 * beat
    chords = [  # A minor 7, F major 7, C major 7, G with an added ninth: (bass, pad and arpeggio notes)
        (45, [57, 60, 64, 67], [69, 72, 76, 79, 76, 72]),
        (41, [53, 57, 60, 64], [65, 69, 72, 76, 72, 69]),
        (48, [55, 60, 64, 67], [67, 72, 76, 79, 76, 72]),
        (43, [55, 59, 62, 69], [67, 71, 74, 81, 74, 71]),
    ]
    chord_length = 2 * bar
    loop_length = int(len(chords) * chord_length * RATE)
    total = loop_length + int(4.0 * RATE)
    out = [0.0] * total

    for index, (bass, pad, arpeggio) in enumerate(chords):
        start = int(index * chord_length * RATE)
        length = int(chord_length * RATE)
        # Pad: detuned sines with a slow swell; the release overlaps the next chord.
        release = int(1.5 * RATE)
        for midi in pad:
            for detune in (-0.08, 0.08):
                frequency = note(midi) * 2.0 ** (detune / 12.0)
                phase = random.random() * 2.0 * math.pi
                for i in range(length + release):
                    envelope = min(1.0, i / (1.2 * RATE)) * (1.0 if i < length else 1.0 - (i - length) / release)
                    out[start + i] += 0.035 * envelope * math.sin(phase + 2.0 * math.pi * frequency * i / RATE)
        # Bass: a sine with its octave, one note per bar.
        for half in range(2):
            begin = start + int(half * bar * RATE)
            count = int(bar * RATE)
            frequency = note(bass)
            for i in range(count):
                envelope = min(1.0, i / 400.0) * math.exp(-i / (1.6 * RATE))
                t = i / RATE
                out[begin + i] += 0.11 * envelope * (math.sin(2.0 * math.pi * frequency * t) + 0.3 * math.sin(4.0 * math.pi * frequency * t))
        # Arpeggio: Karplus-Strong plucks on eighth notes, a little behind the beat for a human feel.
        steps = int(chord_length / (beat / 2.0))
        for step in range(steps):
            midi = arpeggio[step % len(arpeggio)]
            begin = start + int(step * beat / 2.0 * RATE) + random.randint(0, 120)
            period = int(RATE / note(midi))
            buffer = [random.uniform(-1.0, 1.0) for _ in range(period)]
            velocity = 0.05 * (1.0 if step % 4 == 0 else 0.75)
            count = int(1.2 * RATE)
            for i in range(count):
                value = buffer[i % period]
                buffer[i % period] = 0.497 * (value + buffer[(i + 1) % period])
                if begin + i < total:
                    out[begin + i] += velocity * value
    # A low-pass softens the plucks before the reverb.
    smoothing = Biquad("low", 3800.0, 0.7)
    out = [smoothing(sample) for sample in out]
    return make_loop(reverb(out, mix=0.35, seconds=2.5), loop_length)


def ambience():
    """Wind (filtered noise with gusts) and birds (chirps with vibrato) in a 24 second loop."""
    random.seed(2)
    loop_length = int(24.0 * RATE)
    total = loop_length + int(2.0 * RATE)
    out = [0.0] * total
    wind = Biquad("band", 500.0, 0.6)
    brown = 0.0
    for i in range(total):
        t = i / RATE
        brown = 0.985 * brown + random.uniform(-1.0, 1.0) * 0.15
        gust = 0.55 + 0.3 * math.sin(2.0 * math.pi * t / 7.3) + 0.15 * math.sin(2.0 * math.pi * t / 2.9 + 1.0)
        if i % 64 == 0:
            wind.Set("band", 380.0 + 260.0 * gust, 0.6)
        out[i] += 0.35 * gust * wind(brown)
    # Birds: a few singers, each with its own pitch, repeating short phrases.
    for singer in range(4):
        base = random.uniform(2600.0, 4200.0)
        time = random.uniform(0.0, 3.0)
        while time < 24.0:
            for _ in range(random.randint(2, 5)):
                length = random.uniform(0.06, 0.16)
                sweep = random.uniform(-0.35, 0.45)
                begin = int(time * RATE)
                count = int(length * RATE)
                phase = 0.0
                for i in range(count):
                    progress = i / count
                    frequency = base * (1.0 + sweep * progress) * (1.0 + 0.04 * math.sin(2.0 * math.pi * 38.0 * i / RATE))
                    phase += 2.0 * math.pi * frequency / RATE
                    envelope = math.sin(math.pi * progress) ** 2
                    if begin + i < total:
                        out[begin + i] += 0.06 * envelope * math.sin(phase)
                time += length + random.uniform(0.03, 0.09)
            time += random.uniform(2.0, 6.5)
    return make_loop(reverb(out, mix=0.25, seconds=1.4), loop_length)


def wood_hit():
    """A knock: a click and a noise burst through the resonances of a wooden box."""
    random.seed(3)
    count = int(0.35 * RATE)
    modes = [(Biquad("band", frequency, 12.0), gain, decay) for frequency, gain, decay in
             ((165.0, 1.0, 0.09), (390.0, 0.7, 0.06), (870.0, 0.45, 0.035), (1650.0, 0.25, 0.02))]
    out = []
    for i in range(count):
        excitation = random.uniform(-1.0, 1.0) * math.exp(-i / (0.004 * RATE))
        sample = 0.0
        for filter, gain, decay in modes:
            sample += gain * filter(excitation) * math.exp(-i / (decay * RATE * 1.5))
        # The resonances ring on in the filters: the last third fades out so the sound ends without a click.
        fade = min(1.0, 3.0 * (count - i) / count)
        out.append((sample + 0.15 * excitation) * fade)
    return out


def metal_hit():
    """A clang: inharmonic partials of a struck metal ball, the high ones fading first."""
    count = int(0.9 * RATE)
    partials = [(1.0, 1.0, 0.5), (2.76, 0.6, 0.3), (5.40, 0.4, 0.18), (8.93, 0.25, 0.1), (13.3, 0.15, 0.06)]
    fundamental = 420.0
    out = []
    for i in range(count):
        t = i / RATE
        sample = 0.0
        for ratio, gain, decay in partials:
            sample += gain * math.exp(-t / decay) * math.sin(2.0 * math.pi * fundamental * ratio * t)
        out.append(sample * min(1.0, i / 30.0))
    return out


def throw():
    """A whoosh: noise through a band-pass sweeping up and down."""
    random.seed(4)
    count = int(0.4 * RATE)
    filter = Biquad("band", 300.0, 1.5)
    out = []
    for i in range(count):
        progress = i / count
        if i % 32 == 0:
            filter.Set("band", 300.0 + 1500.0 * math.sin(math.pi * progress), 1.5)
        out.append(filter(random.uniform(-1.0, 1.0)) * math.sin(math.pi * progress) ** 1.5)
    return out


def crackle():
    """A campfire in an 8 second loop: the low roar of the flames, crackles (short bright bursts) and a few deeper pops."""
    random.seed(5)
    loop_length = int(8.0 * RATE)
    total = loop_length + int(0.5 * RATE)
    out = [0.0] * total
    roar = Biquad("low", 260.0, 0.7)
    brown = 0.0
    for i in range(total):
        brown = 0.99 * brown + random.uniform(-1.0, 1.0) * 0.1
        flicker = 0.75 + 0.25 * math.sin(2.0 * math.pi * i / RATE * 0.7) * math.sin(2.0 * math.pi * i / RATE * 1.9)
        out[i] += 0.5 * flicker * roar(brown)
    time = 0.0
    while time < 8.0:
        pop = random.random() < 0.12
        band = Biquad("band", random.uniform(500.0, 900.0) if pop else random.uniform(1500.0, 5000.0), 1.2)
        gain = random.uniform(0.5, 1.0) if pop else random.uniform(0.08, 0.45)
        decay = random.uniform(0.012, 0.03) if pop else random.uniform(0.002, 0.008)
        begin = int(time * RATE)
        for i in range(int(decay * RATE * 6.0)):
            if begin + i < total:
                out[begin + i] += gain * band(random.uniform(-1.0, 1.0)) * math.exp(-i / (decay * RATE))
        # Crackles come in little clusters.
        time += random.expovariate(14.0) if random.random() < 0.7 else random.uniform(0.15, 0.5)
    return make_loop(out, loop_length)


def fountain_water():
    """Falling water in a 6 second loop: a wash of droplets (fast-flickering band-passed noise) and bubbling chirps."""
    random.seed(6)
    loop_length = int(6.0 * RATE)
    total = loop_length + int(0.5 * RATE)
    out = [0.0] * total
    wash = Biquad("band", 1800.0, 0.5)
    low = Biquad("low", 5200.0, 0.7)
    level = 0.0
    for i in range(total):
        if i % 40 == 0:
            level = random.uniform(0.55, 1.0)
        out[i] += 0.35 * level * low(wash(random.uniform(-1.0, 1.0)))
    for _ in range(int(6.0 * 22)):
        begin = random.randint(0, total - 1)
        length = random.uniform(0.008, 0.03)
        start = random.uniform(500.0, 1100.0)
        rise = random.uniform(1.4, 2.4)
        phase = 0.0
        count = int(length * RATE)
        gain = random.uniform(0.05, 0.16)
        for i in range(count):
            progress = i / count
            phase += 2.0 * math.pi * start * (1.0 + (rise - 1.0) * progress) / RATE
            if begin + i < total:
                out[begin + i] += gain * math.sin(phase) * math.sin(math.pi * progress)
    return make_loop(reverb(out, mix=0.2, seconds=0.8), loop_length)


def goal_chime():
    """A bright rising arpeggio of bell tones for a goal."""
    count = int(2.2 * RATE)
    out = [0.0] * count
    for index, midi in enumerate((72, 76, 79, 84)):
        begin = int(index * 0.09 * RATE)
        frequency = note(midi)
        for i in range(count - begin):
            t = i / RATE
            # Bell partials: the fundamental, a minor-third-ish partial and the octaves, the high ones fading first.
            sample = (math.sin(2.0 * math.pi * frequency * t) * math.exp(-t / 0.9) +
                      0.4 * math.sin(2.0 * math.pi * frequency * 2.4 * t) * math.exp(-t / 0.35) +
                      0.25 * math.sin(2.0 * math.pi * frequency * 4.0 * t) * math.exp(-t / 0.2))
            out[begin + i] += 0.25 * sample * min(1.0, i / 40.0)
    return reverb(out, mix=0.3, seconds=1.6)


def cannon_boom():
    """A cannon shot: a crack, a low thump sweeping down and a rumbling tail."""
    random.seed(7)
    count = int(1.8 * RATE)
    rumble = Biquad("low", 180.0, 0.7)
    crack = Biquad("band", 2400.0, 0.8)
    out = []
    phase = 0.0
    for i in range(count):
        t = i / RATE
        frequency = 40.0 + 70.0 * math.exp(-t / 0.08)
        phase += 2.0 * math.pi * frequency / RATE
        thump = math.sin(phase) * math.exp(-t / 0.35)
        noise = random.uniform(-1.0, 1.0)
        tail = rumble(noise) * 2.5 * math.exp(-t / 0.6)
        snap = crack(noise) * math.exp(-t / 0.012)
        out.append((0.9 * thump + 0.7 * tail + 0.8 * snap) * min(1.0, i / 20.0))
    return reverb(out, mix=0.25, seconds=1.5)


def gate_rumble():
    """A portcullis on the move: a motor's hum, the rattle of its chain and a clank at the end."""
    random.seed(8)
    count = int(1.6 * RATE)
    rattle = Biquad("band", 1100.0, 2.0)
    out = [0.0] * count
    for i in range(count):
        t = i / RATE
        envelope = min(1.0, t / 0.1) * min(1.0, (1.25 - t) / 0.1) if t < 1.25 else 0.0
        hum = math.sin(2.0 * math.pi * 72.0 * t) + 0.5 * math.sin(2.0 * math.pi * 144.0 * t) + 0.25 * math.sin(2.0 * math.pi * 216.0 * t)
        # Chain links click past at about 26 per second.
        click = math.exp(-((t * 26.0) % 1.0) * 18.0)
        out[i] += envelope * (0.18 * hum + 0.6 * click * rattle(random.uniform(-1.0, 1.0)))
    clank_start = int(1.22 * RATE)
    for i in range(count - clank_start):
        t = i / RATE
        sample = sum(gain * math.exp(-t / decay) * math.sin(2.0 * math.pi * 180.0 * ratio * t)
                     for ratio, gain, decay in ((1.0, 1.0, 0.25), (2.7, 0.5, 0.12), (5.1, 0.3, 0.06)))
        out[clank_start + i] += 0.6 * sample * min(1.0, i / 25.0)
    return out


# When the neon sign's tubes are lit after it is switched on, as (start, end, level): None ends a tube's last flicker, which
# stays on. NeonSign.cs flickers the tubes by the same timelines, so the buzz follows the light.
NEON_FRAME = ((0.18, 0.24, 0.8), (0.36, 0.4, 0.5), (0.55, None, 1.0))
NEON_LETTERS = ((0.78, 0.86, 0.7), (0.98, 1.02, 0.45), (1.12, 1.24, 0.9), (1.33, 1.36, 0.5), (1.48, None, 1.0))


def neon_level(timeline, t):
    for start, end, level in timeline:
        if t >= start and (end is None or t < end):
            return level
    return 0.0


def neon_hum_sample(t):
    """The hum of a neon transformer at a time: 100 Hz with its harmonics."""
    return sum(gain * math.sin(2.0 * math.pi * 100.0 * harmonic * t) for harmonic, gain in ((1, 1.0), (2, 0.6), (3, 0.35), (4, 0.2), (6, 0.1)))


def neon_ignite():
    """The neon sign switching on: every flicker of a tube clicks and buzzes (the transformer's hum with a sizzle), as the
    timelines go; once both tubes hold, the buzz settles and fades, and the quiet hum loop goes on."""
    random.seed(10)
    count = int(2.6 * RATE)
    sizzle = Biquad("band", 3200.0, 1.5)
    out = []
    for i in range(count):
        t = i / RATE
        level = max(0.55 * neon_level(NEON_FRAME, t), neon_level(NEON_LETTERS, t))
        settle = 1.0 if t < 1.7 else max(0.0, 1.0 - (t - 1.7) / 0.9)
        out.append(level * settle * (0.35 * neon_hum_sample(t) + 0.5 * sizzle(random.uniform(-1.0, 1.0))))
    for timeline, gain in ((NEON_FRAME, 0.7), (NEON_LETTERS, 1.0)):
        for start, _, level in timeline:
            begin = int(start * RATE)
            click = Biquad("band", 1800.0, 0.8)
            for i in range(int(0.02 * RATE)):
                out[begin + i] += 3.0 * gain * level * click(random.uniform(-1.0, 1.0)) * math.exp(-i / (0.002 * RATE))
    return out


def neon_hum():
    """The neon sign's steady hum in a 2 second loop (a whole number of the hum's periods) with a faint sizzle."""
    random.seed(13)
    loop_length = int(2.0 * RATE)
    total = loop_length + int(0.2 * RATE)
    sizzle = Biquad("band", 3200.0, 1.5)
    out = [0.0] * total
    for i in range(loop_length):
        out[i] += 0.35 * neon_hum_sample(i / RATE)
    for i in range(total):
        out[i] += 0.12 * sizzle(random.uniform(-1.0, 1.0))
    return make_loop(out, loop_length)


def crickets():
    """A night in a 12 second loop: crickets, each chirping at its own pitch and pace (short trains of pulses), over a faint
    low wind."""
    random.seed(11)
    loop_length = int(12.0 * RATE)
    total = loop_length + int(1.5 * RATE)
    out = [0.0] * total
    wind = Biquad("low", 300.0, 0.7)
    brown = 0.0
    for i in range(total):
        brown = 0.99 * brown + random.uniform(-1.0, 1.0) * 0.05
        out[i] += 0.3 * wind(brown)
    for _ in range(6):
        carrier = random.uniform(3900.0, 4900.0)
        pulses = random.randint(2, 4)
        pulse_rate = random.uniform(28.0, 40.0)
        period = random.uniform(0.45, 0.95)
        gain = random.uniform(0.04, 0.1)
        time = random.uniform(0.0, period)
        while time < 12.0:
            for pulse in range(pulses):
                begin = int((time + pulse / pulse_rate) * RATE)
                length = int(0.6 / pulse_rate * RATE)
                for i in range(length):
                    if begin + i < total:
                        out[begin + i] += gain * math.sin(2.0 * math.pi * carrier * i / RATE) * math.sin(math.pi * i / length) ** 2
            time += period * random.uniform(0.9, 1.1)
    return make_loop(reverb(out, mix=0.2, seconds=1.2), loop_length)


def ui_hover():
    """A soft tick as the mouse moves onto a HUD button."""
    count = int(0.05 * RATE)
    return [math.sin(2.0 * math.pi * 2400.0 * i / RATE) * math.exp(-i / (0.008 * RATE)) * min(1.0, i / 8.0) for i in range(count)]


def ui_click():
    """A HUD button's click: a short, woody tock with a bright edge."""
    random.seed(9)
    count = int(0.12 * RATE)
    body = Biquad("band", 1150.0, 6.0)
    out = []
    for i in range(count):
        t = i / RATE
        excitation = random.uniform(-1.0, 1.0) * math.exp(-t / 0.0015)
        tone = math.sin(2.0 * math.pi * 1700.0 * t) * math.exp(-t / 0.02)
        out.append((3.0 * body(excitation) + 0.35 * tone) * min(1.0, 5.0 * (count - i) / count))
    return out


if __name__ == "__main__":
    for name, generate, peak in (("Music.wav", music, 0.8), ("Ambience.wav", ambience, 0.7), ("WoodHit.wav", wood_hit, 0.9),
                                 ("MetalHit.wav", metal_hit, 0.85), ("Throw.wav", throw, 0.7), ("Crackle.wav", crackle, 0.75),
                                 ("Fountain.wav", fountain_water, 0.7), ("Goal.wav", goal_chime, 0.85), ("Cannon.wav", cannon_boom, 0.9),
                                 ("Gate.wav", gate_rumble, 0.8), ("NeonIgnite.wav", neon_ignite, 0.85), ("NeonHum.wav", neon_hum, 0.7),
                                 ("Night.wav", crickets, 0.7), ("UIHover.wav", ui_hover, 0.6), ("UIClick.wav", ui_click, 0.8)):
        write_wav(name, generate(), peak)
        print("wrote", os.path.join(OUTPUT, name))
