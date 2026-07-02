/* superlame-mt: SuperWorker -- one LAME encoder instance on its own thread.
 * Ported from SuperFast worker.{h,cpp}, (C) 2007-2018 Robert Kausch, GPL v2+. */
#ifndef SUPERLAME_WORKER_H
#define SUPERLAME_WORKER_H

#include <lame.h>
#include "lame_dispatch.h"
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

/* Minimal encoder configuration for the standalone CLI. Mirrors the subset of
 * SuperFast's "Preset 0" custom settings we expose. */
struct EncoderConfig {
    int  rate     = 44100;
    int  channels = 2;
    int  vbrMode  = vbr_off; // vbr_off / vbr_abr / vbr_mtrh
    int  bitrate  = 192;     // CBR kbps
    int  abrBitrate = 192;   // ABR kbps
    int  vbrQuality = 40;    // VBR quality *10 (so 40 => -V4); per SuperFast /10 convention
    int  quality  = -1;      // -q; -1 = leave at preset default
    int  stereoMode = 0;     // 0 auto, 1 mono, 2 stereo, 3 joint
    bool useFloat = false;   // true: samples are normalized +/-1.0 float (>16-bit sources)
};

class SuperWorker {
private:
    /* Recursive: matches smooth's Threads::Mutex (Win32 CRITICAL_SECTION).
     * The orchestrator calls Lock() then Encode() which locks again on the
     * same thread -- a std::mutex would deadlock/fault on the re-entry. */
    std::recursive_mutex    workerMutex;
    std::thread            *thread = nullptr;

    const LameEngine       *eng = nullptr;
    lame_t                  context = nullptr;
    int                     channels;
    bool                    useFloat;        // float (>16-bit) vs short (16-bit) path
    int                     sampleBytes;     // 4 if float, else 2

    int                     frameSize;
    int                     maxPacketSize;

    /* Interleaved samples as raw bytes (short or float per sampleBytes). Raw
     * storage keeps the chunking math identical for both element types. */
    std::vector<unsigned char> samplesBuffer;
    std::vector<unsigned char> packetBuffer;
    std::vector<int>        packetSizes;

    int                     overlap;

    std::atomic<bool>       process{false};
    std::atomic<bool>       flush{false};
    std::atomic<bool>       quit{false};

    void Run();

public:
    SuperWorker(const EncoderConfig &config, int overlap);
    ~SuperWorker();

    void Start();

    /* offset/size are in SAMPLES (not bytes); buffer is raw interleaved samples
     * of the worker's element type (short or float). */
    void Encode(const unsigned char *buffer, int offset, int size, bool last);
    void ReEncode(int skipFrames, int dummyFrames);

    void GetInfoTag(std::vector<unsigned char> &buffer) const;

    void Quit();
    void Wait();

    bool Lock()    { workerMutex.lock(); return true; }
    bool Release() { workerMutex.unlock(); return true; }

    bool IsReady() const { return !process.load(); }

    const std::vector<unsigned char> &GetPackets() const    { return packetBuffer; }
    const std::vector<int>           &GetPacketSizes() const { return packetSizes; }
};

#endif
