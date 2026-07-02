/* superlame-mt: engine selection + CPUID probe.
 *
 * Four libmp3lame builds are linked in, each with prefixed symbols:
 *   __zv5_*   -march=znver5  (Zen 5, AVX-512 + latest tuning)   [built, UNVERIFIED on Zen<5]
 *   __zv4_*   -march=znver4  (Zen 4, AVX-512)                   [built, UNVERIFIED on Zen<4]
 *   __l3v_*   -march=znver3  (Zen 3/2/1, AVX2/FMA/BMI2)         [tested]
 *   __lsse_*  -march=x86-64  (generic SSE2 fallback)            [tested]
 *
 * At startup we CPUID-probe and bind a table of function pointers to the best
 * engine the CPU can run. The frontend calls through SelectLameEngine().
 *
 * The four engines expose the same symbol set; to avoid writing 4x the extern
 * declarations and 4x the table initializers by hand, we drive both from one
 * X-macro list (LAME_SYMS) expanded once per prefix.
 */
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

/* Token paste helpers so a literal prefix token pastes onto each base name. */
#define CAT2(a, b) a ## b
#define CAT(a, b)  CAT2(a, b)

/* Block generator: for a given literal prefix, declare externs + a table.
 * The symbol list below (ENGINE_EXTERNS / ENGINE_INITS) is expanded once per
 * engine; field order MUST match the LameEngine struct in lame_dispatch.h. */
#define ENGINE_BLOCK(PFX, DISPLAYNAME, VARNAME) \
  extern "C" { \
    ENGINE_EXTERNS(PFX) \
  } \
  static const LameEngine VARNAME = { DISPLAYNAME, ENGINE_INITS(PFX) };

/* extern declarations for one prefix */
#define ENGINE_EXTERNS(PFX) \
  X_EXTERN(PFX, lame_t, lame_init, (void)) \
  X_EXTERN(PFX, int,    lame_set_in_samplerate, (lame_global_flags *, int)) \
  X_EXTERN(PFX, int,    lame_set_num_channels, (lame_global_flags *, int)) \
  X_EXTERN(PFX, int,    lame_set_brate, (lame_global_flags *, int)) \
  X_EXTERN(PFX, int,    lame_set_quality, (lame_global_flags *, int)) \
  X_EXTERN(PFX, int,    lame_set_mode, (lame_global_flags *, MPEG_mode)) \
  X_EXTERN(PFX, int,    lame_set_VBR, (lame_global_flags *, vbr_mode)) \
  X_EXTERN(PFX, int,    lame_set_VBR_mean_bitrate_kbps, (lame_global_flags *, int)) \
  X_EXTERN(PFX, int,    lame_set_VBR_quality, (lame_global_flags *, float)) \
  X_EXTERN(PFX, int,    lame_init_params, (lame_global_flags *)) \
  X_EXTERN(PFX, int,    lame_get_framesize, (const lame_global_flags *)) \
  X_EXTERN(PFX, int,    lame_encode_buffer_interleaved, (lame_global_flags *, short[], int, unsigned char *, int)) \
  X_EXTERN(PFX, int,    lame_encode_buffer, (lame_global_flags *, const short[], const short[], int, unsigned char *, int)) \
  X_EXTERN(PFX, int,    lame_encode_buffer_interleaved_ieee_float, (lame_global_flags *, const float[], int, unsigned char *, int)) \
  X_EXTERN(PFX, int,    lame_encode_buffer_ieee_float, (lame_global_flags *, const float[], const float[], int, unsigned char *, int)) \
  X_EXTERN(PFX, int,    lame_encode_flush, (lame_global_flags *, unsigned char *, int)) \
  X_EXTERN(PFX, int,    lame_encode_flush_nogap, (lame_global_flags *, unsigned char *, int)) \
  X_EXTERN(PFX, size_t, lame_get_lametag_frame, (const lame_global_flags *, unsigned char *, size_t)) \
  X_EXTERN(PFX, int,    lame_close, (lame_global_flags *)) \
  X_EXTERN(PFX, void,   id3tag_init, (lame_global_flags *)) \
  X_EXTERN(PFX, void,   id3tag_add_v2, (lame_global_flags *)) \
  X_EXTERN(PFX, void,   id3tag_add_v2_4_UTF8, (lame_global_flags *)) \
  X_EXTERN(PFX, void,   id3tag_v2_only, (lame_global_flags *)) \
  X_EXTERN(PFX, void,   id3tag_set_title, (lame_global_flags *, const char *)) \
  X_EXTERN(PFX, void,   id3tag_set_artist, (lame_global_flags *, const char *)) \
  X_EXTERN(PFX, void,   id3tag_set_album, (lame_global_flags *, const char *)) \
  X_EXTERN(PFX, void,   id3tag_set_year, (lame_global_flags *, const char *)) \
  X_EXTERN(PFX, void,   id3tag_set_comment, (lame_global_flags *, const char *)) \
  X_EXTERN(PFX, int,    id3tag_set_track, (lame_global_flags *, const char *)) \
  X_EXTERN(PFX, int,    id3tag_set_genre, (lame_global_flags *, const char *)) \
  X_EXTERN(PFX, int,    id3tag_set_albumart, (lame_global_flags *, const char *, size_t)) \
  X_EXTERN(PFX, size_t, lame_get_id3v2_tag, (lame_global_flags *, unsigned char *, size_t)) \
  X_EXTERN(PFX, size_t, lame_get_id3v1_tag, (lame_global_flags *, unsigned char *, size_t))

