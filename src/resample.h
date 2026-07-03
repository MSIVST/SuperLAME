/* superlame-mt: sample-rate conversion via r8brain-free-src (MIT).
 *
 * MP3 can only store a fixed set of sample rates. When the input rate isn't one
 * of them, we resample to the nearest legal rate HERE, before SuperFast splits
 * the audio into chunks -- so every parallel encoder sees one clean rate and
 * LAME's own (per-frame, stateful) resampler never engages. That avoids the
 * chunk-boundary drift you get when resampling happens inside the workers.
 *
 * We use CDSPResampler24 at its defaults: ~207 dB stop-band, 2% transition band,
 * linear phase. That is reference-grade -- better than LAME's internal Blackman-
 * sinc, and transparent by a large margin *before* MP3 quantization, so the
 * encoder (not the resampler) is the quality bottleneck. See THIRD-PARTY.md
 * ("Resampling quality") for the full rationale and the linear-vs-minimum-phase
 * / 24-vs-16-bit trade-offs. */
#ifndef SUPERLAME_RESAMPLE_H
#define SUPERLAME_RESAMPLE_H

#include "../r8brain/CDSPResampler.h"
#include <vector>
#include <cstdint>
#include <thread>
#include <algorithm>

inline bool IsLegalMp3Rate(int r) {
    switch (r) {
        case 8000: case 11025: case 12000: case 16000:
        case 22050: case 24000: case 32000: case 44100: case 48000:
            return true;
        default: return false;
    }
}

/* Choose the output MP3 rate for an unsupported input rate. We're family-aware:
 * a 44.1k-family rate (88.2/176.4/...) maps to 44100, a 48k-family rate
 * (96/192/...) maps to 48000 -- these are clean integer ratios and keep the
 * audio in its native clock family (better than LAME's blind round-up, which
 * would send 88.2k to 48k, a non-integer 0.544x conversion). Other odd rates
 * fall back to the nearest supported rate. */
inline int NearestMp3Rate(int freq) {
    if (IsLegalMp3Rate(freq)) return freq;
    /* Integer multiples of the two base families. */
    if (freq % 44100 == 0) return 44100;
    if (freq % 48000 == 0) return 48000;
    if (freq % 22050 == 0) return (freq / 22050) % 2 == 0 ? 44100 : 22050;
    if (freq % 24000 == 0) return (freq / 24000) % 2 == 0 ? 48000 : 24000;
    /* Generic fallback: nearest supported rate by value. */
    static const int rates[] = { 8000,11025,12000,16000,22050,24000,32000,44100,48000 };
    int best = 48000, bestd = 1 << 30;
    for (int r : rates) { int dd = freq > r ? freq - r : r - freq; if (dd < bestd) { bestd = dd; best = r; } }
    return best;
}

/* Resample one channel of doubles src->dst. `nThreads` splits the channel into
 * overlapping time-segments resampled in parallel and stitched. Each segment
 * gets `guard` input samples of warm-up context on each side so the filter
 * history is correct at the seams; the extra output is trimmed off. */
inline std::vector<double> ResampleChannel(const std::vector<double> &in,
                                           int srcRate, int dstRate, int nThreads) {
    long long inN  = (long long) in.size();
    long long outN = inN * dstRate / srcRate;
    if (inN == 0) return {};

    /* One-shot for small inputs or single thread -- simplest correct path. */
    if (nThreads <= 1 || inN < 1 << 18) {
        r8b::CDSPResampler24 rs((double) srcRate, (double) dstRate, (int) std::min(inN, (long long) (1 << 20)));
        std::vector<double> out((size_t) outN);
        rs.oneshot(in.data(), (int) inN, out.data(), (int) outN);
        return out;
    }

    /* Chunked: split the OUTPUT range across threads; each thread resamples the
     * corresponding input span plus guard on both sides, then keeps only its
     * portion. guard >> filter length (a few thousand taps at most). */
    const long long guard = 8192;
    int chunks = nThreads;
    std::vector<double> out((size_t) outN);
    std::vector<std::thread> pool;

    for (int k = 0; k < chunks; k++) {
        long long o0 = outN *  k      / chunks;   /* this chunk's output range */
        long long o1 = outN * (k + 1) / chunks;
        if (o1 <= o0) continue;
        pool.emplace_back([&, o0, o1]() {
            /* Input span mapping to [o0,o1), padded by guard on each side. */
            long long i0 = o0 * srcRate / dstRate - guard;
            long long i1 = o1 * srcRate / dstRate + guard;
            if (i0 < 0)   i0 = 0;
            if (i1 > inN) i1 = inN;
            long long segIn = i1 - i0;

            r8b::CDSPResampler24 rs((double) srcRate, (double) dstRate, (int) segIn);
            /* Resample the whole padded input span. */
            long long segOut = segIn * dstRate / srcRate;
            std::vector<double> tmp((size_t) segOut);
            rs.oneshot(in.data() + i0, (int) segIn, tmp.data(), (int) segOut);

            /* Where does the padded segment's output start, in global out coords? */
            long long padOutStart = i0 * dstRate / srcRate;
            for (long long o = o0; o < o1; o++) {
                long long t = o - padOutStart;
                out[(size_t) o] = (t >= 0 && t < segOut) ? tmp[(size_t) t] : 0.0;
            }
        });
    }
    for (auto &t : pool) t.join();
    return out;
}

/* Resample interleaved float (+/-1.0) samples from srcRate to dstRate.
 * `channels` is 1 or 2. Channels resample concurrently; each channel is itself
 * split across the remaining cores. Returns interleaved float at dstRate. */
inline std::vector<float> ResampleFloat(const std::vector<float> &in, int channels,
                                        int srcRate, int dstRate) {
    size_t frames = in.size() / channels;
    long long expectFrames = (long long) frames * dstRate / srcRate;

    int hw = (int) std::max(1u, std::thread::hardware_concurrency());
    int perChan = std::max(1, hw / channels);   /* split cores across channels */

    std::vector<std::vector<double>> chOut(channels);
    std::vector<float> out((size_t) expectFrames * channels);

    /* One thread per channel: deinterleave, resample (itself multi-threaded),
     * then interleave that channel's output back -- all off the main thread so
     * the serial interleave overhead is parallelized across channels too. */
    std::vector<std::thread> pool;
    for (int c = 0; c < channels; c++) {
        pool.emplace_back([&, c]() {
            std::vector<double> chIn(frames);
            for (size_t i = 0; i < frames; i++) chIn[i] = in[i * channels + c];
            std::vector<double> o = ResampleChannel(chIn, srcRate, dstRate, perChan);
            for (long long i = 0; i < expectFrames; i++) {
                double v = (i < (long long) o.size()) ? o[(size_t) i] : 0.0;
                if (v >  1.0) v =  1.0;
                if (v < -1.0) v = -1.0;
                out[(size_t) (i * channels + c)] = (float) v;
            }
        });
    }
    for (auto &t : pool) t.join();
    return out;
}

#endif
