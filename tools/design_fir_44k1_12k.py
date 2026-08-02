#!/usr/bin/env python3
"""
Design a Parks-McClellan (remez) prototype FIR for 44.1 kHz <-> 12 kHz
rational resampling (exact ratio 40/147).

Mirrors the design procedure documented in AudioCodecFilter_4X_PolyphaseFIR.hpp:

  Method          : Parks-McClellan equiripple (remez)
  Passband edge   : 5500 Hz
  Stopband edge   : 6500 Hz   (first alias zone of 12 kHz: 12000 - 5500)
  Stopband atten  : >= 65 dB
  Passband ripple : ~0.25 dB class (Pareto trade-off with stopband)

The common intermediate rate is L * 44100 = M * 12000 = 1_764_000 Hz
with L=40, M=147.  One prototype at that rate serves both directions:

  encode: interp 40 / decimate 147  ->  40 polyphase branches
  decode: interp 147 / decimate 40  -> 147 polyphase branches

N is chosen as a multiple of lcm(40,147)=5880 so both decompositions are even.

Usage (from repo root, with tools/.venv active):

  python tools/design_fir_44k1_12k.py
  python tools/design_fir_44k1_12k.py --emit-header
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np
from scipy import signal

FS_IN = 44100
FS_OUT = 12000
L = 40   # interpolation factor on encode (44.1 -> intermediate)
M = 147  # decimation factor on encode
FS_INTERP = L * FS_IN  # == M * FS_OUT == 1_764_000

F_PASS = 5500.0
F_STOP = 6500.0
TARGET_STOP_DB = 65.0
TARGET_PASS_DB = 0.25  # soft target; document measured value

# Smallest N that decomposes evenly into both polyphase banks
LCM_LM = math.lcm(L, M)  # 5880


def design_remez(num_taps: int, stop_weight: float) -> np.ndarray:
    """Linear-phase lowpass at FS_INTERP; gain = L for interpolation identity."""
    bands = [0.0, F_PASS, F_STOP, FS_INTERP / 2.0]
    desired = [1.0, 0.0]
    weight = [1.0, stop_weight]
    h = signal.remez(num_taps, bands, desired, fs=FS_INTERP, weight=weight, maxiter=200)
    # Normalise passband gain then scale by L (noble-identity interpolator gain)
    w_test = np.array([0.0])
    _, H0 = signal.freqz(h, worN=w_test, fs=FS_INTERP)
    h = h / np.abs(H0[0]) * float(L)
    return h.astype(np.float64)


def measure_response(h: np.ndarray) -> dict:
    n_fft = 1 << int(np.ceil(np.log2(max(len(h) * 8, 65536))))
    w, H = signal.freqz(h, worN=n_fft, fs=FS_INTERP)
    mag_db = 20.0 * np.log10(np.maximum(np.abs(H) / float(L), 1e-15))

    pass_mask = w <= F_PASS
    stop_mask = w >= F_STOP
    pass_ripple = float(np.max(mag_db[pass_mask]) - np.min(mag_db[pass_mask]))
    # Peak absolute deviation from 0 dB in passband
    pass_dev = float(np.max(np.abs(mag_db[pass_mask])))
    stop_atten = float(-np.max(mag_db[stop_mask]))  # positive dB of attenuation
    return {
        "pass_ripple_db": pass_ripple,
        "pass_dev_db": pass_dev,
        "stop_atten_db": stop_atten,
        "n_fft": n_fft,
    }


def meets_spec(meas: dict) -> bool:
    return meas["stop_atten_db"] >= TARGET_STOP_DB and meas["pass_dev_db"] <= TARGET_PASS_DB * 2.0


def find_design() -> tuple[np.ndarray, dict, float, int]:
    """
    Search stopband weight and N (multiples of LCM) for a design that meets
    stopband atten while keeping passband deviation in the ~0.25 dB class.
    """
    best = None
    # Prefer smallest N; try a few stop weights (higher -> more stop atten, more pass ripple)
    for mult in range(1, 4):
        num_taps = LCM_LM * mult
        if num_taps % 2 == 0:
            # remez often prefers odd length for Type I; keep even for phase symmetry of banks
            # Parks-McClellan supports even length (Type II) for lowpass — OK.
            pass
        for stop_weight in (10.0, 20.0, 40.0, 80.0, 160.0):
            print(f"Trying N={num_taps}, stop_weight={stop_weight} ...", flush=True)
            h = design_remez(num_taps, stop_weight)
            meas = measure_response(h)
            print(
                f"  pass_dev={meas['pass_dev_db']:.3f} dB  "
                f"pass_ripple={meas['pass_ripple_db']:.3f} dB  "
                f"stop_atten={meas['stop_atten_db']:.1f} dB",
                flush=True,
            )
            score = (
                0 if meets_spec(meas) else 1,
                num_taps,
                abs(meas["stop_atten_db"] - TARGET_STOP_DB),
                meas["pass_dev_db"],
            )
            cand = (h, meas, stop_weight, num_taps)
            if best is None or score < best[0]:
                best = (score, cand)
            if meets_spec(meas):
                return cand
    assert best is not None
    print("WARNING: no design met soft targets; using best candidate.", flush=True)
    return best[1]


def polyphase_decompose(h: np.ndarray, num_phases: int) -> np.ndarray:
    """
    Decompose prototype into num_phases branches.
    phase[p][k] = h[k * num_phases + p], zero-padded if needed.
    """
    n = len(h)
    taps_per = (n + num_phases - 1) // num_phases
    padded = np.zeros(taps_per * num_phases, dtype=np.float64)
    padded[:n] = h
    phases = np.zeros((num_phases, taps_per), dtype=np.float64)
    for p in range(num_phases):
        phases[p] = padded[p::num_phases]
    return phases


def fmt_coef(x: float) -> str:
    return f"{x:+.10f}f"


def emit_cpp_tables(enc: np.ndarray, dec: np.ndarray) -> str:
    lines = []
    lines.append(f"// Auto-generated by tools/design_fir_44k1_12k.py — do not hand-edit.")
    lines.append(f"static constexpr int POLY_L = {L};")
    lines.append(f"static constexpr int POLY_M = {M};")
    lines.append(f"static constexpr int POLY_ENC_PHASES = {L};")
    lines.append(f"static constexpr int POLY_ENC_TAPS = {enc.shape[1]};")
    lines.append(f"static constexpr int POLY_DEC_PHASES = {M};")
    lines.append(f"static constexpr int POLY_DEC_TAPS = {dec.shape[1]};")
    lines.append("")
    lines.append(
        f"static const float poly_enc[POLY_ENC_PHASES][POLY_ENC_TAPS] = {{"
    )
    for p in range(enc.shape[0]):
        lines.append(f"    {{  /* encode phase {p} */")
        row = ",\n".join(
            "        " + ", ".join(fmt_coef(enc[p, j]) for j in range(i, min(i + 4, enc.shape[1])))
            for i in range(0, enc.shape[1], 4)
        )
        lines.append(row)
        lines.append("    }," if p + 1 < enc.shape[0] else "    }")
    lines.append("};")
    lines.append("")
    lines.append(
        f"static const float poly_dec[POLY_DEC_PHASES][POLY_DEC_TAPS] = {{"
    )
    for p in range(dec.shape[0]):
        lines.append(f"    {{  /* decode phase {p} */")
        row = ",\n".join(
            "        " + ", ".join(fmt_coef(dec[p, j]) for j in range(i, min(i + 4, dec.shape[1])))
            for i in range(0, dec.shape[1], 4)
        )
        lines.append(row)
        lines.append("    }," if p + 1 < dec.shape[0] else "    }")
    lines.append("};")
    return "\n".join(lines)


def comment_block(meas: dict, num_taps: int, stop_weight: float) -> str:
    return f"""/**
 *
 * Polyphase FIR rational resampler — 44.1 kHz <-> 12 kHz (40:147)
 *
 * Design
 * ------
 *   Method             : Parks-McClellan equiripple (remez)
 *   Prototype rate     : {FS_INTERP} Hz  (= 40 * 44100 = 147 * 12000)
 *   Total taps         : {num_taps}  (linear-phase)
 *   Encode phases (L)  : {L}   taps/phase : {num_taps // L}
 *   Decode phases (M)  : {M}  taps/phase : {num_taps // M}
 *   Remez stop weight  : {stop_weight:g}
 *
 * Measured frequency response (prototype / L, relative to 0 dB passband)
 * ---------------------------
 *   Passband edge      : {F_PASS:.0f} Hz
 *   Passband deviation : {meas['pass_dev_db']:.3f} dB
 *   Passband ripple    : {meas['pass_ripple_db']:.3f} dB
 *   Transition band    : {F_PASS:.0f} - {F_STOP:.0f} Hz
 *   Stopband edge      : {F_STOP:.0f} Hz
 *   Stopband atten     : {meas['stop_atten_db']:.1f} dB
 *
 * Trade-off note
 * --------------
 * Same absolute pass/stop edges as the 48 kHz 4:1 design.  The stopband
 * edge (6500 Hz) coincides with the start of the first alias zone of the
 * 12 kHz domain (fs_out - f_pass = 12000 - 5500 = 6500 Hz).
 *
 * Regenerate coefficients:
 *   source tools/.venv/bin/activate && python tools/design_fir_44k1_12k.py --emit-header
 *
 */"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--emit-header",
        action="store_true",
        help="Write Codecs/Filters/AudioCodecFilter_44100_12000_PolyphaseFIR_coeffs.inc",
    )
    ap.add_argument(
        "--n",
        type=int,
        default=0,
        help="Force tap count (multiple of 5880). 0 = search.",
    )
    ap.add_argument(
        "--stop-weight",
        type=float,
        default=0.0,
        help="Force remez stopband weight. 0 = search.",
    )
    args = ap.parse_args()

    if args.n and args.stop_weight:
        h = design_remez(args.n, args.stop_weight)
        meas = measure_response(h)
        stop_weight = args.stop_weight
        num_taps = args.n
    else:
        h, meas, stop_weight, num_taps = find_design()

    print("\n=== Selected design ===")
    print(f"N={num_taps}, stop_weight={stop_weight}")
    print(f"pass_dev={meas['pass_dev_db']:.3f} dB, stop_atten={meas['stop_atten_db']:.1f} dB")
    print(comment_block(meas, num_taps, stop_weight))

    enc = polyphase_decompose(h, L)
    # Same prototype is reused for decode (interp M / decimate L).  Remez
    # scaled h by L for the encode interpolator; decode needs gain M.
    dec = polyphase_decompose(h * (float(M) / float(L)), M)
    assert enc.shape[0] == L and dec.shape[0] == M
    print(f"encode bank: {enc.shape[0]} x {enc.shape[1]}")
    print(f"decode bank: {dec.shape[0]} x {dec.shape[1]}")

    if args.emit_header:
        out = Path(__file__).resolve().parents[1] / "Codecs" / "Filters" / "AudioCodecFilter_44100_12000_PolyphaseFIR_coeffs.inc"
        meta = Path(__file__).resolve().parents[1] / "Codecs" / "Filters" / "AudioCodecFilter_44100_12000_PolyphaseFIR_design.txt"
        out.write_text(emit_cpp_tables(enc, dec) + "\n")
        meta.write_text(comment_block(meas, num_taps, stop_weight) + "\n")
        print(f"Wrote {out}")
        print(f"Wrote {meta}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
