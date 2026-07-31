#!/usr/bin/env python3
"""
Compare an original WAV to a decoded WAV and report residual distortion.

For complex programme (music/speech), classic harmonic THD is not well-defined
(no single fundamental).  This script delay-aligns the pair and measures a
THD+N-style error-to-signal ratio:

    distortion = RMS(decoded_aligned - original) / RMS(original)

Reported as a percentage (and dB).  Sliding windows give average and peak.

Usage (from repo root, with tools/.venv):

  source tools/.venv/bin/activate
  python tools/anog_thd_compare.py \\
      "Tests/TestData/music/01 Stay On These Roads.wav" \\
      "Tests/TestData/music/01 Stay On These Roads_decoded.wav"

  # defaults to those paths if present
  python tools/anog_thd_compare.py
"""

from __future__ import annotations

import argparse
import math
import sys
import wave
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[1]
DEFAULT_ORIG = REPO / "Tests/TestData/music/01 Stay On These Roads.wav"
DEFAULT_DEC = REPO / "Tests/TestData/music/01 Stay On These Roads_decoded.wav"


def load_wav(path: Path) -> tuple[np.ndarray, int]:
    """Return float64 samples shape (nframes, nch) in ~[-1, 1], and sample rate."""
    with wave.open(str(path), "rb") as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        rate = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)
    if sw == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif sw == 4:
        as_f = np.frombuffer(raw, dtype="<f4").astype(np.float64)
        if np.max(np.abs(as_f)) <= 8.0:
            data = as_f
        else:
            data = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise SystemExit(f"{path}: unsupported sample width {sw}")
    return data.reshape(-1, nch), rate


