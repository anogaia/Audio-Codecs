# ANOG file format (`.anog`)

Version **1** — little-endian throughout.

Container for Audio-Codecs VOIP bitstreams: variable channel count, length-prefixed
interleaved mono packets, and a seek table immediately after the file header.

Inspired by IVF (framed packets), CAF/FLAC (header + seek table near the front).

## Layout

```text
File
├── ANOGHeader          (64 bytes)
├── SeekTable           (seek_entry_count × 16 bytes)
└── PacketStream        (until EOF)
```

## `ANOGHeader` (64 bytes)

| Offset | Field | Type | Notes |
|--------|-------|------|--------|
| 0 | `magic` | `char[4]` | `'A','N','O','G'` |
| 4 | `version` | `u16` | `1` |
| 6 | `header_size` | `u16` | `64` |
| 8 | `channels` | `u8` | 1…N (stereo = 2) |
| 9 | `codec_id` | `u8` | see below |
| 10 | `filter_id` | `u8` | see below |
| 11 | `flags` | `u8` | reserved `0` |
| 12 | `pcm_sample_rate` | `u32` | e.g. 44100 / 48000 |
| 16 | `compressed_rate` | `u32` | `12000` for current codecs |
| 20 | `frame_ms` | `u16` | requested frame duration (writer) |
| 22 | `reserved0` | `u16` | `0` |
| 24 | `pcm_total_samples` | `u64` | samples per channel |
| 32 | `seek_entry_count` | `u32` | time frames (= seek rows) |
| 36 | `compressed_total_samples` | `u64` | compressed-domain samples per channel |
| 44 | `padding` | `u8[20]` | `0` |

### Codec IDs

| ID | Codec |
|----|--------|
| `1` | 8BitVbrDelta |

### Filter IDs

| ID | Filter |
|----|--------|
| `1` | 48 kHz ↔ 12 kHz polyphase FIR (4:1) |
| `2` | 44.1 kHz ↔ 12 kHz rational polyphase FIR (40:147) |

## `SeekTable` entry (16 bytes)

| Field | Type | Notes |
|-------|------|--------|
| `tick` | `u64` | compressed-domain sample index at **frame start** (shared across channels) |
| `file_offset` | `u64` | absolute file offset of first packet of this time frame (channel 0) |

One entry per **time frame**, not per channel.

**Seeking:** binary search by `tick`, jump to `file_offset`. Treat frame starts as
keyframes: reset filter/codec state before decoding from a seek point.

Compressed samples in frame `i`:

```text
count = (i + 1 < seek_entry_count)
      ? seek[i+1].tick - seek[i].tick
      : compressed_total_samples - seek[i].tick
```

## Packet

| Field | Type | Notes |
|-------|------|--------|
| `payload_len` | `u16` | bytes of payload |
| `channel` | `u8` | `0 .. channels-1` |
| `comp_samples` | `u16` | compressed-domain samples represented by this packet |
| `payload` | `u8[payload_len]` | opaque codec packet |

### Interleave order

For each time frame `t`, channels are written in order `0 .. N-1`. **Within a channel**,
one or more packets may appear (in order) so that their `comp_samples` sum to the
frame’s compressed length. This keeps each VBR payload near the codec’s native
size (≤ **256** compressed samples), which the 8BitVbrDelta compressor expects.

Example stereo frame with 1200 compressed samples/ch (four × 256 + one × 176):

```text
pkt(t, ch0, 256) × 4, pkt(t, ch0, 176),
pkt(t, ch1, 256) × 4, pkt(t, ch1, 176)
```

No per-packet timestamp; time comes from the seek table and running `tick`.

## Frame duration (`frame_ms`)

Writers expose `--frame-ms` (default **100**).

For frame index `i` (0-based):

1. Ideal PCM end index ≈ `round((i+1) * frame_ms * pcm_sample_rate / 1000)`, clamped to `pcm_total_samples`.
2. PCM slice = `[prev_end, end)`.
3. Decimate with the selected filter (**streaming state** across frames when encoding from the start).
4. Compressed length is whatever the filter emits (best-effort; 44.1→12 may vary by ±1 sample between frames).
5. Seek row `tick` = cumulative compressed samples **before** this frame.
6. Encode each channel’s compressed PCM in VBR chunks of at most **256** samples
   (one or more packets per channel).

The last frame may be shorter than `frame_ms`.

Examples at 100 ms:

- 48 kHz / 4:1 → 4800 PCM → 1200 compressed (exact)
- 44.1 kHz / 40:147 → 4410 PCM → 1200 compressed (exact)

## Encode strategy (seek table after header)

1. Known-length WAV → compute `seek_entry_count` from duration / `frame_ms`.
2. Write header + zero-filled seek table.
3. For each frame: record `file_offset`, write interleaved packets, fill seek row.
4. Seek back; rewrite seek table; patch `compressed_total_samples` (and totals) in the header.

## Legacy

The earlier `VBIF` + raw `.vbi` dump was a prototype for compression-ratio experiments.
New code should use `.anog` only.

## Tools

```bash
anog -enc input.wav [output.anog] [--frame-ms 100]
anog -dec input.anog [output.wav]
anog -enc '*'            # all WAVs in cwd
anog -dec '*'            # all ANOGs in cwd
anog -all                # WAVs in cwd → ./ANOG/
anog -all -r             # recursive; mirrored tree under ./ANOG/
```

Quote `'*'` for the shell. Existing outputs ask before overwrite.

## Future options

### WAV metadata pass-through

**Status:** not implemented (v1 ignores non-audio WAV tags).

RIFF/WAVE files can carry metadata in chunks other than `data` (e.g. `LIST`/`INFO` for artist/title/album, `bext`, `id3 `, `iXML`). Today encode reads PCM only and decode writes a plain 16-bit PCM WAV, so those tags are dropped.

**Proposed approach**

1. On encode, scan the source RIFF and collect chunks **other than** `data` and `fmt ` as an opaque byte blob (chunk id + size + payload, including pad bytes), in file order.
2. Store the blob in the `.anog` file (e.g. `metadata_size` + raw bytes after the fixed header / before or after the seek table — exact layout TBD; bump `version` or use reserved header fields).
3. On decode, write a new WAV with a correct `fmt ` + `data` for the decoded 16-bit PCM, then append the stored non-audio chunks verbatim so tags round-trip.

**Notes**

- Do **not** restore the original `fmt ` blindly: decoded audio is always 16-bit integer PCM at the header’s `pcm_sample_rate` / channel count; a stale float/24-bit `fmt ` would lie.
- Some sources have no metadata chunks at all (only `fmt ` + `data`); pass-through is a no-op for those.
- Current `AudioFile` helpers only optionally retain `iXML`; a small dedicated RIFF walk on encode is enough and keeps the blob format-agnostic.

