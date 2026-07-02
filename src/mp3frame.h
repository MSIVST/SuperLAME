/* superlame-mt: MP3 frame bitstream helpers + frame CRC.
 *
 * Ported verbatim (logic-preserving) from SuperFast's repacker.cpp anonymous
 * namespace and framecrc.cpp. These do byte-level MP3 header parsing and the
 * MPEG Layer III CRC-16. Do not "clean up" the bit math -- it is exact.
 *
 * Original: BoCA SuperFast codec, (C) 2007-2018 Robert Kausch, GPL v2+.
 */
#ifndef SUPERLAME_MP3FRAME_H
#define SUPERLAME_MP3FRAME_H

#include <cmath>

typedef unsigned char  u8;
typedef unsigned short u16;

namespace mp3 {

static const int maxFrameSize = 1441;

static const int bitrates[4][16]   = {
    { 0,  8000, 16000, 24000, 32000, 40000, 48000, 56000,  64000,  80000,  96000, 112000, 128000, 144000, 160000, 0 }, // MPEG 2.5
    { 0,     0,     0,     0,     0,     0,     0,     0,      0,      0,      0,      0,      0,      0,      0, 0 },     // reserved
    { 0,  8000, 16000, 24000, 32000, 40000, 48000, 56000,  64000,  80000,  96000, 112000, 128000, 144000, 160000, 0 }, // MPEG 2
    { 0, 32000, 40000, 48000, 56000, 64000, 80000, 96000, 112000, 128000, 160000, 192000, 224000, 256000, 320000, 0 } }; // MPEG 1

static const int samplerates[4][4] = {
    { 11025, 12000,  8000, 0 },   // MPEG 2.5
    {     0,     0,     0, 0 },    // reserved
    { 22050, 24000, 16000, 0 },   // MPEG 2
    { 44100, 48000, 32000, 0 } }; // MPEG 1

inline int  GetMode(const u8 *f)            { return (f[1] >> 3) & 0x03; }
inline int  GetLayer(const u8 *f)           { return (f[1] >> 1) & 0x03; }
inline int  GetBitrateIndex(const u8 *f)    { return f[2] >> 4; }
inline void SetBitrateIndex(u8 *f, int br)  { f[2] = (br << 4) | (f[2] & 0x0F); }
inline int  GetSampleRateIndex(const u8 *f) { return (f[2] >> 2) & 0x03; }
inline bool GetPadding(const u8 *f)         { return (f[2] >> 1) & 0x01; }
inline void SetPadding(u8 *f, bool pad)     { f[2] = (pad ? 0x02 : 0x00) | (f[2] & 0xFD); }

inline int GetFrameSize(const u8 *f) {
    int mode = GetMode(f), brindex = GetBitrateIndex(f), srindex = GetSampleRateIndex(f);
    int bitrate = bitrates[mode][brindex];
    int srate   = samplerates[mode][srindex];
    int padding = GetPadding(f);
    return (mode == 3 ? 144 : 72) * bitrate / srate + padding;
}

inline int GetHeaderLength(const u8 *f) { return (f[1] & 0x01) ? 4 : 6; }

inline int GetSideInfoLength(const u8 *f) {
    return GetMode(f) == 3 ? ((f[3] >> 6) == 0x03 ? 17 : 32)
                           : ((f[3] >> 6) == 0x03 ?  9 : 17);
}

inline int GetMainDataOffset(const u8 *f) {
    int hl = GetHeaderLength(f);
    return GetMode(f) == 3 ? (f[hl] << 1) | (f[hl + 1] >> 7) : f[hl];
}

inline void SetMainDataOffset(u8 *f, int offset) {
    int hl = GetHeaderLength(f);
    if (GetMode(f) == 3) {
        f[hl]     = offset >> 1;
        f[hl + 1] = ((offset & 1) << 7) | (f[hl + 1] & 0x7F);
    } else {
        f[hl] = offset;
    }
}

inline int GetMainDataLength(const u8 *f) {
    int hl = GetHeaderLength(f);
    int bits = 0;
    if (GetMode(f) != 3) {
        if (GetSideInfoLength(f) == 9) {
            bits += ((f[hl + 1] & 0x7F) << 5) | (f[hl + 2] >> 3);
        } else {
            bits += ((f[hl + 1] & 0x3F) << 6) | (f[hl +  2] >> 2);
            bits += ((f[hl + 9] & 0x7F) << 5) | (f[hl + 10] >> 3);
        }
    } else {
        if (GetSideInfoLength(f) == 17) {
            bits += ((f[hl + 2] & 0x3F) << 6) | (f[hl +  3] >> 2);
            bits += ((f[hl + 9] & 0x07) << 9) | (f[hl + 10] << 1) | (f[hl + 11] >> 7);
        } else {
            bits += ((f[hl +  2] & 0x0F) <<  8) | (f[hl +  3]);
            bits += ((f[hl +  9] & 0x01) << 11) | (f[hl + 10] << 3) | (f[hl + 11] >> 5);
            bits += ((f[hl + 17] & 0x3F) <<  6) | (f[hl + 18] >> 2);
            bits += ((f[hl + 24] & 0x07) <<  9) | (f[hl + 25] << 1) | (f[hl + 26] >> 7);
        }
    }
    return (int) std::ceil((float) bits / 8);
}

/* Validate a frame header. When `reference` is non-null, also require that the
 * frame's mode / header length / samplerate match it -- this prevents the
 * reservoir resizer from latching onto a false sync inside main data.
 * (Layer-III check + reference matching are the BoCA 2019-12-22 robustness fix;
 *  layer==1 in the field means Layer III.) */
inline bool IsValidFrame(const u8 *f, const u8 *reference = nullptr) {
    if (((f[0] << 3) | (f[1] >> 5)) != 0x07FF) return false;
    int mode = GetMode(f), layer = GetLayer(f),
        brindex = GetBitrateIndex(f), srindex = GetSampleRateIndex(f);
    if (mode == 1 || layer != 1 || brindex == 0 || brindex == 15 || srindex == 3) return false;
    if (GetFrameSize(f) < 24) return false;
    if (reference) {
        if (GetMode(f)            != GetMode(reference)            ||
            GetHeaderLength(f)    != GetHeaderLength(reference)    ||
            GetSampleRateIndex(f) != GetSampleRateIndex(reference)) return false;
    }
    return true;
}

/* MPEG Layer III CRC-16, polynomial 0x8005. Ported from framecrc.cpp. */
class FrameCRC {
    static u16 table[256];
    static bool initialized;
    u16 crc;
    static bool InitTable() {
        u16 polynomial = 0x8005;
        for (int i = 0; i <= 0xFF; i++) {
            u16 value = i << 8;
            for (int n = 0; n < 8; n++)
                value = (value << 1) ^ (value & (1 << 15) ? polynomial : 0);
            table[i] = value;
        }
        return true;
    }
    FrameCRC() : crc(0xFFFF) {}
    void Feed(const u8 *data, int size) {
        while (size--) crc = (crc << 8) ^ table[(crc >> 8) ^ *data++];
    }
public:
    static void Update(u8 *frame) {
        (void) initialized;
        FrameCRC c;
        int sil = ((frame[1] >> 3) & 0x03) == 0x03 ? ((frame[3] >> 6) == 0x03 ? 17 : 32)
                                                    : ((frame[3] >> 6) == 0x03 ?  9 : 17);
        c.Feed(frame + 2, 2);
        c.Feed(frame + 6, sil);
        frame[4] = c.crc >> 8;
        frame[5] = c.crc & 0xFF;
    }
};

} // namespace mp3

#endif
