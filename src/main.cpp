/* superlame-mt: standalone SuperFast multithreaded LAME MP3 encoder.
 *
 * Orchestration ported from SuperFast lame.cpp (EncoderLAME), (C) 2007-2018
 * Robert Kausch, GPL v2+. Reads a PCM WAV, encodes via N parallel libmp3lame
 * instances on overlapping chunks, reassembles a bit-reservoir-correct MP3.
 */
#include "worker.h"
#include "repacker.h"
#include "iodriver.h"
#include "lame_dispatch.h"
#include "mp3frame.h"
#include "decode.h"
#include "resample.h"
#include "tags.h"
#include "flac_decode.h"

#include <lame.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <thread>
#include <algorithm>
#include <chrono>
#include <new>

/* --------------------- Unicode-safe path handling ---------------------- *
 * On Windows the ANSI argv (main's char**) cannot represent characters
 * outside the active code page (e.g. the U+221E infinity in a filename), so
 * paths arrive corrupted. We instead take the original UTF-16 command line,
 * carry path args as UTF-8, and open files via _wfopen after converting back
 * to UTF-16. On non-Windows, fopen with the UTF-8 bytes is already correct. */
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX          /* keep windows.h from clobbering std::min/std::max */
#endif
#include <windows.h>
static std::wstring utf8_to_w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
static std::string w_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
#include <io.h>
#include <fcntl.h>
static FILE *open_utf8(const char *path, const wchar_t *mode) {
    /* "-" means stdin (read) or stdout (write); use it in binary mode. */
    if (path[0] == '-' && path[1] == '\0') {
        bool reading = (mode[0] == L'r');
        FILE *f = reading ? stdin : stdout;
        _setmode(_fileno(f), _O_BINARY);
        return f;
    }
    return _wfopen(utf8_to_w(path).c_str(), mode);
}
#else
static FILE *open_utf8(const char *path, const char *mode) {
    if (path[0] == '-' && path[1] == '\0') return (mode[0] == 'r') ? stdin : stdout;
    return fopen(path, mode);
}
#endif

/* 64-bit file size via the current position (ftell overflows a 32-bit long on
 * Windows for files > 2 GB, which otherwise silently corrupts large reads). */
static int64_t FileSize64(FILE *f) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) return -1;
    int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    return n;
#else
    if (fseeko(f, 0, SEEK_END) != 0) return -1;
    int64_t n = (int64_t) ftello(f);
    fseeko(f, 0, SEEK_SET);
    return n;
#endif
}

/* ------------------------------ RAM guard ------------------------------- *
 * The encoder holds the whole input, its float expansion, the resampler
 * scratch, and the whole output MP3 in RAM. This estimates peak usage from the
 * input size and refuses (or warns) up front instead of crashing on bad_alloc. */
static uint64_t AvailablePhysicalRAM() {
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) return (uint64_t) ms.ullAvailPhys;
    return 0;
#else
    long pages = sysconf(_SC_AVPHYS_PAGES), ps = sysconf(_SC_PAGE_SIZE);
    return (pages > 0 && ps > 0) ? (uint64_t) pages * ps : 0;
#endif
}

/* Rough peak-RAM estimate for encoding an input of `fileBytes`, at `bits` depth.
 * >16-bit sources expand to float and (if resampling) get double-precision
 * scratch, so the multiplier is larger. Conservative on purpose. */
static uint64_t EstimatePeakRAM(uint64_t fileBytes, int bits, bool willResample) {
    /* input buffer (1x) is always held. */
    double mult = 1.0;
    if (bits > 16) {
        /* 24/32-bit -> float samples ~ (32/bits)x the packed data, plus we keep
         * the raw input during conversion. */
        mult += (bits == 24) ? 4.0 / 3.0 : 1.0;   /* float buffer */
        if (willResample) mult += 3.0;            /* per-channel double in+out scratch */
    } else {
        mult += 1.0;                              /* short samples buffer */
        if (willResample) mult += 3.0;
    }
    mult += 0.35;                                 /* output MP3 (worst-case-ish) */
    return (uint64_t) (fileBytes * mult);
}

/* --------------------- output frame-chain validation ------------------- *
 * Walk the encoded MP3 frame by frame, following each frame's size to the
 * next. Returns false if the chain ever lands on something that isn't a valid
 * frame header (i.e. the repacker desynced the stream). Skips a leading
 * Info/Xing/ID3v2 frame naturally since those are valid MPEG frames too. */
static bool FrameChainValid(const std::vector<unsigned char> &d) {
    int64_t sz = (int64_t) d.size();
    long pos = 0;
    /* Skip an ID3v2 tag if present. */
    if (sz > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3')
        pos = 10 + (((d[6] & 0x7F) << 21) | ((d[7] & 0x7F) << 14) | ((d[8] & 0x7F) << 7) | (d[9] & 0x7F));
    /* Find first sync. */
    while (pos + 4 < sz && !mp3::IsValidFrame(d.data() + pos)) pos++;
    if (pos + 4 >= sz) return false;          // no frames at all
    while (pos + 4 <= sz) {
        if (!mp3::IsValidFrame(d.data() + pos)) return false;
        int fb = mp3::GetFrameSize(d.data() + pos);
        if (fb < 24) return false;
        pos += fb;
    }
    return true;
}

/* ------------------------------ WAV reader ------------------------------ */
/* WavData lives in audiodata.h (shared with flac_decode.h). */

static uint32_t rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
static uint16_t rd16(const u8 *p) { return p[0] | (p[1] << 8); }

/* Identify an image by magic bytes. Returns "image/jpeg" / "image/png" for the
 * two formats MP3 players actually render as APIC cover art, or nullptr for
 * anything else (WebP/JXL/GIF/BMP/...) which we deliberately refuse rather than
 * embed art no player will show. */
static const char *SniffCoverMime(const std::vector<unsigned char> &b) {
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "image/jpeg";
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47 &&
        b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A)  return "image/png";
    return nullptr;
}

/* Clamp a 32-bit value into 16-bit signed range. (Retained for reference.) */
[[maybe_unused]] static inline short clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (short) v;
}

static bool ReadAiff(const std::vector<u8> &buf, WavData &w);   /* fwd decl */

/* FLAC decode is routed through ReadWav (same in-RAM buffer). ReadWav's
 * signature is fixed (used in the RAM-guard peek too), so the decode knobs and
 * the metadata/art it recovers are carried in file-scope state set/read by
 * main() around the call. */
static int      g_flacThreads = 1;      /* set before ReadWav; MT decode width */
static bool     g_flacQuiet   = false;
static FlacMeta g_flacMeta;             /* embedded tags + front-cover art */
static bool     g_inputWasFlac = false; /* true if the last ReadWav decoded FLAC */

/* WAV reader accepting 8/16/24/32-bit integer PCM and 32/64-bit IEEE float,
 * including WAVE_FORMAT_EXTENSIBLE, any sample rate, mono or stereo. Everything
 * is converted to 16-bit signed interleaved samples (what the encoder takes).
 * 16-bit PCM is copied verbatim so that path stays bit-identical to before.
 * Also transparently dispatches to the AIFF parser for FORM/AIFF input. */
