/* superlame-mt: SuperWorker implementation.
 * Ported from SuperFast worker.cpp, (C) 2007-2018 Robert Kausch, GPL v2+. */
#include "worker.h"
#include <algorithm>
#include <cstring>
#include <chrono>

static inline void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

SuperWorker::SuperWorker(const EncoderConfig &config, int iOverlap) {
    overlap     = iOverlap;
    channels    = config.channels;
    useFloat    = config.useFloat;
    sampleBytes = useFloat ? (int) sizeof(float) : (int) sizeof(short);
    eng         = &SelectLameEngine();

    context = eng->init();

    eng->set_in_samplerate(context, config.rate);
    /* Force output rate == input rate so LAME's internal per-frame resampler can
     * NEVER run inside a worker. Any in-worker rate change would break the
     * SuperFast overlap accounting (which assumes 1 input frame == 1 output
     * frame) and silently drop frames. The frontend has already resampled the
     * whole stream to the desired rate with r8brain before chunking. */
    eng->set_out_samplerate(context, config.rate);
    eng->set_num_channels(context, config.channels);

    /* Bitrate / VBR (SuperFast "Preset 0" custom path). */
    if (config.vbrMode == vbr_off) {
        eng->set_brate(context, config.bitrate);
    }

    if (config.quality >= 0) eng->set_quality(context, config.quality);

    if      (config.stereoMode == 1) eng->set_mode(context, MONO);
    else if (config.stereoMode == 2) eng->set_mode(context, STEREO);
    else if (config.stereoMode == 3) eng->set_mode(context, JOINT_STEREO);
    else                             eng->set_mode(context, NOT_SET);

    switch (config.vbrMode) {
        default:
        case vbr_off:
            break;
        case vbr_abr:
            eng->set_VBR(context, vbr_abr);
            eng->set_VBR_mean_bitrate_kbps(context, config.abrBitrate);
            break;
        case vbr_mtrh:
            eng->set_VBR(context, vbr_mtrh);
            eng->set_VBR_quality(context, config.vbrQuality / 10.0);
            break;
    }

    eng->init_params(context);

    frameSize     = eng->get_framesize(context);
    maxPacketSize = (int) (frameSize * 1.25 + 7200);
}

SuperWorker::~SuperWorker() {
    if (thread) { delete thread; thread = nullptr; }
    eng->close(context);
}

void SuperWorker::Start() {
    thread = new std::thread(&SuperWorker::Run, this);
}

void SuperWorker::Run() {
    while (!quit) {
        while (!quit && !process) SleepMs(1);
        if (quit) break;

        workerMutex.lock();

        packetBuffer.resize(0);
        packetSizes.clear();

        int samplesLeft    = (int) (samplesBuffer.size() / sampleBytes);
        int samplesPerFrame = frameSize * channels;
        int framesProcessed = 0;

        while (flush || samplesLeft >= samplesPerFrame) {
            packetBuffer.resize(packetBuffer.size() + maxPacketSize);

            int dataLength = 0;
            unsigned char *mp3out = packetBuffer.data() + packetBuffer.size() - maxPacketSize;
            unsigned char *chunk  = samplesBuffer.data() + (size_t) framesProcessed * samplesPerFrame * sampleBytes;

            if (samplesLeft > 0) {
                if (useFloat) {
                    const float *fc = (const float *) chunk;
                    if (channels == 2)
                        dataLength = eng->encode_buffer_interleaved_ieee_float(
                            context, fc, std::min(samplesLeft / 2, frameSize), mp3out, maxPacketSize);
                    else
                        dataLength = eng->encode_buffer_ieee_float(
                            context, fc, nullptr, std::min(samplesLeft, frameSize), mp3out, maxPacketSize);
                } else {
                    short *sc = (short *) chunk;
                    if (channels == 2)
                        dataLength = eng->encode_buffer_interleaved(
                            context, sc, std::min(samplesLeft / 2, frameSize), mp3out, maxPacketSize);
                    else
                        dataLength = eng->encode_buffer(
                            context, sc, nullptr, std::min(samplesLeft, frameSize), mp3out, maxPacketSize);
                }
            } else {
                dataLength = eng->encode_flush(context, mp3out, maxPacketSize);
            }

            packetBuffer.resize(packetBuffer.size() - maxPacketSize + dataLength);

            if (samplesLeft < 0 && dataLength == 0) break;
            if (dataLength > 0) packetSizes.push_back(dataLength);

            framesProcessed++;
            samplesLeft -= samplesPerFrame;
        }

        /* Flush at the end of each block in parallel mode. */
        if (overlap > 0) {
            packetBuffer.resize(packetBuffer.size() + maxPacketSize);
            int dataLength = eng->encode_flush_nogap(
                context, packetBuffer.data() + packetBuffer.size() - maxPacketSize, maxPacketSize);
            packetBuffer.resize(packetBuffer.size() - maxPacketSize + dataLength);
            if (dataLength > 0) packetSizes.push_back(dataLength);
        }

        workerMutex.unlock();

        process = false;
    }
}

void SuperWorker::Encode(const unsigned char *buffer, int offset, int size, bool last) {
    workerMutex.lock();
    samplesBuffer.resize((size_t) size * sampleBytes);
    memcpy(samplesBuffer.data(), buffer + (size_t) offset * sampleBytes, (size_t) size * sampleBytes);
    workerMutex.unlock();

    flush   = last;
    process = true;
}

void SuperWorker::ReEncode(int skipFrames, int dummyFrames) {
    int skipSamples  = skipFrames  * frameSize * channels;
    int dummySamples = dummyFrames * frameSize * channels;

    /* Backup remaining samples (raw bytes). */
    size_t skipBytes   = (size_t) skipSamples * sampleBytes;
    std::vector<unsigned char> backupBuffer(samplesBuffer.size() - skipBytes);
    memcpy(backupBuffer.data(), samplesBuffer.data() + skipBytes, backupBuffer.size());

    /* Build dummy data to pressure the reservoir (same pattern as the short
     * path; for float, scale the i*147 pattern into normalized range). */
    std::vector<unsigned char> dummyBuffer((size_t) dummySamples * sampleBytes);
    if (useFloat) {
        float *d = (float *) dummyBuffer.data();
        for (int i = 0; i < dummySamples; i++) d[i] = (float) (short) (i * 147) / 32768.0f;
    } else {
        short *d = (short *) dummyBuffer.data();
        for (int i = 0; i < dummySamples; i++) d[i] = (short) (i * 147);
    }

    /* Encode dummy frames to pressure reservoir. */
    Encode(dummyBuffer.data(), 0, dummySamples, flush);

    workerMutex.unlock();
    while (process) SleepMs(1);
    workerMutex.lock();

    /* Re-encode previous samples. */
    Encode(backupBuffer.data(), 0, (int) (backupBuffer.size() / sampleBytes), flush);

    workerMutex.unlock();
    while (process) SleepMs(1);
    workerMutex.lock();
}

void SuperWorker::GetInfoTag(std::vector<unsigned char> &buffer) const {
    buffer.resize(2880);
    int size = eng->get_lametag_frame(context, buffer.data(), buffer.size());
    buffer.resize(size);
}

void SuperWorker::Quit() {
    quit = true;
}

void SuperWorker::Wait() {
    if (thread && thread->joinable()) thread->join();
}
