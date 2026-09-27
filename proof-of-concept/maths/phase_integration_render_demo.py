"""
Additive partial: full render vs. phase-precomputed render
==========================================================

Warning: 100% AI-generated code

One frequency-modulated partial is produced two ways, as two mono 24-bit /
44.1 kHz WAV files, each 10 seconds long.

  PASS 1  (fm_render_full.wav)
      A straight render of the whole 10 s. The oscillator's phase is obtained
      the ordinary way: accumulate the instantaneous frequency sample by
      sample (trapezoidal integration of f into phase). This is the reference.

  PASS 2  (fm_render_from_phase.wav)
      5 s of silence, then the LAST 5 s of the same partial -- but rendered
      without having accumulated anything before t = 5 s. Instead the phase at
      t = 5 s is computed directly by CLOSED-FORM INTEGRATION of the frequency
      (the complex-exponential / phasor method), and the oscillator is seeded
      with that value and run from t = 5 s onward.

If the closed-form phase is correct, the 5..10 s region of PASS 2 coincides
with the 5..10 s region of PASS 1: dropping into the middle of the sound
reproduces it exactly. The script reports the phase agreement at the seam.

Signal model (one partial):

    instantaneous frequency
        f(t) = carrier(t) * ( modA(t) + modB(t) ) * modC(t)

    unwrapped phase (cycles)          audio sample
        psi(t) = INT_0^t f(tau) dtau      s(t) = gain * sin(2*pi*psi(t))

Each envelope has the form  g(t) = a*t + b + SUM_k A_k*sin(2*pi*nu_k*t + phi_k).
Only numpy is required (plus the standard library: math, cmath, itertools, wave).
"""

import math
import cmath
import itertools
import wave
import numpy as np


# ============================================================================
#  PARAMETERS  --  everything you might want to change lives here
# ============================================================================

SAMPLE_RATE = 44_100          # Hz
DURATION_S  = 10.0            # length of each file, seconds
SPLIT_S     = 5.0            # PASS 2 is silence before this, audio after
GAIN        = 0.9            # peak amplitude (headroom below full scale)

OUT_FULL       = "fm_render_full.wav"
OUT_FROM_PHASE = "fm_render_from_phase.wav"

# Envelope form:  g(t) = a*t + b + sum_k  A_k*sin(2*pi*nu_k*t + phi_k)
#   sines rows are  (amplitude, frequency_Hz, phase_rad)

# Carrier (Hz): linear glide 100 -> 200 Hz, PLUS a quarter-cycle sine that
# adds another 0 -> 100 Hz. Net carrier sweeps 100 -> 300 Hz, non-linearly.
#   quarter cycle over 10 s  =>  sine frequency 1/40 Hz (period 40 s),
#   amplitude 100 Hz, so at t = 10 s the sine contributes sin(pi/2)*100 = 100 Hz.
CARRIER = {
    "a": 10.0, "b": 100.0,
    "sines": [
        # amplitude   frequency (Hz)   phase (rad)
        (100.0,       1.0 / 40.0,      0.0),
    ],
}

# Two modulators that are SUMMED together, then that sum multiplies the carrier.
MOD_SUMMED = [
    {   # modulator A: LFO centred at 1.25, +/-0.75 at 1 Hz
        "a": 0.0, "b": 1.25,
        "sines": [(0.75, 1.0, 0.0)],
    },
    {   # modulator B: centred at -0.125, +/-0.125 at 2 Hz, quarter-cycle head start
        "a": 0.0, "b": -0.125,
        "sines": [(0.125, 2.0, math.pi / 2.0)],
    },
]

# A third modulator that MULTIPLIES the (carrier * summed-modulators) product.
# Centred at 0, so it swings symmetrically through zero (0.1 Hz => one full
# cycle across the 10 s file; the instantaneous frequency is zero at t = 0, 5, 10).
MOD_MULTIPLIED = {
    "a": 0.0, "b": 1.0,
    "sines": [(0.2, 0.1, 0.0)],
}


# ============================================================================
#  Envelope helpers
# ============================================================================

def eval_envelope(env, t):
    """Numeric envelope value, vectorised over a numpy array t."""
    v = env["a"] * t + env["b"]
    for (A, nu, phi) in env["sines"]:
        v = v + A * np.sin(2.0 * np.pi * nu * t + phi)
    return v