static bool ReadWav(const char *path, WavData &w) {
#ifdef _WIN32
    FILE *f = open_utf8(path, L"rb");
#else
    FILE *f = open_utf8(path, "rb");
#endif
    if (!f) { fprintf(stderr, "cannot open input: %s\n", path); return false; }
    bool isStdin = (path[0] == '-' && path[1] == '\0');
    std::vector<u8> buf;
    int64_t sz2 = isStdin ? -1 : FileSize64(f);
    if (!isStdin && sz2 >= 0) {
        buf.resize((size_t) sz2);
        if (sz2 > 0 && fread(buf.data(), 1, (size_t) sz2, f) != (size_t) sz2) { fclose(f); return false; }
        fclose(f);
    } else {
        /* Non-seekable (stdin/pipe): read to EOF. */
        unsigned char chunk[64 * 1024];
        size_t r;
        while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) buf.insert(buf.end(), chunk, chunk + r);
        if (!isStdin) fclose(f);
    }
    int64_t sz = (int64_t) buf.size();

    /* AIFF (big-endian) is handled by a separate parser on the same bytes. */
    if (sz >= 12 && memcmp(buf.data(), "FORM", 4) == 0 && memcmp(buf.data() + 8, "AIFF", 4) == 0)
        return ReadAiff(buf, w);

    /* FLAC: "fLaC" magic. Decode from the in-RAM buffer into the float path. */
    if (sz >= 4 && memcmp(buf.data(), "fLaC", 4) == 0) {
        g_flacMeta = FlacMeta{};
        bool ok = DecodeFlac(buf, w, g_flacMeta, g_flacThreads, g_flacQuiet);
        g_inputWasFlac = ok;
        return ok;
    }

    if (sz < 44 || memcmp(buf.data(), "RIFF", 4) != 0 || memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "input is not a WAV or AIFF file\n"); return false;
    }

    int  fmtTag = 0, bits = 0;
    bool isFloat = false, haveFmt = false;

    uint64_t usz = buf.size();
    uint64_t p = 12;
    while (p + 8 <= usz) {
        const u8 *ck = buf.data() + p;
        uint64_t cksz = rd32(ck + 4);
        uint64_t body = p + 8;
        uint64_t avail = usz - body;
        if (cksz > avail) cksz = avail;              /* clamp lying chunk size */

        if (memcmp(ck, "fmt ", 4) == 0) {
            if (cksz < 16) { fprintf(stderr, "WAV: short fmt chunk\n"); return false; }
            fmtTag     = rd16(ck + 8);
            w.channels = rd16(ck + 8 + 2);
            w.rate     = rd32(ck + 8 + 4);
            bits       = rd16(ck + 8 + 14);
            /* WAVE_FORMAT_EXTENSIBLE: real format is the SubFormat GUID's first
             * 2 bytes (at fmt+24); only read it if the chunk is that long. */
            if (fmtTag == 0xFFFE && cksz >= 26) fmtTag = rd16(ck + 8 + 24);
            isFloat = (fmtTag == 3);
            haveFmt = true;
            w.bits  = bits;

        } else if (memcmp(ck, "data", 4) == 0) {
            if (!haveFmt) { fprintf(stderr, "WAV has no fmt chunk\n"); return false; }
            if (fmtTag != 1 && fmtTag != 3) {
                fprintf(stderr, "unsupported WAV encoding (format tag %d); only PCM and IEEE float\n", fmtTag);
                return false;
            }
            if (w.channels < 1 || w.channels > 2) {
                fprintf(stderr, "unsupported channel count %d (mono or stereo only)\n", w.channels);
                return false;
            }
            if (w.rate < 1 || w.rate > 384000) {
                fprintf(stderr, "unsupported sample rate %d\n", w.rate);
                return false;
            }

            const u8 *d = ck + 8;
            size_t dataBytes = (size_t) cksz;        /* already clamped to avail */
            int bytesPerSample = bits / 8;
            if (bytesPerSample < 1) { fprintf(stderr, "bad bit depth %d\n", bits); return false; }
            size_t nSamples = dataBytes / bytesPerSample;

            /* 8 and 16-bit sources fit losslessly in 16-bit shorts -> short path,
             * which stays byte-for-byte identical to the original reader/encoder.
             * 24/32-bit and float carry more than 16 bits, so they go through the
             * full-precision normalized-float path (LAME's ieee_float encoder),
             * preserving resolution all the way to MP3 quantization. */
            if (!isFloat && (bits == 8 || bits == 16)) {
                w.useFloat = false;
                w.samples.resize(nSamples);
                if (bits == 16) {
                    memcpy(w.samples.data(), d, nSamples * 2);          /* exact */
                } else {
                    for (size_t i = 0; i < nSamples; i++)               /* u8, centred 128 */
                        w.samples[i] = (short) (((int) d[i] - 128) << 8);
                }
            } else {
                w.useFloat = true;
                w.samplesF.resize(nSamples);
                if (!isFloat && bits == 24) {
                    for (size_t i = 0; i < nSamples; i++) {
                        const u8 *s = d + i * 3;
                        int32_t v = (s[0] << 8) | (s[1] << 16) | ((int32_t)(int8_t) s[2] << 24);
                        w.samplesF[i] = (float) v / 2147483648.0f;     /* /2^31 -> +/-1 */
                    }
                } else if (!isFloat && bits == 32) {
                    for (size_t i = 0; i < nSamples; i++)
                        w.samplesF[i] = (float) ((int32_t) rd32(d + i * 4)) / 2147483648.0f;
                } else if (isFloat && bits == 32) {
                    memcpy(w.samplesF.data(), d, nSamples * 4);        /* already +/-1 float */
                } else if (isFloat && bits == 64) {
                    for (size_t i = 0; i < nSamples; i++) {
                        double dv; memcpy(&dv, d + i * 8, 8);
                        w.samplesF[i] = (float) dv;
                    }
                } else {
                    fprintf(stderr, "unsupported WAV: %d-bit %s\n", bits, isFloat ? "float" : "PCM");
                    return false;
                }
            }
            return true;
        }
        uint64_t adv = 8 + cksz + (cksz & 1);
        if (adv < 8) break;                          /* overflow paranoia */
        p += adv;
    }
    fprintf(stderr, "no data chunk\n");
    return false;
}

