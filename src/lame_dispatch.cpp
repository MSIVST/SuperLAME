/* superlame-mt: engine selection + CPUID probe. */
#include "lame_dispatch.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
static void cpuidex(int regs[4], int leaf, int sub) { __cpuidex(regs, leaf, sub); }
#else
#include <cpuid.h>
static void cpuidex(int regs[4], int leaf, int sub) {
    __cpuid_count(leaf, sub, regs[0], regs[1], regs[2], regs[3]);
}
#endif

/* True if the CPU supports the Zen3-relevant feature set we built znver3 for:
 * AVX2 (leaf 7, EBX bit 5) + FMA (leaf 1, ECX bit 12) + OSXSAVE/AVX enabled. */
static bool HasZnver3Features() {
    int r[4];
    cpuidex(r, 0, 0);
    int maxLeaf = r[0];
    if (maxLeaf < 7) return false;

    cpuidex(r, 1, 0);
    bool osxsave = (r[2] >> 27) & 1;
    bool avx     = (r[2] >> 28) & 1;
    bool fma     = (r[2] >> 12) & 1;
    if (!(osxsave && avx && fma)) return false;

    cpuidex(r, 7, 0);
    bool avx2 = (r[1] >> 5) & 1;
    bool bmi2 = (r[1] >> 8) & 1;
    return avx2 && bmi2;
}

/* Prefixed engine symbols (defined in the two libmp3lame builds). */
extern "C" {
    /* znver3 engine */
    lame_t __l3v_lame_init(void);
    int    __l3v_lame_set_in_samplerate(lame_global_flags *, int);
    int    __l3v_lame_set_num_channels(lame_global_flags *, int);
    int    __l3v_lame_set_brate(lame_global_flags *, int);
    int    __l3v_lame_set_quality(lame_global_flags *, int);
    int    __l3v_lame_set_mode(lame_global_flags *, MPEG_mode);
    int    __l3v_lame_set_VBR(lame_global_flags *, vbr_mode);
    int    __l3v_lame_set_VBR_mean_bitrate_kbps(lame_global_flags *, int);
    int    __l3v_lame_set_VBR_quality(lame_global_flags *, float);
    int    __l3v_lame_init_params(lame_global_flags *);
    int    __l3v_lame_get_framesize(const lame_global_flags *);
    int    __l3v_lame_encode_buffer_interleaved(lame_global_flags *, short[], int, unsigned char *, int);
    int    __l3v_lame_encode_buffer(lame_global_flags *, const short[], const short[], int, unsigned char *, int);
    int    __l3v_lame_encode_buffer_interleaved_ieee_float(lame_global_flags *, const float[], int, unsigned char *, int);
    int    __l3v_lame_encode_buffer_ieee_float(lame_global_flags *, const float[], const float[], int, unsigned char *, int);
    int    __l3v_lame_encode_flush(lame_global_flags *, unsigned char *, int);
    int    __l3v_lame_encode_flush_nogap(lame_global_flags *, unsigned char *, int);
    size_t __l3v_lame_get_lametag_frame(const lame_global_flags *, unsigned char *, size_t);
    int    __l3v_lame_close(lame_global_flags *);
    void   __l3v_id3tag_init(lame_global_flags *);
    void   __l3v_id3tag_add_v2(lame_global_flags *);
    void   __l3v_id3tag_add_v2_4_UTF8(lame_global_flags *);
    void   __l3v_id3tag_v2_only(lame_global_flags *);
    void   __l3v_id3tag_set_title(lame_global_flags *, const char *);
    void   __l3v_id3tag_set_artist(lame_global_flags *, const char *);
    void   __l3v_id3tag_set_album(lame_global_flags *, const char *);
    void   __l3v_id3tag_set_year(lame_global_flags *, const char *);
    void   __l3v_id3tag_set_comment(lame_global_flags *, const char *);
    int    __l3v_id3tag_set_track(lame_global_flags *, const char *);
    int    __l3v_id3tag_set_genre(lame_global_flags *, const char *);
    size_t __l3v_lame_get_id3v2_tag(lame_global_flags *, unsigned char *, size_t);
    size_t __l3v_lame_get_id3v1_tag(lame_global_flags *, unsigned char *, size_t);

    /* sse2 engine */
    lame_t __lsse_lame_init(void);
    int    __lsse_lame_set_in_samplerate(lame_global_flags *, int);
    int    __lsse_lame_set_num_channels(lame_global_flags *, int);
    int    __lsse_lame_set_brate(lame_global_flags *, int);
    int    __lsse_lame_set_quality(lame_global_flags *, int);
    int    __lsse_lame_set_mode(lame_global_flags *, MPEG_mode);
    int    __lsse_lame_set_VBR(lame_global_flags *, vbr_mode);
    int    __lsse_lame_set_VBR_mean_bitrate_kbps(lame_global_flags *, int);
    int    __lsse_lame_set_VBR_quality(lame_global_flags *, float);
    int    __lsse_lame_init_params(lame_global_flags *);
    int    __lsse_lame_get_framesize(const lame_global_flags *);
    int    __lsse_lame_encode_buffer_interleaved(lame_global_flags *, short[], int, unsigned char *, int);
    int    __lsse_lame_encode_buffer(lame_global_flags *, const short[], const short[], int, unsigned char *, int);
    int    __lsse_lame_encode_buffer_interleaved_ieee_float(lame_global_flags *, const float[], int, unsigned char *, int);
    int    __lsse_lame_encode_buffer_ieee_float(lame_global_flags *, const float[], const float[], int, unsigned char *, int);
    int    __lsse_lame_encode_flush(lame_global_flags *, unsigned char *, int);
    int    __lsse_lame_encode_flush_nogap(lame_global_flags *, unsigned char *, int);
    size_t __lsse_lame_get_lametag_frame(const lame_global_flags *, unsigned char *, size_t);
    int    __lsse_lame_close(lame_global_flags *);
    void   __lsse_id3tag_init(lame_global_flags *);
    void   __lsse_id3tag_add_v2(lame_global_flags *);
    void   __lsse_id3tag_add_v2_4_UTF8(lame_global_flags *);
    void   __lsse_id3tag_v2_only(lame_global_flags *);
    void   __lsse_id3tag_set_title(lame_global_flags *, const char *);
    void   __lsse_id3tag_set_artist(lame_global_flags *, const char *);
    void   __lsse_id3tag_set_album(lame_global_flags *, const char *);
    void   __lsse_id3tag_set_year(lame_global_flags *, const char *);
    void   __lsse_id3tag_set_comment(lame_global_flags *, const char *);
    int    __lsse_id3tag_set_track(lame_global_flags *, const char *);
    int    __lsse_id3tag_set_genre(lame_global_flags *, const char *);
    size_t __lsse_lame_get_id3v2_tag(lame_global_flags *, unsigned char *, size_t);
    size_t __lsse_lame_get_id3v1_tag(lame_global_flags *, unsigned char *, size_t);
}

