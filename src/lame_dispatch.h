/* superlame-mt: runtime engine dispatch.
 *
 * Two libmp3lame builds are linked into the binary with prefixed symbols:
 *   __l3v_*   -> compiled -march=znver3 (AVX2/FMA path)
 *   __lsse_*  -> compiled -march=x86-64 (generic SSE2 fallback)
 *
 * At startup we CPUID-probe for the Zen3-relevant feature set (AVX2) and bind a
 * table of function pointers to the chosen engine. The frontend calls through
 * LAME_DISPATCH(name)(...) instead of lame_name(...).
 */
#ifndef SUPERLAME_DISPATCH_H
#define SUPERLAME_DISPATCH_H

#include <lame.h>

/* The subset of the LAME API the SuperFast frontend uses. */
struct LameEngine {
    const char *name;
    lame_t (*init)(void);
    int    (*set_in_samplerate)(lame_global_flags *, int);
    int    (*set_out_samplerate)(lame_global_flags *, int);
    int    (*get_out_samplerate)(const lame_global_flags *);
    int    (*set_num_channels)(lame_global_flags *, int);
    int    (*set_brate)(lame_global_flags *, int);
    int    (*set_quality)(lame_global_flags *, int);
    int    (*set_mode)(lame_global_flags *, MPEG_mode);
    int    (*set_VBR)(lame_global_flags *, vbr_mode);
    int    (*set_VBR_mean_bitrate_kbps)(lame_global_flags *, int);
    int    (*set_VBR_quality)(lame_global_flags *, float);
    int    (*init_params)(lame_global_flags *);
    int    (*get_framesize)(const lame_global_flags *);
    int    (*encode_buffer_interleaved)(lame_global_flags *, short[], int, unsigned char *, int);
    int    (*encode_buffer)(lame_global_flags *, const short[], const short[], int, unsigned char *, int);
    /* Full-precision float input (normalized +/-1.0), for >16-bit sources. */
    int    (*encode_buffer_interleaved_ieee_float)(lame_global_flags *, const float[], int, unsigned char *, int);
    int    (*encode_buffer_ieee_float)(lame_global_flags *, const float[], const float[], int, unsigned char *, int);
    int    (*encode_flush)(lame_global_flags *, unsigned char *, int);
    int    (*encode_flush_nogap)(lame_global_flags *, unsigned char *, int);
    size_t (*get_lametag_frame)(const lame_global_flags *, unsigned char *, size_t);
    int    (*close)(lame_global_flags *);

    /* ID3 tagging (frontend renders tags around the assembled stream). */
    void   (*id3tag_init)(lame_global_flags *);
    void   (*id3tag_add_v2)(lame_global_flags *);
    void   (*id3tag_add_v2_4_UTF8)(lame_global_flags *);
    void   (*id3tag_v2_only)(lame_global_flags *);
    void   (*id3tag_set_title)(lame_global_flags *, const char *);
    void   (*id3tag_set_artist)(lame_global_flags *, const char *);
    void   (*id3tag_set_album)(lame_global_flags *, const char *);
    void   (*id3tag_set_year)(lame_global_flags *, const char *);
    void   (*id3tag_set_comment)(lame_global_flags *, const char *);
    int    (*id3tag_set_track)(lame_global_flags *, const char *);
    int    (*id3tag_set_genre)(lame_global_flags *, const char *);
    int    (*id3tag_set_albumart)(lame_global_flags *, const char *, size_t);
    size_t (*lame_get_id3v2_tag)(lame_global_flags *, unsigned char *, size_t);
    size_t (*lame_get_id3v1_tag)(lame_global_flags *, unsigned char *, size_t);
};

/* Selects and returns the active engine (probes CPUID once). */
const LameEngine &SelectLameEngine();

#endif