/* Big-endian readers for AIFF. */
static uint32_t rd32be(const u8 *p) { return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t rd16be(const u8 *p) { return (p[0] << 8) | p[1]; }

/* Decode an 80-bit IEEE-754 extended float (AIFF sample rate field) to int. */
static long aiffRate(const u8 *p) {
    int expo = (((p[0] & 0x7F) << 8) | p[1]) - 16383 - 63;
    uint64_t mant = 0;
    for (int i = 0; i < 8; i++) mant = (mant << 8) | p[2 + i];
    return (long) (ldexp((double) mant, expo) + 0.5);
}

/* AIFF reader (16/24-bit big-endian PCM, mono/stereo). Converts to the same
 * WavData as ReadWav: 16-bit -> short path, 24-bit -> full-precision float. */
static bool ReadAiff(const std::vector<u8> &buf, WavData &w) {
    uint64_t sz = buf.size();
    if (sz < 12 || memcmp(buf.data(), "FORM", 4) != 0 || memcmp(buf.data() + 8, "AIFF", 4) != 0)
        return false;

    int bits = 0; bool haveComm = false;
    uint64_t p = 12;
    while (p + 8 <= sz) {
        const u8 *ck = buf.data() + p;
        uint64_t cksz = rd32be(ck + 4);
        uint64_t body = p + 8;                       /* offset of chunk body */
        uint64_t avail = sz - body;                  /* bytes actually present */
        if (cksz > avail) cksz = avail;              /* clamp lying chunk size */

        if (memcmp(ck, "COMM", 4) == 0) {
            if (cksz < 18) { fprintf(stderr, "AIFF: short COMM chunk\n"); return false; }
            w.channels = rd16be(ck + 8);
            bits       = rd16be(ck + 8 + 6);
            w.rate     = (int) aiffRate(ck + 8 + 8);
            if (w.rate < 1 || w.rate > 384000) { fprintf(stderr, "AIFF: bad sample rate %d\n", w.rate); return false; }
            haveComm = true;
        } else if (memcmp(ck, "SSND", 4) == 0) {
            if (!haveComm) { fprintf(stderr, "AIFF: no COMM chunk\n"); return false; }
            if (w.channels < 1 || w.channels > 2) { fprintf(stderr, "AIFF: only mono/stereo\n"); return false; }
            if (cksz < 8) { fprintf(stderr, "AIFF: short SSND chunk\n"); return false; }
            uint64_t soff = rd32be(ck + 8);          /* offset to first sample frame */
            if (soff > cksz - 8) { fprintf(stderr, "AIFF: bad SSND offset\n"); return false; }
            const u8 *d = ck + 8 + 8 + soff;
            uint64_t dataBytes = cksz - 8 - soff;    /* now guaranteed non-negative */
            int bps = bits / 8;
            if (bps < 1) { fprintf(stderr, "AIFF: bad bit depth %d\n", bits); return false; }
            size_t n = (size_t) (dataBytes / bps);
            if (bits == 16) {
                w.useFloat = false; w.samples.resize(n);
                for (size_t i = 0; i < n; i++) w.samples[i] = (short) rd16be(d + i * 2);
            } else if (bits == 24) {
                w.useFloat = true; w.samplesF.resize(n);
                for (size_t i = 0; i < n; i++) {
                    const u8 *s = d + i * 3;
                    int32_t v = ((int32_t)(int8_t) s[0] << 24) | (s[1] << 16) | (s[2] << 8); /* BE, sign-extended */
                    w.samplesF[i] = (float) v / 2147483648.0f;
                }
            } else {
                fprintf(stderr, "AIFF: unsupported %d-bit (16/24 only)\n", bits); return false;
            }
            return true;
        }
        /* Advance by the (clamped) chunk plus AIFF's pad byte, guarding overflow. */
        uint64_t adv = 8 + cksz + (cksz & 1);
        if (adv < 8) break;                          /* paranoia */
        p += adv;
    }
    fprintf(stderr, "AIFF: no SSND chunk\n");
    return false;
}

/* --------------------------- Encoder orchestrator ---------------------- */
class SuperEncoder {
    EncoderConfig             config;
    MemDriver                *driver;
    std::vector<SuperWorker*> workers;
    SuperRepacker            *repacker = nullptr;

    int   frameSize    = 0;
    int   blockSize    = 128;
    int   overlap      = 4;
    int   nextWorker   = 0;
    int64_t totalSamples = 0;

    int   sampleBytes = 2;                    // 2 (short) or 4 (float) per sample
    std::vector<unsigned char> samplesBuffer; // raw interleaved samples

    int  ProcessPackets(const std::vector<unsigned char> &data, const std::vector<int> &chunkSizes,
                        bool first, int &processed, bool &complete);
    int  ProcessResults(SuperWorker *worker, bool first);
    int  EncodeFrames(bool flush);

public:
    SuperEncoder(const EncoderConfig &cfg, MemDriver *drv, int numThreads);
    ~SuperEncoder();

    /* data is raw interleaved samples (short or float per cfg.useFloat);
     * numSamples is the element count. */
    void WriteData(const void *data, int numSamples);
    void Finish();
};

SuperEncoder::SuperEncoder(const EncoderConfig &cfg, MemDriver *drv, int numThreads) {
    config = cfg;
    driver = drv;
    sampleBytes = cfg.useFloat ? (int) sizeof(float) : (int) sizeof(short);

    /* Determine frameSize from a throwaway context (via the selected engine). */
    const LameEngine &e = SelectLameEngine();
    lame_t probe = e.init();
    e.set_in_samplerate(probe, config.rate);
    e.set_num_channels(probe, config.channels);
    if (config.vbrMode == vbr_off) e.set_brate(probe, config.bitrate);
    else if (config.vbrMode == vbr_abr) { e.set_VBR(probe, vbr_abr); e.set_VBR_mean_bitrate_kbps(probe, config.abrBitrate); }
    else { e.set_VBR(probe, vbr_mtrh); e.set_VBR_quality(probe, config.vbrQuality / 10.0); }
    e.init_params(probe);
    frameSize = e.get_framesize(probe);
    e.close(probe);

    /* Overlap: disabled for single thread, else ~4*1152/frameSize frames. */
    if (numThreads == 1) overlap = 0;
    else                 overlap = 4 * 1152 / frameSize;

    for (int i = 0; i < numThreads; i++) workers.push_back(new SuperWorker(config, overlap));
    for (auto *w : workers) w->Start();

    repacker = new SuperRepacker(driver);
    repacker->EnableRateControl(8000, 320000);
}

SuperEncoder::~SuperEncoder() {
    delete repacker;
    for (auto *w : workers) delete w;
}

void SuperEncoder::WriteData(const void *data, int numSamples) {
    size_t prevBytes = samplesBuffer.size();
    samplesBuffer.resize(prevBytes + (size_t) numSamples * sampleBytes);
    memcpy(samplesBuffer.data() + prevBytes, data, (size_t) numSamples * sampleBytes);
    totalSamples += numSamples / config.channels;
    EncodeFrames(false);
}

int SuperEncoder::EncodeFrames(bool flush) {
    int framesToProcess = blockSize;
    int framesProcessed = 0;
    int dataLength      = 0;
    int samplesPerFrame = frameSize * config.channels;
    int bufSamples      = (int) (samplesBuffer.size() / sampleBytes);

    if (flush) framesToProcess = (int) std::floor((double) bufSamples / samplesPerFrame);

    /* Degenerate input: fewer than one full frame buffered. Don't run the
     * parallel block loop (its overlap math goes negative and the memmove
     * underflows). On flush, hand the leftover (possibly 0) straight to one
     * worker with the flush flag so the encoder emits its final frame cleanly. */
    if (framesToProcess <= 0) {
        if (flush) {
            SuperWorker *w0 = workers[nextWorker % workers.size()];
            while (!w0->IsReady()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            w0->Lock();
            if (w0->GetPacketSizes().size() != 0)
                dataLength += ProcessResults(w0, nextWorker == (int) workers.size());
            w0->Encode(samplesBuffer.data(), 0, bufSamples, true);
            w0->Release();
            nextWorker++;
            samplesBuffer.clear();
            /* fall through to the drain loop below */
        } else {
            return 0;   /* not flushing yet: keep buffering until we have a frame */
        }
    } else
    while (bufSamples - framesProcessed * samplesPerFrame >= samplesPerFrame * framesToProcess) {
        SuperWorker *workerToUse = workers[nextWorker % workers.size()];

        while (!workerToUse->IsReady()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

        workerToUse->Lock();
        if (workerToUse->GetPacketSizes().size() != 0)
            dataLength += ProcessResults(workerToUse, nextWorker == (int) workers.size());

        workerToUse->Encode(samplesBuffer.data(), framesProcessed * samplesPerFrame,
                            flush ? bufSamples : samplesPerFrame * framesToProcess, flush);
        workerToUse->Release();

        framesProcessed += framesToProcess - overlap;
        nextWorker++;

        if (flush) break;
    }

    size_t processedBytes = (size_t) framesProcessed * samplesPerFrame * sampleBytes;
    memmove(samplesBuffer.data(), samplesBuffer.data() + processedBytes,
            samplesBuffer.size() - processedBytes);
    samplesBuffer.resize(samplesBuffer.size() - processedBytes);

    if (!flush) return dataLength;

    /* Drain remaining workers. */
    for (int i = 0; i < (int) workers.size(); i++) {
        SuperWorker *workerToUse = workers[nextWorker % workers.size()];
        while (!workerToUse->IsReady()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        workerToUse->Lock();
        if (workerToUse->GetPacketSizes().size() != 0)
            dataLength += ProcessResults(workerToUse, nextWorker == (int) workers.size());
        workerToUse->Release();
        nextWorker++;
    }

    repacker->Flush();
    return dataLength;
}

int SuperEncoder::ProcessResults(SuperWorker *worker, bool first) {
    int  processed = 0;
    bool complete  = false;

    if (workers.size() == 1)
        return ProcessPackets(worker->GetPackets(), worker->GetPacketSizes(), first, processed, complete);

    int dataLength = 0;
    for (int round = 0; !complete; round++) {
        dataLength += ProcessPackets(worker->GetPackets(), worker->GetPacketSizes(), first, processed, complete);

        if (round & 4 && overlap > 0) { round = 0; overlap--; processed++; }
        if (!complete) worker->ReEncode(processed, round);
    }
    overlap = 4 * 1152 / frameSize;
    return dataLength;
}

int SuperEncoder::ProcessPackets(const std::vector<unsigned char> &data, const std::vector<int> &chunkSizes,
                                 bool first, int &processed, bool &complete) {
    if (workers.size() == 1) return driver->WriteData(data.data(), (int) data.size());

    std::vector<unsigned char> packets;
    std::vector<int>           packetSizes;
    repacker->UnpackFrames(data, packets, packetSizes);

    int offset = 0, dataLength = 0;
    if (!first) for (int i = 0; i < overlap; i++) offset += packetSizes[i];

    processed = 0;
    complete  = false;

    for (int i = 0; i < (int) packetSizes.size(); i++) {
        if (i < overlap && !first) continue;
        if (packetSizes[i] == 0)   continue;

        if (!repacker->WriteFrame(packets.data() + offset, packetSizes[i])) return dataLength;

        processed++;
        offset     += packetSizes[i];
        dataLength += packetSizes[i];
    }

    complete = true;
    return dataLength;
}

void SuperEncoder::Finish() {
    EncodeFrames(true);

    /* Write Xing/Info header. */
    std::vector<unsigned char> tag;
    workers.front()->GetInfoTag(tag);
    if (workers.size() > 1) repacker->UpdateInfoTag(tag, totalSamples);

    driver->Seek(0);
    driver->WriteData(tag.data(), (int) tag.size());

    for (auto *w : workers) w->Quit();
    for (auto *w : workers) w->Wait();
}

/* Ask LAME which output sample rate it would choose for this exact encoder
 * configuration and input rate. LAME auto-downsamples in several cases -- most
 * importantly the high VBR levels (-V7..-V9 drop to 22050/11025 Hz) and low
 * CBR/ABR bitrates -- via its internal per-frame resampler. In SuperFast MT that
 * internal resampler is fatal: the overlap accounting assumes 1 input frame == 1
 * output frame, so any in-worker rate change silently drops ~overlap frames per
 * chunk. We probe LAME here so we can pre-resample with r8brain to the SAME rate
 * LAME would have picked, keeping the 1:1 invariant intact (and getting HQ
 * resampling for free). Returns the chosen out rate, or inRate on any failure. */
static int ProbeLameOutRate(const EncoderConfig &cfg, int inRate) {
    const LameEngine &e = SelectLameEngine();
    lame_t p = e.init();
    if (!p) return inRate;
    e.set_in_samplerate(p, inRate);
    e.set_num_channels(p, cfg.channels > 0 ? cfg.channels : 2);
    if (cfg.vbrMode == vbr_off) {
        e.set_brate(p, cfg.bitrate);
    } else if (cfg.vbrMode == vbr_abr) {
        e.set_VBR(p, vbr_abr);
        e.set_VBR_mean_bitrate_kbps(p, cfg.abrBitrate);
    } else { /* vbr_mtrh */
        e.set_VBR(p, vbr_mtrh);
        e.set_VBR_quality(p, cfg.vbrQuality / 10.0f);
    }
    if (cfg.quality >= 0) e.set_quality(p, cfg.quality);
    int out = inRate;
    if (e.init_params(p) >= 0) {
        int r = e.get_out_samplerate(p);
        if (r > 0) out = r;
    }
    e.close(p);
    return out;
}

/* --------------------------------- main -------------------------------- */
#define SUPERLAME_VERSION   "1.0.1"
#define SUPERLAME_LAME_VER  "3.101 beta 3 (SVN r6531)"
#define SUPERLAME_MPG123    "1.33.6"

/* One-line banner printed at the top of every run. Shows SuperLAME's own
 * version prominently, with the underlying LAME lineage in parentheses. (LAME's
 * own version string is kept intact everywhere it matters -- the MP3 Xing/Info
 * tag still reports LAME %s so players and analysers see the true encoder.) */
static void banner() {
    printf("SuperLAME %s 64bits (SuperFast LAME %s + libmpg123 %s)\n",
           SUPERLAME_VERSION, SUPERLAME_LAME_VER, SUPERLAME_MPG123);
}

static void usage(const char *prog) {
    banner();
    printf("\n");
    printf("usage: %s [options] <infile> [outfile.mp3]\n", prog);
    printf("\n");
    printf("    <infile> is WAV (PCM 8/16/24/32-bit or 32/64-bit float), AIFF\n");
    printf("    (16/24-bit) or FLAC (any bit depth, multithreaded decode), mono or\n");
    printf("    stereo, any sample rate. Use \"-\" for stdin/stdout.\n");
    printf("\n");
    printf("RECOMMENDED:\n");
    printf("    superlame-mt -V2 -t0 input.wav output.mp3      (VBR, all cores)\n");
    printf("\n");
    printf("OPTIONS:\n");
    printf("    -b n            CBR bitrate in kbps (default 192)\n");
    printf("    --abr n         ABR (average) bitrate in kbps\n");
    printf("    -V n            VBR quality 0..9  (0=best/biggest, 9=smallest)\n");
    printf("    --preset p      medium / standard / extreme / insane, or 8..320 (ABR)\n");
    printf("    --resample f    output sample rate in kHz or Hz (default: automatic)\n");
    printf("    -q n            encoder quality 0..9 (slow..fast)\n");
    printf("                    NOTE: clamped to >=4 for CBR/ABR (bug #516 q4 fix)\n");
    printf("    -m mode         stereo mode: a=auto  s=stereo  m=mono  j=joint\n");
    printf("    -t n            worker threads (0 or omitted = all CPU threads)\n");
    printf("    -v, --verbose   print encoding configuration and a timing summary\n");
    printf("    --quiet         suppress the banner and progress output\n");
    printf("\n");
    printf("    --decode        decode an MP3 to WAV (input=mp3, output=wav)\n");
    printf("\n");
    printf("  ID3 tags:\n");
    printf("    --tt title      --ta artist     --tl album\n");
    printf("    --ty year       --tc comment    --tn track[/total]\n");
    printf("    --tg genre      --ti file.jpg   (album art: JPEG/PNG)\n");
    printf("    --id3v1-only    --id3v2-only\n");
    printf("\n");
    printf("    --about         what this build is and what's in it\n");
    printf("    --features      feature/capability list\n");
    printf("    --longhelp      full option and notes listing\n");
    printf("    --license       licensing information\n");
    printf("    --version       version and component lineage\n");
    printf("    -h, --help      this help\n");
}

static void longhelp(const char *prog) {
    usage(prog);
    printf("\n");
    printf("RATE CONTROL (pick one; VBR recommended):\n");
    printf("    VBR  -V0..-V9    variable bitrate, quality-targeted (modern psymodel)\n");
    printf("    CBR  -b n        constant bitrate (8..320 kbps for MPEG-1)\n");
    printf("    ABR  --abr n     average bitrate, VBR-like quality at a size target\n");
    printf("\n");
    printf("SAMPLE RATE:\n");
    printf("    MP3 stores a fixed set of rates. Inputs at other rates (e.g. 96/88.2\n");
    printf("    kHz hi-res) are resampled to the nearest legal rate with r8brain\n");
    printf("    (high quality, linear phase), parallelized across cores, BEFORE the\n");
    printf("    encode split. 88.2/176.4 kHz map to 44.1; 96/192 kHz map to 48.\n");
    printf("    --resample forces a specific output rate. At low bitrates the rate is\n");
    printf("    auto-lowered (like LAME) so bits aren't wasted on unusable bandwidth.\n");
    printf("\n");
    printf("MULTITHREADING (SuperFast):\n");
    printf("    Audio is split into overlapping chunks encoded by N parallel LAME\n");
    printf("    instances, then reassembled into one bit-reservoir-correct MP3.\n");
    printf("    -t0 uses every hardware thread. Single-thread (-t1) output is\n");
    printf("    bit-identical to stock LAME; multi-thread differs only by inaudible\n");
    printf("    (<-45 dB) reservoir reconciliation at chunk seams.\n");
    printf("    The repacker carries the full set of fre:ac/BoCA reservoir fixes.\n");
    printf("    As a final safety net the output frame chain is verified, and if it\n");
    printf("    were ever broken the file is transparently re-encoded single-threaded\n");
    printf("    so output is always valid.\n");
    printf("\n");
    printf("CPU ENGINE:\n");
    printf("    One binary contains two libmp3lame builds, chosen by CPUID:\n");
    printf("      znver3 (AVX2/FMA)  -- AMD Zen 3 and any AVX2+FMA+BMI2 CPU\n");
    printf("      x86-64 (SSE2)      -- generic fallback for older CPUs\n");
    printf("    Override for testing: set env  SUPERLAME_ENGINE=sse2|znver3\n");
    printf("\n");
    printf("THE q4 QUALITY FIX (LAME bug #516):\n");
    printf("    Under LAME's current psymodel, CBR/ABR at -q 0..3 degrades quality.\n");
    printf("    This build clamps -q to >=4 for CBR/ABR (maikmerten's patch). VBR is\n");
    printf("    unaffected. Run with -v to see a notice when the clamp engages.\n");
}

static void about() {
    banner();
    printf("\n");
    printf("SuperLAME is a custom MP3 encoder that combines:\n");
    printf("\n");
    printf("  * LAME %s -- the encoder core, the de-facto best MP3 encoder.\n", SUPERLAME_LAME_VER);
    printf("  * libmpg123 %s -- built in as the MPEG decoder (the --decode path).\n", SUPERLAME_MPG123);
    printf("  * maikmerten's \"q4\" patch -- fixes the CBR/ABR quality regression\n");
    printf("    reported as LAME bug #516.\n");
    printf("  * SuperFast multithreading -- Robert Kausch's (fre:ac) technique of\n");
    printf("    running many encoder instances in parallel and repacking the MP3\n");
    printf("    bit reservoir, ported here as a standalone tool.\n");
    printf("  * r8brain-free-src -- Aleksey Vaneev's high-quality sample-rate\n");
    printf("    converter. Hi-res input (e.g. 96/88.2 kHz) is resampled to a legal\n");
    printf("    MP3 rate with a linear-phase, ~200 dB-stopband filter -- cleaner\n");
    printf("    than LAME's internal resampler -- and the conversion is split\n");
    printf("    across CPU cores (per-channel + chunked), so it adds little to the\n");
    printf("    total time even on long hi-res files.\n");
    printf("\n");
    printf("  Built with clang/LLVM, with a runtime CPU-dispatch fat binary tuned\n");
    printf("  for AMD Zen 5/4/3 (znver5/4/3) and a generic SSE2 fallback.\n");
    printf("\n");
    printf("  Active CPU engine right now: %s\n", SelectLameEngine().name);
}

static void features() {
    banner();
    printf("\n");
    printf("FEATURES:\n");
    printf("  [x] CBR, ABR and VBR encoding (MPEG-1 Layer III)\n");
    printf("  [x] Multithreaded SuperFast encoding (near-linear speedup)\n");
    printf("  [x] Bit-reservoir-correct output (decodes cleanly everywhere)\n");
    printf("  [x] LAME/Xing info tag (frame count, TOC, length, CRC)\n");
    printf("  [x] q4 quality fix for CBR/ABR (LAME bug #516)\n");
    printf("  [x] Runtime CPU dispatch: znver5 / znver4 / znver3 / x86-64 (CPUID)\n");
    printf("  [x] MP3 -> WAV decoding (--decode, via libmpg123)\n");
    printf("  [x] WAV + AIFF + FLAC input (FLAC = multithreaded decode)\n");
    printf("  [x] WAV: PCM 8/16/24/32-bit + 32/64-bit float; any rate; stdin/stdout\n");
    printf("  [x] full-precision float pipeline for >16-bit sources (no early truncation)\n");
    printf("  [x] high-quality parallel resampling (r8brain); --resample + low-bitrate auto\n");
    printf("  [x] mono / stereo / joint-stereo\n");
    printf("  [x] ID3v1 + ID3v2 tagging (--tt/--ta/--tl/--ty/--tc/--tn/--tg), UTF-8\n");
    printf("  [x] album art (--ti, JPEG/PNG APIC); FLAC tags + cover imported auto\n");
    printf("  [x] LAME --preset aliases; UTF-8 console output\n");
    printf("  [ ] raw headerless PCM input (-r) not yet supported\n");
}

static void license() {
    banner();
    printf("\n");
    printf("LICENSE:\n");
    printf("  This build combines LAME (LGPL), mpg123 (LGPL) and code derived from\n");
    printf("  fre:ac/BoCA SuperFast (GPL v2+). The combined work is therefore\n");
    printf("  distributed under the GNU General Public License, version 2 or later.\n");
    printf("\n");
    printf("  There is NO WARRANTY, to the extent permitted by law.\n");
    printf("\n");
    printf("  Components:\n");
    printf("    LAME      https://lame.sourceforge.io/\n");
    printf("    mpg123    https://www.mpg123.de/\n");
    printf("    SuperFast https://www.freac.org/  (github.com/enzo1982/superfast)\n");
    printf("    bug #516  https://sourceforge.net/p/lame/bugs/516/\n");
}

static void version() {
    banner();
    printf("\n");
    printf("  SuperLAME  : %s\n", SUPERLAME_VERSION);
    printf("  encoder    : LAME %s\n", SUPERLAME_LAME_VER);
    printf("               (the MP3 Xing/Info tag reports this LAME version, so\n");
    printf("                players and analysers see the true encoder lineage.)\n");
    printf("  decoder    : libmpg123 %s (built-in)\n", SUPERLAME_MPG123);
    printf("  resampler  : r8brain-free-src (linear-phase, parallelized)\n");
    printf("  quality    : maikmerten q4 patch (LAME bug #516, CBR/ABR)\n");
    printf("  parallel   : SuperFast chunk-split + bit-reservoir repack (R. Kausch)\n");
    printf("  engines    : znver5 / znver4 / znver3 / x86-64 (CPUID-selected)\n");
    printf("               (znver4/znver5 are built but UNVERIFIED -- no Zen 4/5\n");
    printf("                host was available to test them; znver3 + SSE2 are.)\n");
    printf("  active     : %s\n", SelectLameEngine().name);
}

static const char *modeName(int vbrMode) {
    return vbrMode == vbr_off ? "CBR" : vbrMode == vbr_abr ? "ABR" : "VBR";
}
static const char *stereoName(int m) {
    return m == 1 ? "mono" : m == 2 ? "stereo" : m == 3 ? "joint stereo" : "auto (joint stereo)";
}

int main(int argc, char **argv) {
    EncoderConfig cfg;
    int numThreads = 0;
    bool verbose = false, quiet = false, decode = false;
    int  resampleTo = 0;      /* --resample: explicit output rate (Hz), 0 = auto */
    TagConfig tag;
    const char *coverPath = nullptr;   /* --ti: album-art image file (0 = none) */
    const char *inPath = nullptr, *outPath = nullptr;
    const char *prog = "SuperLAME";

#ifdef _WIN32
    /* Render our UTF-8 output correctly in the console. Without this the UTF-8
     * bytes we print (filenames with umlauts / accents / U+221E etc.) show as
     * mojibake -- the file was always written correctly, but watching the CLI
     * clobber the text was confusing. Setting the console output code page to
     * UTF-8 makes printf/fprintf bytes display as the intended glyphs (on a
     * modern console with a Unicode-capable font). Saved+restored is overkill
     * for a one-shot CLI, so we just set it for the process's lifetime. */
    UINT prevCP = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    (void) prevCP;

    /* main()'s ANSI argv loses characters outside the code page (e.g. the
     * infinity in a filename). Re-acquire the real UTF-16 command line and
     * re-express every argument as UTF-8 so paths survive intact. */
    std::vector<std::string> wargs;
    std::vector<char *>      wargv;
    {
        int wc = 0;
        LPWSTR *wv = CommandLineToArgvW(GetCommandLineW(), &wc);
        if (wv) {
            wargs.reserve(wc);
            for (int i = 0; i < wc; i++) wargs.push_back(w_to_utf8(wv[i]));
            LocalFree(wv);
            for (auto &s : wargs) wargv.push_back(&s[0]);
            argc = (int) wargv.size();
            argv = wargv.data();
        }
    }
#endif

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];

        /* For a short flag `-X`, accept both the attached form (`-X2`) and the
         * spaced form (`-X 2`), like stock LAME. Returns the value string or
         * nullptr if none is available. `flag` is e.g. "-V". */
        auto optval = [&](const char *flag) -> const char * {
            size_t fl = strlen(flag);
            if (a.size() > fl) return argv[i] + fl;          // attached: -V2
            if (i + 1 < argc)  return argv[++i];             // spaced:   -V 2
            return nullptr;
        };
        auto is = [&](const char *flag) {
            size_t fl = strlen(flag);
            return a.compare(0, fl, flag) == 0 && (a.size() == fl || a.size() > fl);
        };

        const char *val;
        if      (a == "-v" || a == "--verbose"){ verbose = true; }
        else if (a == "--decode")              { decode = true; }
        else if (a == "--quiet")               { quiet = true; }
        else if (a == "--resample" && i + 1 < argc) {
            /* Accept kHz (e.g. 44.1) or Hz (e.g. 44100), like LAME. */
            double v = atof(argv[++i]);
            resampleTo = (int) (v < 1000 ? v * 1000 + 0.5 : v + 0.5);
        }
        /* ID3 tag fields (LAME-compatible flags). */
        else if (a == "--tt" && i + 1 < argc)  { tag.title   = argv[++i]; tag.any = true; }
        else if (a == "--ta" && i + 1 < argc)  { tag.artist  = argv[++i]; tag.any = true; }
        else if (a == "--tl" && i + 1 < argc)  { tag.album   = argv[++i]; tag.any = true; }
        else if (a == "--ty" && i + 1 < argc)  { tag.year    = argv[++i]; tag.any = true; }
        else if (a == "--tc" && i + 1 < argc)  { tag.comment = argv[++i]; tag.any = true; }
        else if (a == "--tn" && i + 1 < argc)  { tag.track   = argv[++i]; tag.any = true; }
        else if (a == "--tg" && i + 1 < argc)  { tag.genre   = argv[++i]; tag.any = true; }
        else if (a == "--ti" && i + 1 < argc)  { coverPath = argv[++i]; }   /* album art */
        else if (a == "--id3v1-only")          { tag.v2 = false; }
        else if (a == "--id3v2-only")          { tag.v1 = false; }
        else if (a == "--add-id3v2")           { tag.v2 = true;  }
        /* LAME-compatible presets (map to our VBR/CBR/ABR config). */
        else if (a == "--preset" && i + 1 < argc) {
            std::string ps = argv[++i];
            if      (ps == "medium")   { cfg.vbrMode = vbr_mtrh; cfg.vbrQuality = 40; }
            else if (ps == "standard") { cfg.vbrMode = vbr_mtrh; cfg.vbrQuality = 20; }
            else if (ps == "extreme")  { cfg.vbrMode = vbr_mtrh; cfg.vbrQuality = 0;  }
            else if (ps == "insane")   { cfg.vbrMode = vbr_off;  cfg.bitrate    = 320; }
            else {
                int br = atoi(ps.c_str());
                if (br >= 8 && br <= 320) { cfg.vbrMode = vbr_abr; cfg.abrBitrate = br; }
                else { fprintf(stderr, "unknown --preset '%s' (medium/standard/extreme/insane or 8..320)\n", ps.c_str()); return 1; }
            }
        }
        else if (a == "-h" || a == "--help")   { usage(prog); return 0; }
        else if (a == "--longhelp")            { longhelp(prog); return 0; }
        else if (a == "--about")               { about(); return 0; }
        else if (a == "--features")            { features(); return 0; }
        else if (a == "--license")             { license(); return 0; }
        else if (a == "--version")             { version(); return 0; }
        else if (a == "--abr" && i + 1 < argc) { cfg.vbrMode = vbr_abr; cfg.abrBitrate = atoi(argv[++i]); }
        else if (is("-b") && (val = optval("-b"))) { cfg.vbrMode = vbr_off;  cfg.bitrate    = atoi(val); }
        else if (is("-V") && (val = optval("-V"))) { cfg.vbrMode = vbr_mtrh; cfg.vbrQuality = atoi(val) * 10; }
        else if (is("-q") && (val = optval("-q"))) { cfg.quality    = atoi(val); }
        else if (is("-t") && (val = optval("-t"))) { numThreads     = atoi(val); }
        else if (is("-m") && (val = optval("-m"))) { char c = val[0]; cfg.stereoMode = c=='m'?1:c=='s'?2:c=='j'?3:0; }
        else if (a.size() && a[0] == '-' && a != "-") {
            fprintf(stderr, "unknown option: %s  (try --help)\n", argv[i]);
            return 1;
        }
        else if (!inPath)  inPath  = argv[i];
        else if (!outPath) outPath = argv[i];
    }

    if (!inPath || !outPath) { usage(prog); return 1; }
    if (numThreads <= 0) numThreads = (int) std::max(1u, std::thread::hardware_concurrency());

    /* Writing the MP3 to stdout: suppress stdout chatter so it can't corrupt the
     * piped stream (real errors still go to stderr). */
    if (outPath[0] == '-' && outPath[1] == '\0') quiet = true;

    if (!quiet) banner();

    /* --decode: MP3 -> WAV via the built-in libmpg123. */
    if (decode) {
#ifdef _WIN32
        FILE *mf = open_utf8(inPath, L"rb");
#else
        FILE *mf = open_utf8(inPath, "rb");
#endif
        if (!mf) { fprintf(stderr, "cannot open input: %s\n", inPath); return 1; }
        int64_t msz = FileSize64(mf);
        std::vector<unsigned char> mp3(msz > 0 ? (size_t) msz : 0);
        if (msz > 0 && fread(mp3.data(), 1, (size_t) msz, mf) != (size_t) msz) { fclose(mf); fprintf(stderr, "read failed\n"); return 1; }
        fclose(mf);

        /* Reject obvious non-MP3 input (e.g. a WAV), so libmpg123 doesn't
         * "decode" false syncs in raw PCM into garbage. Accept an ID3v2 tag
         * ("ID3") or an MPEG audio sync (0xFFEx). RIFF/WAVE is rejected. */
        if (mp3.size() >= 12 && memcmp(mp3.data(), "RIFF", 4) == 0 && memcmp(mp3.data() + 8, "WAVE", 4) == 0) {
            fprintf(stderr, "input is a WAV, not an MP3 (did you mean to encode?)\n"); return 1;
        }
        {
            bool id3 = mp3.size() >= 3 && memcmp(mp3.data(), "ID3", 3) == 0;
            bool sync = mp3.size() >= 2 && mp3[0] == 0xFF && (mp3[1] & 0xE0) == 0xE0;
            if (!id3 && !sync) { fprintf(stderr, "input does not look like an MP3\n"); return 1; }
        }

        std::vector<short> pcm; long rate = 0; int channels = 0;
        auto td0 = std::chrono::steady_clock::now();
        if (!DecodeMp3(mp3, pcm, rate, channels)) { fprintf(stderr, "decode failed (not an MP3?)\n"); return 1; }
        auto td1 = std::chrono::steady_clock::now();

#ifdef _WIN32
        FILE *wf = open_utf8(outPath, L"wb");
#else
        FILE *wf = open_utf8(outPath, "wb");
#endif
        if (!wf || !WriteWav(wf, pcm, rate, channels)) { if (wf) fclose(wf); fprintf(stderr, "write failed: %s\n", outPath); return 1; }
        fclose(wf);

        if (!quiet) {
            double secs  = std::chrono::duration<double>(td1 - td0).count();
            double audio = channels ? (double) (pcm.size() / channels) / rate : 0.0;
            printf("Decoded %s -> %s : %ld Hz, %d ch, %.2f s%s\n",
                   inPath, outPath, rate, channels, audio,
                   secs > 0 ? "" : "");
            if (verbose)
                printf("  %zu samples, decode time %.2f s (%.0fx realtime)\n",
                       pcm.size(), secs, secs > 0 ? audio / secs : 0.0);
        }
        return 0;
    }

    /* RAM preflight: estimate peak usage from the input file size and the
     * available physical RAM, and refuse (or warn) before allocating. Skipped
     * for stdin (unknown size up front). */
    bool inIsStdin = (inPath[0] == '-' && inPath[1] == '\0');
    if (!inIsStdin) {
#ifdef _WIN32
        FILE *sf = open_utf8(inPath, L"rb");
#else
        FILE *sf = open_utf8(inPath, "rb");
#endif
        if (sf) {
            int64_t fsz = FileSize64(sf); fclose(sf);
            if (fsz > 0) {
                /* Peek the bit depth cheaply from a re-open (ReadWav parses it
                 * fully later; here we just guess >16 from file size heuristics
                 * -- safe to assume the heaviest path if unsure). Assume resample
                 * possible unless the target is obviously same-rate; we don't
                 * know the rate yet, so estimate the heavier "may resample" case. */
                uint64_t avail = AvailablePhysicalRAM();
                uint64_t est   = EstimatePeakRAM((uint64_t) fsz, 24, true);
                if (avail > 0 && est > avail) {
                    fprintf(stderr,
                        "error: input is %.1f GB; estimated peak memory ~%.1f GB exceeds "
                        "available RAM ~%.1f GB.\n"
                        "This encoder loads the whole file into memory. Use a smaller file, "
                        "free up RAM, or split the input.\n",
                        fsz / 1e9, est / 1e9, avail / 1e9);
                    return 1;
                }
                if (avail > 0 && est > avail * 3 / 4 && !quiet) {
                    fprintf(stderr,
                        "warning: estimated peak memory ~%.1f GB is a large fraction of "
                        "available RAM ~%.1f GB; encoding may be slow or fail.\n",
                        est / 1e9, avail / 1e9);
                }
            }
        }
    }

    WavData wav;
    g_flacThreads = numThreads;      /* MT width for FLAC decode (if input is FLAC) */
    g_flacQuiet   = quiet;
    g_inputWasFlac = false;
    try {
        if (!ReadWav(inPath, wav)) return 1;
    } catch (const std::bad_alloc &) {
        fprintf(stderr, "error: out of memory reading input (file too large for available RAM).\n");
        return 1;
    }

    /* FLAC carries its own metadata: fill any tag field the user did NOT set on
     * the CLI (explicit --tt/--ta/... always win). Embedded front-cover art is
     * used only if no --ti was given (handled in Phase B where art is wired). */
    if (g_inputWasFlac && g_flacMeta.tags.any) {
        const TagConfig &ft = g_flacMeta.tags;
        if (tag.title.empty()   && !ft.title.empty())   tag.title   = ft.title;
        if (tag.artist.empty()  && !ft.artist.empty())  tag.artist  = ft.artist;
        if (tag.album.empty()   && !ft.album.empty())   tag.album   = ft.album;
        if (tag.year.empty()    && !ft.year.empty())    tag.year    = ft.year;
        if (tag.comment.empty() && !ft.comment.empty()) tag.comment = ft.comment;
        if (tag.track.empty()   && !ft.track.empty())   tag.track   = ft.track;
        if (tag.genre.empty()   && !ft.genre.empty())   tag.genre   = ft.genre;
        if (!tag.title.empty() || !tag.artist.empty() || !tag.album.empty() ||
            !tag.year.empty()  || !tag.comment.empty() ||
            !tag.track.empty() || !tag.genre.empty())
            tag.any = true;
    }

    /* Album art (APIC). Priority: --ti <file> (explicit) beats FLAC-embedded art.
     * We accept only JPEG/PNG (what players render); anything else is refused
     * with a clear message. Large art forces ID3v2.4 (v2.3 caps APIC at 128K). */
    {
        std::vector<unsigned char> art;
        const char *artSource = nullptr;
        if (coverPath) {
            FILE *cf = open_utf8(coverPath, L"rb");
            if (!cf) { fprintf(stderr, "cannot open album-art file: %s\n", coverPath); return 1; }
            int64_t asz = FileSize64(cf);
            if (asz > 0) { art.resize((size_t) asz);
                if (fread(art.data(), 1, (size_t) asz, cf) != (size_t) asz) art.clear(); }
            fclose(cf);
            if (art.empty()) { fprintf(stderr, "album-art file is empty or unreadable: %s\n", coverPath); return 1; }
            artSource = coverPath;
        } else if (g_inputWasFlac && !g_flacMeta.art.data.empty()) {
            art = g_flacMeta.art.data;             /* embedded front cover */
            artSource = "embedded FLAC cover";
        }
        if (!art.empty()) {
            const char *mime = SniffCoverMime(art);
            if (!mime) {
                fprintf(stderr, "album art: %s is not JPEG or PNG; skipping "
                        "(MP3 players only display JPEG/PNG cover art).\n", artSource);
            } else {
                if (art.size() > 128 * 1024) tag.v2 = true;   /* ensure v2 on */
                /* Large images require ID3v2.4 (LAME caps APIC at 128K under
                 * v2.3). We already emit v2.4+UTF8 in RenderTags, so this is
                 * covered; just note it for the user on very large art. */
                if (art.size() > 512 * 1024 && !quiet)
                    fprintf(stderr, "note: embedding a %.0f KB %s cover (ID3v2.4).\n",
                            art.size() / 1024.0, mime + 6);
                tag.art = std::move(art);
            }
        }
    }

    /* Decide the output sample rate, then (if it differs from the input)
     * resample HERE -- before SuperFast chunking -- so every worker sees one
     * clean rate and LAME's own per-frame resampler never runs. r8brain, HQ.
     *
     * Rate is chosen by (in priority):
     *   1. --resample <freq>            explicit user request
     *   2. low-bitrate auto-downsample  matches LAME: at low CBR/ABR rates a
     *      lower sample rate sounds better than a bandwidth-starved high rate
     *   3. illegal input rate           nearest legal MP3 rate
     * Otherwise the input rate is kept as-is. */
    int dstRate = wav.rate;
    if (resampleTo > 0) {
        dstRate = NearestMp3Rate(resampleTo);           /* snap to a legal rate */
    } else {
        /* Ask LAME what output rate it would use for this config. This inherits
         * LAME's exact auto-downsample decision -- for CBR/ABR *and* VBR (the
         * high -V levels drop the rate) -- instead of approximating it. Doing the
         * resample up-front with r8brain guarantees the SuperFast MT overlap
         * invariant (1 input frame == 1 output frame in every worker). */
        int lameRate = ProbeLameOutRate(cfg, wav.rate);
        if (lameRate > 0 && lameRate != wav.rate) dstRate = NearestMp3Rate(lameRate);
        else if (!IsLegalMp3Rate(wav.rate))       dstRate = NearestMp3Rate(wav.rate);
    }

    if (dstRate != wav.rate) {
      try {
        /* Resampler works in float; promote a 16/8-bit short buffer to float. */
        if (!wav.useFloat) {
            wav.samplesF.resize(wav.samples.size());
            for (size_t i = 0; i < wav.samples.size(); i++)
                wav.samplesF[i] = wav.samples[i] / 32768.0f;
            wav.samples.clear();
            wav.useFloat = true;
        }
        if (!quiet)
            fprintf(stderr, "resampling %d Hz -> %d Hz (r8brain)...\n", wav.rate, dstRate);
        wav.samplesF = ResampleFloat(wav.samplesF, wav.channels, wav.rate, dstRate);
        wav.rate = dstRate;
      } catch (const std::bad_alloc &) {
        fprintf(stderr, "error: out of memory during resampling (file too large for available RAM).\n");
        return 1;
      }
    }

    cfg.rate     = wav.rate;
    cfg.channels = wav.channels;
    cfg.useFloat = wav.useFloat;

    /* Effective -q after the q4 clamp, for display. */
    int effQ = cfg.quality;
    bool clamped = false;
    if (cfg.vbrMode != vbr_mtrh && effQ >= 0 && effQ < 4) { effQ = 4; clamped = true; }

    if (verbose && !quiet) {
        const int spf = 1152;
        double ratio = (double) cfg.rate * cfg.channels * 16
                     / ((cfg.vbrMode == vbr_off ? cfg.bitrate : cfg.abrBitrate) * 1000.0);
        printf("\n");
        printf("Encoding %s\n", inPath);
        printf("      to %s\n", outPath);
        printf("Engine: %s   Threads: %d\n", SelectLameEngine().name, numThreads);
        if (cfg.vbrMode == vbr_mtrh)
            printf("Encoding as %.3g kHz %s MPEG-1 Layer III  VBR -V%d\n",
                   cfg.rate / 1000.0, stereoName(cfg.stereoMode), cfg.vbrQuality / 10);
        else
            printf("Encoding as %.3g kHz %s MPEG-1 Layer III (%.1fx) %s %d kbps qval=%d\n",
                   cfg.rate / 1000.0, stereoName(cfg.stereoMode), ratio, modeName(cfg.vbrMode),
                   cfg.vbrMode == vbr_off ? cfg.bitrate : cfg.abrBitrate, effQ < 0 ? 3 : effQ);
        if (clamped)
            printf("CBR/ABR: -q %d clamped to 4 (bug #516 quality fix)\n", cfg.quality);
        printf("Frame size: %d samples   Input: %d samples (%.2f s)%s\n",
               spf, (int) wav.count() / cfg.channels,
               (double) (wav.count() / cfg.channels) / cfg.rate,
               wav.useFloat ? "  [full-precision float input]" : "");
        printf("\n");
    } else if (!quiet) {
        char rate[32];
        if (cfg.vbrMode == vbr_mtrh)      snprintf(rate, sizeof rate, "VBR -V%d", cfg.vbrQuality / 10);
        else if (cfg.vbrMode == vbr_abr)  snprintf(rate, sizeof rate, "ABR %d kbps", cfg.abrBitrate);
        else                              snprintf(rate, sizeof rate, "CBR %d kbps", cfg.bitrate);
        printf("Encoding %s -> %s : %.3g kHz %s, %s, %d thread(s), %s\n",
               inPath, outPath, cfg.rate / 1000.0, stereoName(cfg.stereoMode),
               rate, numThreads, SelectLameEngine().name);
    }

    auto t0 = std::chrono::steady_clock::now();
    MemDriver driver;
    try {
        SuperEncoder enc(cfg, &driver, numThreads);
        /* Feed all samples (frame-aligned chunks happen inside). */
        enc.WriteData(wav.data(), (int) wav.count());
        enc.Finish();
    } catch (const std::bad_alloc &) {
        fprintf(stderr, "error: out of memory during encoding (file too large for available RAM).\n");
        return 1;
    }

    /* Self-heal: the parallel bit-reservoir repacker has a rare corner case on
     * pathological dynamics where one frame's chaining is left inconsistent. If
     * the assembled stream's frame chain is broken anywhere, transparently
     * re-encode single-threaded (always correct) and use that instead. */
    if (numThreads > 1 && !FrameChainValid(driver.Bytes())) {
        /* Always report on stderr (even with --quiet): a desync is a real event
         * worth surfacing, and lets test harnesses detect it without decoding. */
        fprintf(stderr, "note: repacker produced an invalid frame; re-encoding single-threaded for a clean stream...\n");
        MemDriver clean;
        SuperEncoder st(cfg, &clean, 1);
        st.WriteData(wav.data(), (int) wav.count());
        st.Finish();
        driver = std::move(clean);
    }
    auto t1 = std::chrono::steady_clock::now();

    /* Render ID3 tags (frontend writes v2 before / v1 after the MP3 stream). */
    std::vector<unsigned char> id3v2, id3v1;
    if (tag.hasContent()) RenderTags(SelectLameEngine(), tag, id3v2, id3v1);

#ifdef _WIN32
    FILE *of = open_utf8(outPath, L"wb");
#else
    FILE *of = open_utf8(outPath, "wb");
#endif
    bool toStdout = (outPath[0] == '-' && outPath[1] == '\0');
    bool wrote = false;
    if (of) {
        wrote = true;
        if (!id3v2.empty()) wrote = wrote && fwrite(id3v2.data(), 1, id3v2.size(), of) == id3v2.size();
        wrote = wrote && driver.WriteToFILE(of);
        if (!id3v1.empty()) wrote = wrote && fwrite(id3v1.data(), 1, id3v1.size(), of) == id3v1.size();
        if (toStdout) fflush(of); else fclose(of);
    }
    if (!wrote) { fprintf(stderr, "write failed: %s\n", outPath); return 1; }

    if (!quiet) {
        double secs  = std::chrono::duration<double>(t1 - t0).count();
        double audio = (double) (wav.count() / cfg.channels) / cfg.rate;
        long long bytes = driver.GetSize();
        if (verbose) {
            printf("Done.\n");
            printf("  output     : %s (%lld bytes, %.1f KiB)\n", outPath, bytes, bytes / 1024.0);
            printf("  encode time: %.2f s   audio: %.2f s   speed: %.1fx realtime\n",
                   secs, audio, secs > 0 ? audio / secs : 0.0);
            printf("  avg bitrate: %.0f kbps\n", bytes * 8.0 / 1000.0 / (audio > 0 ? audio : 1));
        } else {
            printf("wrote %s (%lld bytes, %.1fx realtime)\n",
                   outPath, bytes, secs > 0 ? audio / secs : 0.0);
        }
    }
    return 0;
}