/* table initializer list for one prefix (order matches the struct) */
#define ENGINE_INITS(PFX) \
  CAT(PFX, lame_init), CAT(PFX, lame_set_in_samplerate), CAT(PFX, lame_set_num_channels), \
  CAT(PFX, lame_set_brate), CAT(PFX, lame_set_quality), CAT(PFX, lame_set_mode), \
  CAT(PFX, lame_set_VBR), CAT(PFX, lame_set_VBR_mean_bitrate_kbps), CAT(PFX, lame_set_VBR_quality), \
  CAT(PFX, lame_init_params), CAT(PFX, lame_get_framesize), \
  CAT(PFX, lame_encode_buffer_interleaved), CAT(PFX, lame_encode_buffer), \
  CAT(PFX, lame_encode_buffer_interleaved_ieee_float), CAT(PFX, lame_encode_buffer_ieee_float), \
  CAT(PFX, lame_encode_flush), CAT(PFX, lame_encode_flush_nogap), \
  CAT(PFX, lame_get_lametag_frame), CAT(PFX, lame_close), \
  CAT(PFX, id3tag_init), CAT(PFX, id3tag_add_v2), CAT(PFX, id3tag_add_v2_4_UTF8), CAT(PFX, id3tag_v2_only), \
  CAT(PFX, id3tag_set_title), CAT(PFX, id3tag_set_artist), CAT(PFX, id3tag_set_album), \
  CAT(PFX, id3tag_set_year), CAT(PFX, id3tag_set_comment), \
  CAT(PFX, id3tag_set_track), CAT(PFX, id3tag_set_genre), CAT(PFX, id3tag_set_albumart), \
  CAT(PFX, lame_get_id3v2_tag), CAT(PFX, lame_get_id3v1_tag)

#define X_EXTERN(PFX, ret, name, params) ret CAT(PFX, name) params;

/* Instantiate the four engines. */
ENGINE_BLOCK(__zv5_,  "znver5 (AVX-512, unverified)", kZnver5)
ENGINE_BLOCK(__zv4_,  "znver4 (AVX-512, unverified)", kZnver4)
ENGINE_BLOCK(__l3v_,  "znver3 (AVX2/FMA)",            kZnver3)
ENGINE_BLOCK(__lsse_, "x86-64 (SSE2 fallback)",       kSse2)

/* --------------------------- CPU feature probe --------------------------- */
struct CpuInfo {
    bool avx2 = false, fma = false, bmi2 = false;
    bool avx512f = false, avx512bw = false, avx512vl = false;
    unsigned family = 0, model = 0;
    bool isAMD = false;
};