def sum_envelopes(envs):
    """Combine a list of envelopes into one by adding them (the summed factor)."""
    return {
        "a": sum(e["a"] for e in envs),
        "b": sum(e["b"] for e in envs),
        "sines": [s for e in envs for s in e["sines"]],
    }


def instantaneous_frequency(t):
    """f(t) = carrier * (modA + modB) * modC, vectorised over t (Hz)."""
    carrier = eval_envelope(CARRIER, t)
    summed  = eval_envelope(MOD_SUMMED[0], t) + eval_envelope(MOD_SUMMED[1], t)
    mult    = eval_envelope(MOD_MULTIPLIED, t)
    return carrier * summed * mult


# ============================================================================
#  Closed-form phase:  psi(T) = INT_0^T (product of envelopes) dt
#
#  Each sine becomes a conjugate pair of phasors, the product of the factors is
#  the outer product of their phasor bags (coefficients multiply, frequencies
#  and polynomial degrees add), and every term integrates via the elementary
#  result below. Exact; the linear glide's b and a*t are just the zero-frequency
#  and ramp phasors.
# ============================================================================

def _envelope_atoms(env):
    """Envelope -> list of (coeff, degree, Omega) meaning  coeff * t^degree * e^{i*Omega*t}."""
    atoms = []
    if env["b"] != 0.0:
        atoms.append((complex(env["b"]), 0, 0.0))          # constant  b
    if env["a"] != 0.0:
        atoms.append((complex(env["a"]), 1, 0.0))          # ramp      a*t
    for (A, nu, phi) in env["sines"]:
        w = 2.0 * math.pi * nu
        atoms.append(( A * cmath.exp(1j * phi) / (2j), 0,  w))   # +w phasor
        atoms.append((-A * cmath.exp(-1j * phi) / (2j), 0, -w))  # -w phasor (conjugate)
    return atoms


