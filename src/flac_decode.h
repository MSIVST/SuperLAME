/* superlame-mt: FLAC input decoding via dr_flac (MIT-0 / public domain).
 *
 * FLAC is a native input format here: the whole file is already in RAM (the
 * caller read it for the WAV/AIFF path), so we decode straight from memory into
 * the same interleaved +/-1.0 float buffer the WAV float path produces. That
 * feeds the existing resample -> SuperFast pipeline unchanged, and 24-bit /
 * hi-res FLAC keeps full precision (no early truncation to 16-bit).
 *
 * Multithreaded decode: FLAC frames are independently decodable and dr_flac can
 * seek by PCM frame, so we split the stream into N ranges and decode them
 * concurrently -- the same range-parallel shape the resampler uses -- keeping
 * decode off the critical path so the SuperFast encode speed-up isn't diluted.
 *
 * Metadata (Vorbis comments -> title/artist/... and the front-cover PICTURE
 * block -> album art) is captured during a cheap metadata-only open. CLI tags
 * always win over embedded ones; embedded art is used only when no --ti given.
 */
#ifndef SUPERLAME_FLAC_DECODE_H
#define SUPERLAME_FLAC_DECODE_H

#include "audiodata.h"
#include "tags.h"
#include <vector>
#include <string>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>

/* dr_flac: declaration here; exactly one TU defines DR_FLAC_IMPLEMENTATION. */
#include "../dr_flac/dr_flac.h"

/* Front-cover album art extracted from a FLAC PICTURE block (empty if none). */
struct FlacArt {
    std::vector<unsigned char> data;   /* raw image bytes (JPEG/PNG/...) */
    std::string                mime;   /* e.g. "image/jpeg" */
    int                        type = -1;  /* FLAC/ID3 picture type; 3 = front cover */
};

/* Case-insensitive prefix test for "KEY=" Vorbis comment keys. */
static inline bool flac_key_is(const char *comment, uint32_t len, const char *key) {
    size_t kl = strlen(key);
    if (len < kl + 1 || comment[kl] != '=') return false;
    for (size_t i = 0; i < kl; i++) {
        char a = comment[i], b = key[i];
        if (a >= 'a' && a <= 'z') a = (char) (a - 32);
        if (b >= 'a' && b <= 'z') b = (char) (b - 32);
        if (a != b) return false;
    }
    return true;
}

/* Collected during the metadata pass. */
struct FlacMeta {
    TagConfig tags;
    FlacArt   art;
};

/* dr_flac metadata callback: pull Vorbis comments and the best PICTURE block. */
static inline void flac_on_meta(void *pUserData, drflac_metadata *m) {
    FlacMeta *fm = (FlacMeta *) pUserData;
    if (m->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
        drflac_vorbis_comment_iterator it;
        drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount,
                                            m->data.vorbis_comment.pComments);
        uint32_t clen = 0;
        const char *c;
        while ((c = drflac_next_vorbis_comment(&it, &clen)) != NULL) {
            auto val = [&](const char *key) -> std::string {
                size_t kl = strlen(key) + 1;               /* "KEY=" */
                return std::string(c + kl, clen - (uint32_t) kl);
            };
            if      (flac_key_is(c, clen, "TITLE"))       fm->tags.title   = val("TITLE");
            else if (flac_key_is(c, clen, "ARTIST"))      fm->tags.artist  = val("ARTIST");
            else if (flac_key_is(c, clen, "ALBUM"))       fm->tags.album   = val("ALBUM");
            else if (flac_key_is(c, clen, "DATE"))        fm->tags.year    = val("DATE");
            else if (flac_key_is(c, clen, "COMMENT"))     fm->tags.comment = val("COMMENT");
            else if (flac_key_is(c, clen, "TRACKNUMBER")) fm->tags.track   = val("TRACKNUMBER");
            else if (flac_key_is(c, clen, "GENRE"))       fm->tags.genre   = val("GENRE");
        }
        if (!fm->tags.title.empty() || !fm->tags.artist.empty() || !fm->tags.album.empty() ||
            !fm->tags.year.empty()  || !fm->tags.comment.empty() ||
            !fm->tags.track.empty() || !fm->tags.genre.empty())
            fm->tags.any = true;
    } else if (m->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE) {
        /* Prefer front cover (type 3); otherwise take the first picture seen.
         * pPictureData points to a temporary buffer -- copy it now. */
        int t = (int) m->data.picture.type;
        bool better = fm->art.data.empty() || (t == 3 && fm->art.type != 3);
        if (better && m->data.picture.pictureDataSize > 0 && m->data.picture.pPictureData) {
            fm->art.data.assign(m->data.picture.pPictureData,
                                m->data.picture.pPictureData + m->data.picture.pictureDataSize);
            fm->art.mime.assign(m->data.picture.mime ? m->data.picture.mime : "",
                                m->data.picture.mimeLength);
            fm->art.type = t;
        }
    }
}