static const LameEngine kZnver3 = {
    "znver3 (AVX2/FMA)",
    __l3v_lame_init, __l3v_lame_set_in_samplerate, __l3v_lame_set_num_channels,
    __l3v_lame_set_brate, __l3v_lame_set_quality, __l3v_lame_set_mode,
    __l3v_lame_set_VBR, __l3v_lame_set_VBR_mean_bitrate_kbps, __l3v_lame_set_VBR_quality,
    __l3v_lame_init_params, __l3v_lame_get_framesize,
    __l3v_lame_encode_buffer_interleaved, __l3v_lame_encode_buffer,
    __l3v_lame_encode_buffer_interleaved_ieee_float, __l3v_lame_encode_buffer_ieee_float,
    __l3v_lame_encode_flush, __l3v_lame_encode_flush_nogap,
    __l3v_lame_get_lametag_frame, __l3v_lame_close,
    __l3v_id3tag_init, __l3v_id3tag_add_v2, __l3v_id3tag_add_v2_4_UTF8, __l3v_id3tag_v2_only,
    __l3v_id3tag_set_title, __l3v_id3tag_set_artist, __l3v_id3tag_set_album,
    __l3v_id3tag_set_year, __l3v_id3tag_set_comment,
    __l3v_id3tag_set_track, __l3v_id3tag_set_genre,
    __l3v_lame_get_id3v2_tag, __l3v_lame_get_id3v1_tag,
};

static const LameEngine kSse2 = {
    "x86-64 (SSE2 fallback)",
    __lsse_lame_init, __lsse_lame_set_in_samplerate, __lsse_lame_set_num_channels,
    __lsse_lame_set_brate, __lsse_lame_set_quality, __lsse_lame_set_mode,
    __lsse_lame_set_VBR, __lsse_lame_set_VBR_mean_bitrate_kbps, __lsse_lame_set_VBR_quality,
    __lsse_lame_init_params, __lsse_lame_get_framesize,
    __lsse_lame_encode_buffer_interleaved, __lsse_lame_encode_buffer,
    __lsse_lame_encode_buffer_interleaved_ieee_float, __lsse_lame_encode_buffer_ieee_float,
    __lsse_lame_encode_flush, __lsse_lame_encode_flush_nogap,
    __lsse_lame_get_lametag_frame, __lsse_lame_close,
    __lsse_id3tag_init, __lsse_id3tag_add_v2, __lsse_id3tag_add_v2_4_UTF8, __lsse_id3tag_v2_only,
    __lsse_id3tag_set_title, __lsse_id3tag_set_artist, __lsse_id3tag_set_album,
    __lsse_id3tag_set_year, __lsse_id3tag_set_comment,
    __lsse_id3tag_set_track, __lsse_id3tag_set_genre,
    __lsse_lame_get_id3v2_tag, __lsse_lame_get_id3v1_tag,
};

/* One-time selection. SUPERLAME_ENGINE env var overrides the CPUID probe:
 *   "sse2"   -> force the generic fallback (lets us exercise it on any CPU)
 *   "znver3" -> force the AVX2 path
 * Otherwise: znver3 if the CPU has AVX2+FMA+BMI2, else sse2. */
static const LameEngine &Choose() {
    const char *force = getenv("SUPERLAME_ENGINE");
    if (force) {
        if (strcmp(force, "sse2")   == 0) return kSse2;
        if (strcmp(force, "znver3") == 0) return kZnver3;
    }
    return HasZnver3Features() ? kZnver3 : kSse2;
}

const LameEngine &SelectLameEngine() {
    static const LameEngine &chosen = Choose();
    return chosen;
}