def find_lag(ref: np.ndarray, test: np.ndarray, max_lag: int) -> int:
    """
    Lag of `test` relative to `ref`: positive means test is delayed
    (test[t + lag] ≈ ref[t]).

    Uses coarse-to-fine residual minimisation on a mid mono segment — more
    reliable than raw cross-correlation for codec-delayed programme material.
    """
    a = ref.mean(axis=1)
    b = test.mean(axis=1)
    n = min(len(a), len(b))
    max_lag = min(max_lag, n // 4)
    seg = min(n // 4, 5 * 48000)
    start = min(n // 5, max(0, n - seg - max_lag - 1))

    def residual(lag: int) -> float:
        if lag >= 0:
            r = a[start : start + seg]
            t = b[start + lag : start + lag + seg]
        else:
            r = a[start - lag : start - lag + seg]
            t = b[start : start + seg]
        if len(r) != seg or len(t) != seg:
            return float("inf")
        return float(np.mean((t - r) ** 2))

    # Coarse search (both directions)
    coarse_step = max(1, min(32, max_lag // 256 or 1))
    best_lag = 0
    best_err = residual(0)
    for lag in range(-max_lag, max_lag + 1, coarse_step):
        err = residual(lag)
        if err < best_err:
            best_err = err
            best_lag = lag

    # Fine search around best
    lo = max(-max_lag, best_lag - coarse_step)
    hi = min(max_lag, best_lag + coarse_step)
    for lag in range(lo, hi + 1):
        err = residual(lag)
        if err < best_err:
            best_err = err
            best_lag = lag
    return best_lag


def align(ref: np.ndarray, test: np.ndarray, lag: int) -> tuple[np.ndarray, np.ndarray]:
    if lag >= 0:
        t = test[lag:]
        r = ref[: len(t)]
        t = t[: len(r)]
    else:
        r = ref[-lag:]
        t = test[: len(r)]
        r = r[: len(t)]
    return r, t


def window_ratios(ref: np.ndarray, test: np.ndarray, win: int, hop: int) -> np.ndarray:
    r = ref.mean(axis=1)
    t = test.mean(axis=1)
    ratios = []
    for start in range(0, len(r) - win + 1, hop):
        seg_r = r[start : start + win]
        seg_t = t[start : start + win]
        rms_r = math.sqrt(float(np.mean(seg_r * seg_r)))
        # Skip quiet windows (avoid huge ratios / 100% spikes in fades)
        if rms_r < 1e-3:
            continue
        err = seg_t - seg_r
        rms_e = math.sqrt(float(np.mean(err * err)))
        ratios.append(rms_e / rms_r)
    return np.asarray(ratios, dtype=np.float64)


def pct(x: float) -> str:
    return f"{100.0 * x:.4f}%"


def db(x: float) -> str:
    return f"{20.0 * math.log10(x):.2f} dB" if x > 0 else "-inf dB"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="ANOG original vs decoded residual distortion (THD+N-style)"
    )
    ap.add_argument("original", nargs="?", type=Path, default=None)
    ap.add_argument("decoded", nargs="?", type=Path, default=None)
    ap.add_argument("--window-ms", type=float, default=100.0)
    ap.add_argument("--hop-ms", type=float, default=50.0)
    ap.add_argument(
        "--max-lag-ms",
        type=float,
        default=2000.0,
        help="max alignment search in ms (default 2000; codec FIR delay can be large)",
    )
    args = ap.parse_args()

    orig_path = args.original or DEFAULT_ORIG
    dec_path = args.decoded or DEFAULT_DEC
    if not orig_path.is_file():
        print(f"Original not found: {orig_path}", file=sys.stderr)
        return 1
    if not dec_path.is_file():
        print(f"Decoded not found: {dec_path}", file=sys.stderr)
        return 1

    ref, rate_a = load_wav(orig_path)
    test, rate_b = load_wav(dec_path)
    if rate_a != rate_b:
        print(f"Sample rate mismatch: {rate_a} vs {rate_b}", file=sys.stderr)
        return 1
    if ref.shape[1] != test.shape[1]:
        print(f"Channel mismatch: {ref.shape[1]} vs {test.shape[1]}", file=sys.stderr)
        return 1

    max_lag = int(args.max_lag_ms * rate_a / 1000.0)
    max_lag = min(max_lag, min(len(ref), len(test)) // 4)
    lag = find_lag(ref, test, max_lag)

    r, t = align(ref, test, lag)
    err = t - r
    rms_r = math.sqrt(float(np.mean(r * r)))
    rms_e = math.sqrt(float(np.mean(err * err)))
    overall = rms_e / rms_r if rms_r > 0 else float("nan")

    win = max(1, int(args.window_ms * rate_a / 1000.0))
    hop = max(1, int(args.hop_ms * rate_a / 1000.0))
    ratios = window_ratios(r, t, win, hop)

    print(f"Original: {orig_path}")
    print(f"Decoded:  {dec_path}")
    print(
        f"Rate: {rate_a} Hz  channels: {ref.shape[1]}  "
        f"aligned: {len(r)} samples ({len(r) / rate_a:.2f} s)"
    )
    print(f"Best delay (decoded lags original): {lag} samples ({1000.0 * lag / rate_a:.2f} ms)")
    print()
    print("Residual distortion  RMS(error) / RMS(original)")
    print("  (THD+N-style for programme material — not classic single-tone harmonic THD)")
    print(f"  Overall:  {pct(overall)}   ({db(overall)})")
    if len(ratios):
        avg = float(np.mean(ratios))
        peak = float(np.max(ratios))
        med = float(np.median(ratios))
        print(f"  Window:   {args.window_ms:g} ms, hop {args.hop_ms:g} ms ({len(ratios)} windows)")
        print(f"  Average:  {pct(avg)}   ({db(avg)})")
        print(f"  Peak:     {pct(peak)}   ({db(peak)})")
        print(f"  p99:      {pct(float(np.percentile(ratios, 99)))}   ({db(float(np.percentile(ratios, 99)))})")
        print(f"  Median:   {pct(med)}   ({db(med)})")
    else:
        print("  No active windows (signal too quiet?)")

    print()
    print("Per-channel overall:")
    for c in range(ref.shape[1]):
        rc, tc = r[:, c], t[:, c]
        rr = math.sqrt(float(np.mean(rc * rc)))
        ee = math.sqrt(float(np.mean((tc - rc) ** 2)))
        d = ee / rr if rr > 0 else float("nan")
        print(f"  CH{c}: {pct(d)}   ({db(d)})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
