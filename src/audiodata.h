/* superlame-mt: the in-memory decoded-audio buffer shared by the input readers
 * (WAV/AIFF/FLAC) and the encode pipeline. A source is either 16-bit short
 * (16/8-bit PCM, kept bit-identical) or normalized +/-1.0 float (24/32-bit,
 * float, and all resampled/decoded paths -- full precision, no truncation). */
#ifndef SUPERLAME_AUDIODATA_H
#define SUPERLAME_AUDIODATA_H

#include <vector>
#include <cstddef>

struct WavData {
    int  rate = 0, channels = 0, bits = 0;
    bool useFloat = false;              // true => samplesF holds normalized +/-1.0 float
    std::vector<short> samples;        // interleaved 16-bit (16/8-bit sources)
    std::vector<float> samplesF;       // interleaved float  (24/32-bit/float sources)
    /* element count + raw pointer, regardless of type */
    size_t count() const { return useFloat ? samplesF.size() : samples.size(); }
    const void *data() const { return useFloat ? (const void *) samplesF.data() : (const void *) samples.data(); }
};

#endif
