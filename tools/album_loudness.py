#!/usr/bin/env python3
"""
Measure album loudness (BS.1770 via ffmpeg ebur128) and write loudness.json
sidecars for Amy jukebox WAV24 albums.

Non-destructive: originals untouched; Client applies default_gain_db + trim_db
in the mix path.

Policy (J9):
  target ≈ -16 LUFS, true-peak ceiling -1 dBTP
  default_gain_db = min(target - integrated, ceiling - true_peak)

Usage (ffmpeg required: apt install ffmpeg):

  python tools/album_loudness.py /path/to/WAV24/Artist/Album
  python tools/album_loudness.py /path/to/WAV24 -all -r
  python tools/album_loudness.py /path/to/WAV24/R.E.M./Monster --dry-run

Preserves existing trim_db on rescan.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

TARGET_LUFS = -16.0
PEAK_CEILING_DBTP = -1.0
SIDECAR_NAME = "loudness.json"
FORMAT_LABEL = "WAV24"

# ffmpeg ebur128 summary lines (stderr), e.g.:
#   I:         -14.2 LUFS
#   LRA:         7.8 LU
#   Peak:       -1.02 dBFS
RE_I = re.compile(r"^\s*I:\s*([-\d.]+)\s*LUFS", re.I | re.M)
RE_LRA = re.compile(r"^\s*LRA:\s*([-\d.]+)\s*LU", re.I | re.M)
RE_PEAK = re.compile(r"^\s*Peak:\s*([-\d.]+)\s*dB", re.I | re.M)
# True peak when -tp true is available:
RE_TP = re.compile(r"^\s*True peak:\s*([-\d.]+)\s*dB", re.I | re.M)
RE_TP_ALT = re.compile(r"True peak:\s*([-\d.]+)", re.I)


def track_sort_key(p: Path) -> tuple:
    """Sort CD-AUDIO style NN Title.wav (and D-NN) like the jukebox catalog."""
    name = p.stem
    m = re.match(r"^(?:(\d+)-)?(\d+)\s+", name)
    if m:
        disc = int(m.group(1) or 0)
        num = int(m.group(2))
        return (disc, num, name.lower())
    return (9999, 9999, name.lower())


def list_album_wavs(album_dir: Path) -> list[Path]:
    wavs = [p for p in album_dir.iterdir() if p.is_file() and p.suffix.lower() == ".wav"]
    return sorted(wavs, key=track_sort_key)


def find_albums(root: Path, recursive: bool) -> list[Path]:
    """Album dirs are folders that contain at least one .wav (not nested under another)."""
    root = root.resolve()
    if not recursive:
        if list_album_wavs(root):
            return [root]
        # One level: Artist/Album
        out = []
        for artist in sorted(p for p in root.iterdir() if p.is_dir() and not p.name.startswith(".")):
            for album in sorted(p for p in artist.iterdir() if p.is_dir() and not p.name.startswith(".")):
                if list_album_wavs(album):
                    out.append(album)
        return out

    albums: list[Path] = []
    for p in sorted(root.rglob("*")):
        if not p.is_dir() or p.name.startswith("."):
            continue
        if list_album_wavs(p):
            albums.append(p)
    return albums


def run_ebur128(paths: list[Path]) -> tuple[float, float | None, float]:
    """
    Measure integrated LUFS, LRA (or None), and true-peak (dBTP / peak dBFS)
    for one or more WAVs concatenated in order via ffmpeg concat demuxer.
    """
    if not paths:
        raise ValueError("no tracks")
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("ffmpeg not found — apt install ffmpeg")

    with tempfile.TemporaryDirectory(prefix="album_loudness_") as tmp:
        tmp_path = Path(tmp)
        list_file = tmp_path / "concat.txt"
        # Absolute paths; escape single quotes for concat demuxer
        lines = []
        for p in paths:
            ap = str(p.resolve()).replace("'", "'\\''")
            lines.append(f"file '{ap}'")
        list_file.write_text("\n".join(lines) + "\n", encoding="utf-8")

        # ebur128 with true-peak; -f null discards audio
        cmd = [
            ffmpeg,
            "-hide_banner",
            "-nostats",
            "-f",
            "concat",
            "-safe",
            "0",
            "-i",
            str(list_file),
            "-af",
            "ebur128=peak=true:framelog=verbose",
            "-f",
            "null",
            "-",
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        text = (proc.stderr or "") + "\n" + (proc.stdout or "")
        if proc.returncode != 0:
            # Retry without true-peak if older ffmpeg
            cmd[cmd.index("ebur128=peak=true:framelog=verbose")] = "ebur128=peak=sample"
            proc = subprocess.run(cmd, capture_output=True, text=True)
            text = (proc.stderr or "") + "\n" + (proc.stdout or "")
            if proc.returncode != 0:
                raise RuntimeError(
                    f"ffmpeg ebur128 failed ({proc.returncode}): {text[-800:]}"
                )

    # Prefer the last Summary block values (album-integrated)
    integrated = _last_float(RE_I, text)
    if integrated is None:
        raise RuntimeError(f"could not parse integrated LUFS from ffmpeg:\n{text[-1200:]}")
    lra = _last_float(RE_LRA, text)
    tp = _last_float(RE_TP, text)
    if tp is None:
        tp = _last_float(RE_TP_ALT, text)
    if tp is None:
        tp = _last_float(RE_PEAK, text)
    if tp is None:
        raise RuntimeError(f"could not parse peak from ffmpeg:\n{text[-1200:]}")
    return integrated, lra, tp


def _last_float(pat: re.Pattern[str], text: str) -> float | None:
    matches = pat.findall(text)
    if not matches:
        return None
    return float(matches[-1])


def measure_track(path: Path) -> dict:
    integrated, _lra, tp = run_ebur128([path])
    return {
        "file": path.name,
        "integrated_lufs": round(integrated, 2),
        "true_peak_dbtp": round(tp, 2),
    }


def compute_default_gain_db(
    integrated_lufs: float,
    true_peak_dbtp: float,
    *,
    target_lufs: float = TARGET_LUFS,
    peak_ceiling_dbtp: float = PEAK_CEILING_DBTP,
) -> tuple[float, bool]:
    """Return (gain_db, peak_capped)."""
    loudness_gain = target_lufs - integrated_lufs
    peak_headroom = peak_ceiling_dbtp - true_peak_dbtp
    gain = min(loudness_gain, peak_headroom)
    peak_capped = gain < loudness_gain - 1e-6
    return round(gain, 2), peak_capped


def load_existing_trim(sidecar: Path) -> float:
    if not sidecar.is_file():
        return 0.0
    try:
        data = json.loads(sidecar.read_text(encoding="utf-8"))
        return float(data.get("trim_db", 0.0))
    except (OSError, json.JSONDecodeError, TypeError, ValueError):
        return 0.0


def scan_album(
    album_dir: Path,
    *,
    dry_run: bool,
    skip_tracks: bool,
    target_lufs: float,
    peak_ceiling: float,
) -> dict:
    album_dir = album_dir.resolve()
    wavs = list_album_wavs(album_dir)
    if not wavs:
        raise SystemExit(f"no .wav files in {album_dir}")

    sidecar = album_dir / SIDECAR_NAME
    trim_db = load_existing_trim(sidecar)

    track_rows: list[dict] = []
    if not skip_tracks:
        for w in wavs:
            print(f"  track {w.name} …", flush=True)
            track_rows.append(measure_track(w))

    print(f"  album ({len(wavs)} tracks) …", flush=True)
    integrated, lra, tp = run_ebur128(wavs)
    gain_db, peak_capped = compute_default_gain_db(
        integrated, tp, target_lufs=target_lufs, peak_ceiling_dbtp=peak_ceiling
    )

    doc = {
        "v": 1,
        "scanned_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "format": FORMAT_LABEL,
        "target_lufs": target_lufs,
        "peak_ceiling_dbtp": peak_ceiling,
        "integrated_lufs": round(integrated, 2),
        "true_peak_dbtp": round(tp, 2),
        "default_gain_db": gain_db,
        "trim_db": round(trim_db, 2),
        "peak_capped": peak_capped,
        "tracks": track_rows,
    }
    if lra is not None:
        doc["loudness_range_lu"] = round(lra, 2)

    rel = album_dir.name
    print(
        f"{album_dir}: I={doc['integrated_lufs']} LUFS  "
        f"TP={doc['true_peak_dbtp']} dBTP  "
        f"gain={gain_db} dB"
        + (" (peak-capped)" if peak_capped else "")
        + (f"  trim={trim_db} dB" if trim_db else "")
    )

    if not dry_run:
        sidecar.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        print(f"  wrote {sidecar}")
    else:
        print(f"  dry-run (would write {sidecar})")
        print(json.dumps(doc, indent=2))

    return doc


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Scan WAV24 albums → loudness.json (Amy jukebox J9)"
    )
    ap.add_argument(
        "path",
        type=Path,
        help="album directory, or library/artist root with -all",
    )
    ap.add_argument(
        "-all",
        action="store_true",
        help="scan every album under path (Artist/Album layout)",
    )
    ap.add_argument(
        "-r",
        action="store_true",
        help="with -all: find album dirs recursively",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="measure and print; do not write loudness.json",
    )
    ap.add_argument(
        "--skip-tracks",
        action="store_true",
        help="album-level measure only (faster; empty tracks[])",
    )
    ap.add_argument(
        "--target-lufs",
        type=float,
        default=TARGET_LUFS,
        help=f"target integrated loudness (default {TARGET_LUFS})",
    )
    ap.add_argument(
        "--peak-ceiling",
        type=float,
        default=PEAK_CEILING_DBTP,
        help=f"true-peak ceiling dBTP (default {PEAK_CEILING_DBTP})",
    )
    args = ap.parse_args()
    root = args.path.resolve()
    if not root.exists():
        print(f"not found: {root}", file=sys.stderr)
        return 1

    if args.all:
        albums = find_albums(root, recursive=args.r)
        if not albums:
            print("no albums with .wav found", file=sys.stderr)
            return 1
        print(f"scanning {len(albums)} album(s) under {root}")
        failed = 0
        for album in albums:
            try:
                scan_album(
                    album,
                    dry_run=args.dry_run,
                    skip_tracks=args.skip_tracks,
                    target_lufs=args.target_lufs,
                    peak_ceiling=args.peak_ceiling,
                )
            except Exception as e:
                failed += 1
                print(f"FAIL {album}: {e}", file=sys.stderr)
        print(f"done: {len(albums) - failed} ok, {failed} failed")
        return 1 if failed else 0

    if not root.is_dir():
        print(f"not a directory: {root}", file=sys.stderr)
        return 1
    scan_album(
        root,
        dry_run=args.dry_run,
        skip_tracks=args.skip_tracks,
        target_lufs=args.target_lufs,
        peak_ceiling=args.peak_ceiling,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        pass
