/* superlame-mt: MP3 -> WAV decode via the built-in libmpg123.
 *
 * libmpg123 is already linked (it's the encoder's internal decoder). For
 * --decode we use its feed API directly: push the whole MP3 in, pull signed-16
 * PCM out, then wrap it in a WAV header. Output format (rate/channels) comes
 * from the stream. */
#ifndef SUPERLAME_DECODE_H
#define SUPERLAME_DECODE_H

#include <mpg123.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdint>

/* Returns true on success. `outRate`/`outChannels` report the decoded format. */
inline bool DecodeMp3(const std::vector<unsigned char> &mp3,
                      std::vector<short> &pcm, long &outRate, int &outChannels) {
    static bool inited = false;
    if (!inited) { mpg123_init(); inited = true; }

    int err = MPG123_OK;
    mpg123_handle *mh = mpg123_new(nullptr, &err);
    if (!mh) return false;

    /* Force output to signed 16-bit at the stream's native rate/channels:
     * allow all rates, signed-16 only. */
    mpg123_format_none(mh);
    const long rates[] = { 8000,11025,12000,16000,22050,24000,32000,44100,48000 };
    for (long r : rates) mpg123_format(mh, r, MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);

    if (mpg123_open_feed(mh) != MPG123_OK) { mpg123_delete(mh); return false; }

    long rate = 0; int channels = 0, encoding = 0;
    std::vector<unsigned char> out(64 * 1024);
    size_t done = 0;
    bool haveFormat = false;

    /* Feed the whole buffer once, then drain. */
    int ret = mpg123_decode(mh, mp3.data(), mp3.size(), out.data(), out.size(), &done);
    while (true) {
        if (ret == MPG123_NEW_FORMAT) {
            mpg123_getformat(mh, &rate, &channels, &encoding);
            haveFormat = true;
        }
        if (done > 0) {
            const short *s = (const short *) out.data();
            pcm.insert(pcm.end(), s, s + done / sizeof(short));
        }
        if (ret == MPG123_DONE) break;
        if (ret == MPG123_NEED_MORE) break;  /* fed everything already */
        if (ret != MPG123_OK && ret != MPG123_NEW_FORMAT) break;  /* error */
        /* Continue draining the already-fed data. */
        ret = mpg123_decode(mh, nullptr, 0, out.data(), out.size(), &done);
    }

    mpg123_delete(mh);

    if (!haveFormat || channels == 0) return false;
    outRate = rate;
    outChannels = channels;
    return true;
}

/* Write a canonical 16-bit PCM WAV from interleaved samples. Returns false if
 * any write fails (e.g. disk full) or the audio exceeds RIFF's 32-bit sizes. */
inline bool WriteWav(FILE *f, const std::vector<short> &pcm, long rate, int channels) {
    if (!f) return false;
    if (pcm.size() > (0xFFFFFFFFu - 44) / sizeof(short)) {
        fprintf(stderr, "decoded audio is too large for a WAV container (>4 GB)\n");
        return false;
    }
    uint32_t dataBytes = (uint32_t) (pcm.size() * sizeof(short));
    uint32_t byteRate  = (uint32_t) (rate * channels * 2);
    uint16_t blockAlign = (uint16_t) (channels * 2);
    bool ok = true;
    auto w32 = [&](uint32_t v) { unsigned char b[4] = { (unsigned char)v,(unsigned char)(v>>8),(unsigned char)(v>>16),(unsigned char)(v>>24) }; ok = ok && fwrite(b,1,4,f) == 4; };
    auto w16 = [&](uint16_t v) { unsigned char b[2] = { (unsigned char)v,(unsigned char)(v>>8) }; ok = ok && fwrite(b,1,2,f) == 2; };
    auto tag = [&](const char *s) { ok = ok && fwrite(s,1,4,f) == 4; };
    tag("RIFF");  w32(36 + dataBytes);  tag("WAVE");
    tag("fmt ");  w32(16);  w16(1);  w16((uint16_t) channels);
    w32((uint32_t) rate);  w32(byteRate);  w16(blockAlign);  w16(16);
    tag("data");  w32(dataBytes);
    ok = ok && fwrite(pcm.data(), 1, dataBytes, f) == dataBytes;
    return ok;
}

#endif
