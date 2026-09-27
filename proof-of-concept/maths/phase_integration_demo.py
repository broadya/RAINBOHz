"""
Phase-integration validation
=============================

WARNING: 100% AI-generated code

Demonstrates that the closed-form integration used to precompute unwrapped
phase is correct, by comparing it against a brute-force numerical integral
that nobody could dispute (the trapezoidal rule with one million samples).

Unwrapped phase is the integral of instantaneous frequency:

    psi(T) = INT_0^T  f_nom(t) * f_mod1(t) * f_mod2(t) * f_mod3(t)  dt

Every frequency envelope (the nominal and each of the three modulators) has
the same form -- a linear glide plus a sum of three sine terms:

    g(t) = a*t + b + SUM_k  A_k * sin(2*pi*nu_k*t + phi_k)

The closed form ("integration"): each sine is written as a pair of complex
exponentials, so the whole product becomes a sum of terms  C * t^p * e^{i*W*t},
and  INT_0^T t^p e^{i*W*t} dt  has an elementary closed form. This is exact and
runs in milliseconds -- it is the formula a renderer would use directly.

The reference ("numerical"): sample f(t) a million times and apply the
trapezoidal rule.
"""

import math
import cmath
import itertools
import numpy as np


# ============================================================================
#  PARAMETERS  --  everything you might want to change is in this block
# ============================================================================

T_END      = 2.0            # integration interval is t in [0, T_END], seconds
N_SAMPLES  = 1_000_000      # samples for the numerical (trapezoidal) reference

# Each envelope:  g(t) = a*t + b + sum_k A_k * sin(2*pi*nu_k*t + phi_k)
#   'a', 'b'   : the linear glide (slope in Hz/s or 1/s, offset in Hz or 1)
#   'sines'    : rows of  (amplitude, frequency_Hz, phase_rad)
#
# Frequencies are sqrt(distinct primes), so every pair of them is
# incommensurate: none is a rational multiple of another, hence no harmonic
# relationships anywhere. Every linear term differs; every phase differs.

NOMINAL = {                         # nominal frequency, in Hz (carrier ~220 Hz)
    "a": 3.70, "b": 220.0,
    "sines": [
        # amplitude   frequency (Hz)   phase (rad)
        (4.000,       math.sqrt(2),    0.100),
        (3.000,       math.sqrt(3),    1.300),
        (2.000,       math.sqrt(5),    2.700),
    ],
}

MODULATORS = [
    {                               # modulator 1 (dimensionless, ~1.0)
        "a": 0.110, "b": 1.000,
        "sines": [
            (0.030,   math.sqrt(7),    0.300),
            (0.020,   math.sqrt(11),   0.900),
            (0.010,   math.sqrt(13),   1.500),
        ],
    },
    {                               # modulator 2 (dimensionless, ~0.97)
        "a": -0.070, "b": 0.970,
        "sines": [
            (0.025,   math.sqrt(17),   0.500),
            (0.015,   math.sqrt(19),   1.100),
            (0.010,   math.sqrt(23),   2.100),
        ],
    },
    {                               # modulator 3 (dimensionless, ~1.04)
        "a": 0.050, "b": 1.040,
        "sines": [
            (0.020,   math.sqrt(29),   0.700),
            (0.015,   math.sqrt(31),   1.700),
            (0.010,   math.sqrt(37),   2.500),
        ],
    },
]

ENVELOPES = [NOMINAL] + MODULATORS


# ============================================================================
#  RESULT BY INTEGRATION  (exact closed form)
# ============================================================================

def _envelope_atoms(env):
    """Rewrite an envelope as a list of (coeff, degree, W) terms  C*t^degree*e^{i W t}.
       Linear glide -> two real terms; each sine -> two complex-exponential terms."""
    atoms = [(complex(env["b"]), 0, 0.0),           # constant  b
             (complex(env["a"]), 1, 0.0)]           # slope     a*t
    for (A, nu, phi) in env["sines"]:
        w = 2.0 * math.pi * nu
        atoms.append(( A * cmath.exp(1j * phi) / (2j), 0,  w))   # +w component
        atoms.append((-A * cmath.exp(-1j * phi) / (2j), 0, -w))  # -w component
    return atoms


