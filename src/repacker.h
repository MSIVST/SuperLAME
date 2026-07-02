/* superlame-mt: SuperRepacker -- MP3 bit-reservoir reconciliation.
 * Ported from SuperFast repacker.{h,cpp}, (C) 2007-2018 Robert Kausch, GPL v2+.
 *
 * Reassembles MP3 frames produced independently by parallel encoder instances
 * into a single bit-reservoir-correct stream. Logic preserved exactly. */
#ifndef SUPERLAME_REPACKER_H
#define SUPERLAME_REPACKER_H

#include "iodriver.h"
#include "mp3frame.h"
#include <vector>
#include <cstdint>

class SuperRepacker {
private:
    MemDriver *driver;

    int offset;
    int frameCount;
    int reservoir;
    int cbrIndex;
    int minIndex;
    int maxIndex;

    std::vector<u8>       frameBuffer;
    std::vector<uint32_t> frameOffsets;

    bool FillReservoir(int threshold = 0);
    bool IncreaseReservoir(int bytes, u8 *reference, int depth = 0);
    bool CheckPrecedingFrame(u8 *data, int offset, u8 *reference);

public:
    explicit SuperRepacker(MemDriver *driver);
    ~SuperRepacker();

    bool EnableRateControl(int minRate, int maxRate);

    bool UnpackFrames(const std::vector<u8> &data,
                      std::vector<u8> &packets, std::vector<int> &packetSizes);
    bool WriteFrame(u8 *frame, int size);

    bool Flush();

    bool UpdateInfoTag(std::vector<u8> &frame, int64_t totalSamples) const;
};

#endif
