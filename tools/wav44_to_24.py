#!/usr/bin/env python3
"""
High-quality 44.1 kHz → 24 kHz PCM16 WAV converter for Amy jukebox libraries.

Rational resample 80:147 (44100×80/147 = 24000) via polyphase FIR
(scipy.signal.resample_poly). Default mixes stereo→mono for the VOIP bed;
use --stereo to keep channels.

Usage (from Audio-Codecs repo root, with tools/.venv):

  source tools/.venv/bin/activate
  python tools/wav44_to_24.py input.wav [output.wav]
  python tools/wav44_to_24.py -all                 # cwd *.wav → ./WAV24/
  python tools/wav44_to_24.py -all -r              # recurse; mirror under WAV24/
  python tools/wav44_to_24.py /path/to/CD --out-root /music/WAV24 -r

CD-AUDIO style names (NN Title.wav) are preserved.

After batch convert, run album_loudness.py on the WAV24 album dirs (see tools/album_loudness.py).
"""

from __future__ import annotations

import argparse
import sys
import wave
from pathlib import Path

import numpy as np

try:
    from scipy.signal import resample_poly
except ImportError:
    sys.exit("scipy required — source tools/.venv/bin/activate first")

# 44100 / 24000 = 147 / 80
UPSAMPLE = 80
DOWNSAMPLE = 147
RATE_IN = 44100
RATE_OUT = 24000


def load_wav(path: Path) -> tuple[np.ndarray, int, int]:
    """Return float64 (nframes, nch) in [-1,1], sample rate, sample width bytes."""
    with wave.open(str(path), "rb") as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        rate = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)
    if sw != 2:
        raise SystemExit(f"{path}: only 16-bit PCM supported (got {8 * sw}-bit)")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float64).reshape(-1, nch) / 32768.0
    return data, rate, sw


def save_wav_s16(path: Path, samples: np.ndarray, rate: int) -> None:
    """samples: float64 (nframes, nch) in ~[-1,1]."""
    path.parent.mkdir(parents=True, exist_ok=True)
    x = np.clip(np.round(samples * 32767.0), -32768, 32767).astype("<i2")
    nch = x.shape[1]
    with wave.open(str(path), "wb") as w:
        w.setnchannels(nch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(x.reshape(-1).tobytes())


def to_mono(x: np.ndarray) -> np.ndarray:
    if x.shape[1] == 1:
        return x
    return x.mean(axis=1, keepdims=True)


def resample_44_to_24(x: np.ndarray) -> np.ndarray:
    """Per-channel polyphase resample 44100 → 24000."""
    outs = []
    for c in range(x.shape[1]):
        y = resample_poly(x[:, c], UPSAMPLE, DOWNSAMPLE)
        outs.append(y)
    # Align lengths (should already match)
    n = min(len(o) for o in outs)
    return np.column_stack([o[:n] for o in outs])


def convert_file(src: Path, dst: Path, *, keep_stereo: bool, overwrite: bool) -> None:
    if dst.exists() and not overwrite:
        ans = input(f"Overwrite {dst}? [y/N] ").strip().lower()
        if ans not in ("y", "yes"):
            print(f"skip {dst}")
            return
    data, rate, _ = load_wav(src)
    if rate != RATE_IN:
        raise SystemExit(f"{src}: expected {RATE_IN} Hz, got {rate}")
    if not keep_stereo:
        data = to_mono(data)
    out = resample_44_to_24(data)
    save_wav_s16(dst, out, RATE_OUT)
    dur_in = data.shape[0] / RATE_IN
    dur_out = out.shape[0] / RATE_OUT
    print(
        f"{src.name} → {dst}  "
        f"{data.shape[1]}ch@{RATE_IN} → {out.shape[1]}ch@{RATE_OUT}  "
        f"{dur_in:.2f}s→{dur_out:.2f}s"
    )


def iter_wavs(root: Path, recursive: bool) -> list[Path]:
    if recursive:
        return sorted(p for p in root.rglob("*.wav") if p.is_file())
    return sorted(p for p in root.glob("*.wav") if p.is_file())


def out_path_for(src: Path, src_root: Path, out_root: Path) -> Path:
    rel = src.relative_to(src_root)
    return out_root / rel


def main() -> int:
    ap = argparse.ArgumentParser(
        description="44.1 kHz PCM16 WAV → 24 kHz PCM16 (jukebox library prep)"
    )
    ap.add_argument("input", nargs="?", type=Path, help="input WAV or directory")
    ap.add_argument("output", nargs="?", type=Path, help="output WAV (single-file mode)")
    ap.add_argument(
        "-all",
        action="store_true",
        help="convert all WAVs in input dir (default cwd) into --out-root",
    )
    ap.add_argument(
        "-r",
        action="store_true",
        help="with -all: recurse; mirror folder tree under --out-root",
    )
    ap.add_argument(
        "--out-root",
        type=Path,
        default=Path("WAV24"),
        help="batch output root (default ./WAV24)",
    )
    ap.add_argument(
        "--stereo",
        action="store_true",
        help="keep stereo (default: mixdown to mono for jukebox bed)",
    )
    ap.add_argument("-y", "--yes", action="store_true", help="overwrite without prompting")
    args = ap.parse_args()

    keep_stereo = args.stereo

    if args.all:
        src_root = (args.input or Path(".")).resolve()
        if not src_root.is_dir():
            print(f"not a directory: {src_root}", file=sys.stderr)
            return 1
        out_root = args.out_root.resolve()
        # Skip walking into the output tree if it sits under src_root
        wavs = []
        for p in iter_wavs(src_root, args.r):
            try:
                p.resolve().relative_to(out_root)
                continue  # inside out_root
            except ValueError:
                wavs.append(p)
        if not wavs:
            print("no .wav files found", file=sys.stderr)
            return 1
        for src in wavs:
            dst = out_path_for(src, src_root, out_root)
            convert_file(src, dst, keep_stereo=keep_stereo, overwrite=args.yes)
        print(f"done: {len(wavs)} file(s) → {out_root}")
        return 0

    if args.input is None:
        ap.print_help()
        return 2

    src = args.input.resolve()
    if not src.is_file():
        print(f"not a file: {src} (use -all for directories)", file=sys.stderr)
        return 1
    if args.output is not None:
        dst = args.output.resolve()
    else:
        dst = src.with_name(src.stem + "_24k.wav")
    convert_file(src, dst, keep_stereo=keep_stereo, overwrite=args.yes)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        pass