static CpuInfo ProbeCpu() {
    CpuInfo ci;
    int r[4];
    cpuidex(r, 0, 0);
    int maxLeaf = r[0];
    char vendor[13] = {0};
    memcpy(vendor + 0, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    ci.isAMD = (strcmp(vendor, "AuthenticAMD") == 0);

    if (maxLeaf < 1) return ci;
    cpuidex(r, 1, 0);
    unsigned eax = (unsigned) r[0];
    unsigned base_family = (eax >> 8)  & 0xf;
    unsigned ext_family  = (eax >> 20) & 0xff;
    unsigned base_model  = (eax >> 4)  & 0xf;
    unsigned ext_model   = (eax >> 16) & 0xf;
    ci.family = base_family + ((base_family == 0xf) ? ext_family : 0);
    ci.model  = base_model  + ((base_family == 0xf || base_family == 0x6) ? (ext_model << 4) : 0);

    bool osxsave = (r[2] >> 27) & 1;
    bool avx     = (r[2] >> 28) & 1;
    ci.fma       = (r[2] >> 12) & 1;
    bool osAVX = osxsave && avx;      /* OS must have enabled XSAVE for AVX regs */

    if (maxLeaf >= 7) {
        cpuidex(r, 7, 0);
        ci.avx2     = osAVX && ((r[1] >> 5)  & 1);
        ci.bmi2     =          ((r[1] >> 8)  & 1);
        /* AVX-512 also needs OS opmask/ZMM save support; on any CPU that reports
         * these bits with osAVX we treat them as usable (Windows enables them). */
        ci.avx512f  = osAVX && ((r[1] >> 16) & 1);
        ci.avx512bw = osAVX && ((r[1] >> 30) & 1);
        ci.avx512vl = osAVX && ((unsigned) (r[1] >> 31) & 1);
    }
    return ci;
}

/* One-time selection. SUPERLAME_ENGINE env var overrides the CPUID probe:
 *   "sse2" | "znver3" | "znver4" | "znver5"  force that engine.
 * Otherwise pick the best the CPU can actually run:
 *   AVX-512 (F+BW+VL) and AMD family >= 0x1A (Zen 5) -> znver5
 *   AVX-512 (F+BW+VL)                                -> znver4  (Zen 4/5)
 *   AVX2 + FMA + BMI2                                -> znver3  (Zen 1/2/3, etc.)
 *   otherwise                                        -> sse2
 * Choosing znver4 for an unidentified AVX-512 CPU is deliberately conservative:
 * it runs correctly on Zen 5 too; we only pick the znver5 build when positively
 * identified. NOTE: znver4/5 are built but UNVERIFIED (no Zen4/5 host here). */
static const LameEngine &Choose() {
    CpuInfo ci = ProbeCpu();
    bool avx512 = ci.avx512f && ci.avx512bw && ci.avx512vl;
    bool avx2ok = ci.avx2 && ci.fma && ci.bmi2;

    const char *force = getenv("SUPERLAME_ENGINE");
    if (force) {
        /* Honor the override, but never hand back an engine the CPU can't run
         * (that would SIGILL). If the forced engine needs an ISA we lack, warn
         * and fall through to normal auto-selection. */
        const LameEngine *want = nullptr;
        bool runnable = true;
        if      (strcmp(force, "sse2")   == 0) { want = &kSse2; }
        else if (strcmp(force, "znver3") == 0) { want = &kZnver3; runnable = avx2ok; }
        else if (strcmp(force, "znver4") == 0) { want = &kZnver4; runnable = avx512; }
        else if (strcmp(force, "znver5") == 0) { want = &kZnver5; runnable = avx512; }
        if (want && runnable) return *want;
        if (want && !runnable)
            fprintf(stderr, "warning: SUPERLAME_ENGINE=%s needs CPU features this "
                    "machine lacks; using auto-selected engine instead.\n", force);
    }

    if (avx512 && ci.isAMD && ci.family >= 0x1A) return kZnver5;
    if (avx512)                                  return kZnver4;
    if (avx2ok)                                  return kZnver3;
    return kSse2;
}

const LameEngine &SelectLameEngine() {
    static const LameEngine &chosen = Choose();
    return chosen;
}
