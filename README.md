Something of a playground for audio codecs I'm working on.
These are designed for use in VOIP, mainly.

They can take in audio at normal capture rate for desktop and mobile, eg. 48kHz or 44.1kHz.

The codec container has pluggable squelchers, filters and compressors, applied in that order.

The basic idea is to first squelch, to remove systematic noise and imperceptible sounds.
Then filter and downsample to a lower rate. Currently, 4:1 downsampling is implemented, giving 12kHz or 11.025kHz respectively.
Finally, the downsampled audio can be compressed to create data packets suitable for VOIP transmission.

Current compressors are:

**8BitScaled**
Compresses the input from 16-bit or Float32 to 8-bit signed, using packet-by-packet scaling.

**8BitVbrDelta**
Internally this first compresses using the 8BitScaled compressor.
Then it takes that 8-bit raw sample packet and compresses it further using a variable bit-width delta
encoding, using runs of deltas at the smallest bit width it can, balancing switching costs against the
cost of encoding samples, using bit widths from 3 to 8 bits per sample.
It also encodes runs of silence efficiently, using two codes which encode either 4 or 16 zero samples. 