def _integral_tp_exp(p, W, T):
    """Exact value of  INT_0^T t^p e^{i W t} dt."""
    if W == 0.0:
        return complex(T ** (p + 1) / (p + 1))
    a = 1j * W
    def antideriv(t):                               # e^{a t} * sum_j ... (from parts)
        s = 0j
        for j in range(p + 1):
            s += ((-1) ** j) * (math.factorial(p) // math.factorial(p - j)) \
                 * (t ** (p - j)) / a ** (j + 1)
        return cmath.exp(a * t) * s
    return antideriv(T) - antideriv(0.0)


def integrate_closed_form():
    """Phase (in cycles) via the exact closed-form integral of the product."""
    atom_lists = [_envelope_atoms(e) for e in ENVELOPES]
    total = 0j
    for combo in itertools.product(*atom_lists):    # one term from each envelope
        coeff, degree, W = 1 + 0j, 0, 0.0
        for (c, d, w) in combo:
            coeff *= c
            degree += d
            W += w
        total += coeff * _integral_tp_exp(degree, W, T_END)
    return total.real                               # imaginary parts cancel


# ============================================================================
#  RESULT BY NUMERICAL METHOD  (trapezoidal rule, N_SAMPLES points)
# ============================================================================

def _eval_envelope(env, t):
    v = env["a"] * t + env["b"]
    for (A, nu, phi) in env["sines"]:
        v = v + A * np.sin(2.0 * np.pi * nu * t + phi)
    return v


def integrate_trapezoidal():
    t = np.linspace(0.0, T_END, N_SAMPLES)
    f = np.ones_like(t)
    for env in ENVELOPES:
        f = f * _eval_envelope(env, t)
    return float(np.trapezoid(f, t))


# ============================================================================
#  REPORT
# ============================================================================

def _print_envelope(label, unit, env):
    print(f"  {label:<16}  a = {env['a']:+8.4f}   b = {env['b']:+9.4f}     ({unit})")
    for (A, nu, phi) in env["sines"]:
        print(f"  {'':<16}  A = {A:7.4f}   nu = {nu:9.6f} Hz   phi = {phi:5.3f} rad")


def main():
    bar = "=" * 76
    print(bar)
    print("  UNWRAPPED PHASE:  CLOSED-FORM INTEGRATION  vs  NUMERICAL INTEGRATION")
    print(bar)
    print()
    print("  psi(T) = INT_0^T  f_nom(t) . f_mod1(t) . f_mod2(t) . f_mod3(t)  dt")
    print("  each envelope:  g(t) = a.t + b + SUM_k A_k . sin(2.pi.nu_k.t + phi_k)")
    print()

    # ---- parameters ------------------------------------------------------
    print("-" * 76)
    print("  PARAMETERS")
    print("-" * 76)
    print(f"  Integration interval :  t in [0, {T_END:g}] s")
    print(f"  Numerical resolution :  {N_SAMPLES:,} samples (trapezoidal rule)")
    print()
    _print_envelope("f_nom  [Hz]", "nominal frequency", NOMINAL)
    for i, m in enumerate(MODULATORS, start=1):
        _print_envelope(f"f_mod{i} [x]", "modulator", m)
    print()
    print("  The nominal envelope is the carrier pitch (~220 Hz) with a linear")
    print("  glide and three vibrato-like sine terms. Each modulator is a")
    print("  dimensionless factor near unity that scales the nominal frequency,")
    print("  again a linear drift plus three sine terms. The twelve sine")
    print("  frequencies are sqrt(distinct primes): pairwise incommensurate, so")
    print("  there are no harmonic relationships. All linear terms differ and")
    print("  every sine begins at a different phase.")
    print()

    # ---- results ---------------------------------------------------------
    exact = integrate_closed_form()
    numer = integrate_trapezoidal()

    print("-" * 76)
    print("  RESULT BY INTEGRATION   (exact closed form)")
    print("-" * 76)
    print(f"  psi = {exact:.10f} cycles   = {exact * 2 * math.pi:.10f} rad")
    print()
    print("-" * 76)
    print(f"  RESULT BY NUMERICAL METHOD   (trapezoidal, {N_SAMPLES:,} samples)")
    print("-" * 76)
    print(f"  psi = {numer:.10f} cycles   = {numer * 2 * math.pi:.10f} rad")
    print()

    # ---- difference ------------------------------------------------------
    abs_diff = abs(exact - numer)
    pct_diff = 100.0 * abs_diff / abs(exact)
    print("-" * 76)
    print("  DIFFERENCE:  INTEGRATION  vs  NUMERICAL")
    print("-" * 76)
    print(f"  absolute      :  {abs_diff:.3e} cycles")
    print(f"  percentage    :  {pct_diff:.3e} %")
    print(bar)


if __name__ == "__main__":
    main()