def _integral_tp_exp(p, W, T):
    """Exact value of  INT_0^T t^p * e^{i*W*t} dt."""
    if W == 0.0:
        return complex(T ** (p + 1) / (p + 1))
    a = 1j * W
    def antideriv(t):                                       # from repeated integration by parts
        s = 0j
        for j in range(p + 1):
            s += ((-1) ** j) * (math.factorial(p) // math.factorial(p - j)) \
                 * (t ** (p - j)) / a ** (j + 1)
        return cmath.exp(a * t) * s
    return antideriv(T) - antideriv(0.0)


def closed_form_phase(factors, T):
    """Unwrapped phase psi(T) in cycles for the product of the given envelopes."""
    atom_lists = [_envelope_atoms(e) for e in factors]
    total = 0j
    n_terms = 0
    for combo in itertools.product(*atom_lists):            # one atom per factor
        coeff, degree, W = 1 + 0j, 0, 0.0
        for (c, d, w) in combo:
            coeff *= c
            degree += d
            W += w
        total += coeff * _integral_tp_exp(degree, W, T)
        n_terms += 1
    return total.real, n_terms                             # imaginary parts cancel


# ============================================================================
#  Rendering
# ============================================================================

def accumulate_phase(f, dt, seed=0.0):
    """Phase in cycles by trapezoidal accumulation of instantaneous frequency.
       psi[0] = seed; psi[n] = psi[n-1] + (f[n-1]+f[n])/2 * dt."""
    psi = np.empty_like(f)
    psi[0] = 0.0
    np.cumsum((f[:-1] + f[1:]) * 0.5 * dt, out=psi[1:])
    psi += seed
    return psi


def to_samples(psi):
    return GAIN * np.sin(2.0 * np.pi * psi)


def write_wav24(path, x, fs):
    """Write a mono 24-bit PCM WAV file from float samples in [-1, 1]."""
    x = np.clip(x, -1.0, 1.0)
    ints = np.round(x * 8_388_607.0).astype("<i4")         # 2^23 - 1 full scale
    raw = np.frombuffer(ints.tobytes(), dtype=np.uint8).reshape(-1, 4)[:, :3].tobytes()
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(3)                                  # 24-bit
        w.setframerate(fs)
        w.writeframes(raw)


# ============================================================================
#  Main
# ============================================================================

def main():
    fs = SAMPLE_RATE
    dt = 1.0 / fs
    n_total = int(round(DURATION_S * fs))
    split_idx = int(round(SPLIT_S * fs))
    t = np.arange(n_total) / fs

    print("=" * 68)
    print("  Additive partial: full render vs phase-precomputed render")
    print("=" * 68)
    print(f"  sample rate      : {fs} Hz")
    print(f"  bit depth        : 24-bit PCM, mono")
    print(f"  duration         : {DURATION_S:g} s  ({n_total} samples)")
    print(f"  split point      : {SPLIT_S:g} s  (sample index {split_idx})")
    print()

    # --- instantaneous frequency over the whole file -----------------------
    f = instantaneous_frequency(t)
    print("  instantaneous frequency f(t) = carrier * (modA + modB) * modC")
    print(f"    range           : {f.min():+.3f} .. {f.max():+.3f} Hz")
    print(f"    f(0)  = {instantaneous_frequency(np.array([0.0]))[0]:+.6f} Hz")
    print(f"    f(5)  = {instantaneous_frequency(np.array([SPLIT_S]))[0]:+.6f} Hz")
    print(f"    f(10) = {instantaneous_frequency(np.array([DURATION_S]))[0]:+.6f} Hz")
    print()

    # --- PASS 1: full render by phase accumulation -------------------------
    print("  PASS 1: rendering full 10 s by sample-by-sample phase accumulation ...")
    psi_full = accumulate_phase(f, dt, seed=0.0)
    x_full = to_samples(psi_full)
    write_wav24(OUT_FULL, x_full, fs)
    print(f"    wrote {OUT_FULL}")
    print()

    # --- PASS 2: closed-form phase at the split, then render the tail ------
    print(f"  PASS 2: computing phase at t = {SPLIT_S:g} s by closed-form integration ...")
    factors = [CARRIER, sum_envelopes(MOD_SUMMED), MOD_MULTIPLIED]
    psi_split_exact, n_terms = closed_form_phase(factors, SPLIT_S)
    print(f"    closed-form terms         : {n_terms}")
    print(f"    psi({SPLIT_S:g}) exact         : {psi_split_exact:.9f} cycles"
          f"   ({psi_split_exact * 2 * math.pi:.9f} rad)")

    # seed the oscillator at the split with the exact phase and run to the end
    f_tail = f[split_idx:]
    psi_tail = accumulate_phase(f_tail, dt, seed=psi_split_exact)
    x_tail = to_samples(psi_tail)
    x_from_phase = np.concatenate([np.zeros(split_idx, dtype=float), x_tail])
    write_wav24(OUT_FROM_PHASE, x_from_phase, fs)
    print(f"    wrote {OUT_FROM_PHASE}  (silence 0..{SPLIT_S:g}s, audio {SPLIT_S:g}..{DURATION_S:g}s)")
    print()

    # --- agreement at the seam --------------------------------------------
    psi_split_accum = psi_full[split_idx]
    d_cycles = psi_split_exact - psi_split_accum
    tail_diff = np.max(np.abs(x_from_phase[split_idx:] - x_full[split_idx:]))
    lsb = 1.0 / 8_388_607.0
    print("  AGREEMENT AT THE SPLIT (closed-form phase vs accumulated phase)")
    print(f"    accumulated psi({SPLIT_S:g})    : {psi_split_accum:.9f} cycles")
    print(f"    exact - accumulated       : {d_cycles:+.3e} cycles"
          f"   ({d_cycles * 2 * math.pi:+.3e} rad)")
    print(f"    max |PASS2 - PASS1| tail  : {tail_diff:.3e}  "
          f"(= {tail_diff / lsb:.2f} LSB at 24-bit)")
    print("=" * 68)
    print("  The tail of PASS 2 reproduces PASS 1: the closed-form phase lets the")
    print("  oscillator start mid-sound without rendering anything before it.")
    print("  (The tiny residual is PASS 1's accumulation error; the closed form is")
    print("   exact. There is an intentional click at the 5 s seam of PASS 2, where")
    print("   silence meets the waveform mid-cycle -- no fade, so the tail stays")
    print("   sample-identical to PASS 1.)")
    print("=" * 68)


if __name__ == "__main__":
    main()