/* Read `want` frames of `ch` channels from `p` into interleaved float `dst`,
 * normalized to +/-1.0. For <=24 bits we use dr_flac's f32 reader directly.
 * For >24-bit sources (e.g. 32-bit-per-sample FLAC) dr_flac's f32 reader
 * refuses (float32 can't hold 32 bits of mantissa), so we read s32 and scale
 * ourselves (accepting the float32 rounding, same as our 32-bit WAV path).
 * Returns frames actually read. */
static inline uint64_t flac_read_block(drflac *p, int ch, int bits,
                                       uint64_t want, float *dst,
                                       std::vector<int32_t> &scratch) {
    if (bits <= 24) return drflac_read_pcm_frames_f32(p, want, dst);
    if (scratch.size() < (size_t) want * ch) scratch.resize((size_t) want * ch);
    uint64_t got = drflac_read_pcm_frames_s32(p, want, scratch.data());
    for (uint64_t i = 0; i < got * (uint64_t) ch; i++)
        dst[i] = (float) scratch[(size_t) i] / 2147483648.0f;  /* /2^31 -> +/-1 */
    return got;
}

/* Decode a FLAC file already loaded in `buf` into `w` (interleaved float).
 * Returns false on any decode error. Fills `meta` with embedded tags/art. */
static inline bool DecodeFlac(const std::vector<unsigned char> &buf, WavData &w,
                              FlacMeta &meta, int nThreads, bool quiet) {
    /* --- metadata + stream info (single open over the whole file) --- */
    drflac *pf = drflac_open_memory_with_metadata(buf.data(), buf.size(),
                                                  flac_on_meta, &meta, NULL);
    if (!pf) {
        fprintf(stderr, "FLAC: could not open/parse stream\n");
        return false;
    }
    int      rate    = (int) pf->sampleRate;
    int      ch      = (int) pf->channels;
    int      bits    = (int) pf->bitsPerSample;
    uint64_t frames  = pf->totalPCMFrameCount;   /* per-channel frame count (0 = unknown) */
    drflac_close(pf);

    if (ch < 1 || ch > 2) { fprintf(stderr, "FLAC: only mono/stereo (%d ch)\n", ch); return false; }
    if (rate < 1 || rate > 384000) { fprintf(stderr, "FLAC: bad sample rate %d\n", rate); return false; }
    /* 32-bit-per-sample FLAC (a recent, rare format addition) is not decodable
     * by the bundled dr_flac -- both its f32 and s32 readers return no samples.
     * Refuse cleanly with an honest reason rather than emitting silence. */
    if (bits > 24) {
        fprintf(stderr, "FLAC: %d-bit-per-sample streams are not supported "
                "(the bundled decoder handles up to 24-bit).\n", bits);
        return false;
    }

    w.rate = rate; w.channels = ch; w.bits = bits;
    w.useFloat = true;
    w.samples.clear();

    /* Decide the decode strategy:
     *  - known total AND <=24-bit  -> multithreaded range-parallel (fast path)
     *  - unknown total, or >24-bit -> single-threaded streaming (robust path)
     * The streaming path also covers files where STREAMINFO omits the sample
     * count (a legal FLAC), which we must NOT reject. */
    bool canMT = (frames > 0) && (bits <= 24) && (nThreads > 1);

    if (!canMT) {
        /* --- single-threaded streaming decode --- */
        drflac *p = drflac_open_memory(buf.data(), buf.size(), NULL);
        if (!p) { fprintf(stderr, "FLAC: could not open stream for decode\n"); return false; }
        std::vector<int32_t> scratch;
        const uint64_t CHUNK = 1u << 16;                 /* frames per read */
        if (frames > 0) w.samplesF.reserve((size_t) frames * ch);
        std::vector<float> tmp((size_t) CHUNK * ch);
        uint64_t total = 0;
        for (;;) {
            uint64_t got = flac_read_block(p, ch, bits, CHUNK, tmp.data(), scratch);
            if (got == 0) break;
            w.samplesF.insert(w.samplesF.end(), tmp.begin(), tmp.begin() + (size_t) got * ch);
            total += got;
        }
        drflac_close(p);
        if (total == 0) { fprintf(stderr, "FLAC: no audio decoded\n"); return false; }
        frames = total;
        if (!quiet)
            fprintf(stderr, "decoded FLAC: %d Hz %d-bit %s, %.2f s (1 thread%s)\n",
                    rate, bits, ch == 1 ? "mono" : "stereo",
                    (double) frames / rate, bits > 24 ? ", 32-bit->float" : "");
        return true;
    }

    /* --- multithreaded range-parallel decode (known length, <=24-bit) --- *
     * Each thread opens its OWN drflac over the same (immutable) memory buffer,
     * seeks to its start frame, and decodes its slice into the shared output.
     * Threads write disjoint regions, so no locking is needed. */
    w.samplesF.assign((size_t) frames * ch, 0.0f);
    int workers = std::max(1, nThreads);
    const uint64_t MIN_PER_THREAD = 1u << 16;      /* 64k frames */
    if (frames / (uint64_t) workers < MIN_PER_THREAD)
        workers = (int) std::max<uint64_t>(1, frames / MIN_PER_THREAD);
    if (workers < 1) workers = 1;

    std::vector<std::thread> pool;
    std::vector<int> ok(workers, 1);
    for (int k = 0; k < workers; k++) {
        uint64_t f0 = frames *  (uint64_t) k      / (uint64_t) workers;
        uint64_t f1 = frames * ((uint64_t) k + 1) / (uint64_t) workers;
        if (f1 <= f0) continue;
        pool.emplace_back([&, k, f0, f1]() {
            drflac *p = drflac_open_memory(buf.data(), buf.size(), NULL);
            if (!p) { ok[k] = 0; return; }
            if (f0 != 0 && !drflac_seek_to_pcm_frame(p, f0)) { drflac_close(p); ok[k] = 0; return; }
            std::vector<int32_t> scratch;
            uint64_t want = f1 - f0;
            float   *dst  = w.samplesF.data() + (size_t) f0 * ch;
            uint64_t got  = flac_read_block(p, ch, bits, want, dst, scratch);
            drflac_close(p);
            if (got != want) ok[k] = 0;   /* short read => decode error/desync */
        });
    }
    for (auto &t : pool) t.join();

    /* If any MT segment failed (rare: seek/desync on an odd file), fall back to
     * a single-threaded streaming decode rather than giving up -- that path is
     * more tolerant. */
    for (int k = 0; k < workers; k++) if (!ok[k]) {
        if (!quiet) fprintf(stderr, "FLAC: MT segment %d failed; retrying single-threaded...\n", k);
        drflac *p = drflac_open_memory(buf.data(), buf.size(), NULL);
        if (!p) { fprintf(stderr, "FLAC: decode failed\n"); return false; }
        std::vector<int32_t> scratch;
        w.samplesF.assign((size_t) frames * ch, 0.0f);
        uint64_t got = flac_read_block(p, ch, bits, frames, w.samplesF.data(), scratch);
        drflac_close(p);
        if (got == 0) { fprintf(stderr, "FLAC: decode failed\n"); return false; }
        if (got < frames) w.samplesF.resize((size_t) got * ch);   /* trust what decoded */
        frames = got;
        if (!quiet)
            fprintf(stderr, "decoded FLAC: %d Hz %d-bit %s, %.2f s (1 thread, fallback)\n",
                    rate, bits, ch == 1 ? "mono" : "stereo", (double) frames / rate);
        return true;
    }

    if (!quiet)
        fprintf(stderr, "decoded FLAC: %d Hz %d-bit %s, %.2f s (%d thread%s)\n",
                rate, bits, ch == 1 ? "mono" : "stereo",
                (double) frames / rate, workers, workers == 1 ? "" : "s");
    return true;
}

#endif
