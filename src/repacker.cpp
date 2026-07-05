/* superlame-mt: SuperRepacker implementation.
 * Ported from SuperFast repacker.cpp, (C) 2007-2018 Robert Kausch, GPL v2+.
 * Logic preserved exactly; only the container/IO types are translated to std. */
#include <cstdio>
#include "repacker.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace mp3;

extern unsigned short MusicCRC16(const unsigned char *data, long len, unsigned short crc);

SuperRepacker::SuperRepacker(MemDriver *iDriver) {
    driver     = iDriver;
    offset     = (int) driver->GetPos();
    frameCount = 0;
    reservoir  = 0;
    cbrIndex   = -1;
    minIndex   = 1;
    maxIndex   = 14;
}

SuperRepacker::~SuperRepacker() {}

bool SuperRepacker::EnableRateControl(int minRate, int maxRate) {
    if (maxRate < minRate) return false;
    minIndex = -minRate;
    maxIndex = -maxRate;
    return true;
}

bool SuperRepacker::UpdateInfoTag(std::vector<u8> &frame, int64_t totalSamples) const {
    if ((int) frame.size() < 169) return false;

    /* SuperLAME hardening (not in BoCA): 169 only covers the smallest
     * side-info layout (MPEG-2 mono, tag end 4+9+0x9C). With larger side info
     * the LAME extension ends at 4 + sideInfo + 0x9C -- past the frame for
     * small CBR frames (e.g. 56 kbps stereo 44.1 = 182 bytes < 192). Pad with
     * scratch zeros so the field patches below stay in bounds, then trim back:
     * the emitted frame (the first `written` bytes) is unchanged, and fields
     * that don't fit are dropped exactly like LAME's own small-frame tag. */
    const size_t written = frame.size();
    const size_t needed  = (size_t) 4 + GetSideInfoLength(frame.data()) + 0x9C;
    if (frame.size() < needed) frame.resize(needed, 0);

    u8 *tag = frame.data() + 4 + GetSideInfoLength(frame.data());

    /* Set frame count (minus this info frame). */
    uint32_t frames = frameCount - 1;
    tag[0x08] =  frames >> 24;
    tag[0x09] = (frames >> 16) & 0xFF;
    tag[0x0A] = (frames >>  8) & 0xFF;
    tag[0x0B] =  frames        & 0xFF;

    /* Set byte count. */
    uint32_t bytes = (uint32_t) (driver->GetSize() - offset);
    tag[0x0C] =  bytes >> 24;
    tag[0x0D] = (bytes >> 16) & 0xFF;
    tag[0x0E] = (bytes >>  8) & 0xFF;
    tag[0x0F] =  bytes        & 0xFF;

    /* Write TOC. */
    for (int i = 0; i < 100; i++) {
        uint32_t off = frameOffsets[(size_t) ((int64_t) frameOffsets.size() * i / 100)];
        tag[0x10 + i] = (u8) std::min((int64_t) 255, (int64_t) std::floor(256.0 * off / bytes));
    }

    /* Set pad samples. */
    int delaySamples = (tag[0x8D] << 4) | ((tag[0x8E] & 0xF0) >> 4);
    int padSamples   = frames * (GetMode(frame.data()) == 3 ? 1152 : 576) - totalSamples - delaySamples;
    tag[0x8E] |= padSamples >> 8;
    tag[0x8F]  = padSamples & 0xFF;

    /* Set byte count (Info field). */
    tag[0x94] =  bytes >> 24;
    tag[0x95] = (bytes >> 16) & 0xFF;
    tag[0x96] = (bytes >>  8) & 0xFF;
    tag[0x97] =  bytes        & 0xFF;

    /* Calculate music CRC over all audio after the info frame. */
    driver->Seek(offset + GetFrameSize(frame.data()));
    std::vector<u8> buffer(256 * 1024);
    int64_t bytesLeft = driver->GetSize() - driver->GetPos();
    unsigned short musicCRC = 0x0000;
    while (bytesLeft) {
        int got = driver->ReadData(buffer.data(), (int) std::min(bytesLeft, (int64_t) (256 * 1024)));
        musicCRC = MusicCRC16(buffer.data(), got, musicCRC);
        bytesLeft -= got;
    }
    tag[0x98] = musicCRC >> 8;
    tag[0x99] = musicCRC & 0xFF;

    /* Info CRC over the tag header. */
    unsigned short tagCRC = MusicCRC16(frame.data(), 4 + GetSideInfoLength(frame.data()) + 0x9A, 0x0000);
    tag[0x9A] = tagCRC >> 8;
    tag[0x9B] = tagCRC & 0xFF;

    if (frame.size() != written) frame.resize(written);   /* drop scratch tail */

    return true;
}

bool SuperRepacker::UnpackFrames(const std::vector<u8> &data,
                                 std::vector<u8> &packets, std::vector<int> &packetSizes) {
    std::vector<u8> main;

    for (int n = 0; n < (int) data.size(); n++) {
        if (!IsValidFrame(data.data() + n)) continue;

        const u8 *frame = data.data() + n;

        int frameb = GetFrameSize(frame);
        int info   = GetHeaderLength(frame) + GetSideInfoLength(frame);
        int bytes  = GetMainDataLength(frame);
        int pre    = GetMainDataOffset(frame);

        /* Set CBR bitrate index. */
        if      (cbrIndex == -1)                                cbrIndex = GetBitrateIndex(frame);
        else if (cbrIndex !=  0 && cbrIndex != GetBitrateIndex(frame)) cbrIndex = 0;

        /* Set minimum/maximum VBR bitrate index. */
        int mode = GetMode(frame);
        if (minIndex < 0) for (int i =  1; i <= 14; i++) if (bitrates[mode][i] >= -minIndex || i == 14) { minIndex = i; break; }
        if (maxIndex < 0) for (int i = 14; i >=  1; i--) if (bitrates[mode][i] <= -maxIndex || i ==  1) { maxIndex = i; break; }

        /* Buffer main data. */
        size_t prevMain = main.size();
        main.resize(main.size() + frameb - info);
        memcpy(main.data() + prevMain, frame + info, frameb - info);

        /* Write packet. */
        int off  = (int) packets.size();
        int size = std::max(info + bytes, frameb);
        packets.resize(packets.size() + size);

        memcpy(packets.data() + off, frame, info);
        memcpy(packets.data() + off + info, main.data() + main.size() - (frameb - info) - pre, bytes);

        SetBitrateIndex(packets.data() + off, cbrIndex);
        SetMainDataOffset(packets.data() + off, 0);  /* keep LAME's padding (BoCA 2020-04-08) */

        packetSizes.push_back(size);

        n += frameb - 1;
    }

    return true;
}

bool SuperRepacker::WriteFrame(u8 *iFrame, int size) {
    u8 frame[maxFrameSize + 511] = { 0 }; // Frame can exceed maxFrameSize when using reservoir.

    if (frameCount++ == 0 &&
        memcmp(iFrame + GetHeaderLength(iFrame), frame, GetSideInfoLength(iFrame)) == 0) {
        driver->WriteData(iFrame, size);
        return true;
    }

    memcpy(frame, iFrame, size);

    int info  = GetHeaderLength(frame) + GetSideInfoLength(frame);
    int bytes = GetMainDataLength(frame);

    /* Fill superfluous reservoir with zero bytes. */
    int maxR = GetMode(frame) == 3 ? 511 : 255;
    FillReservoir(maxR);

    /* Process frame data. */
    while (true) {
        /* Write main data to reservoir. */
        driver->WriteData(frame + info, std::min(reservoir, bytes));
        SetMainDataOffset(frame, GetMainDataOffset(frame) + reservoir);

        /* Write next frame header and remaining bytes. */
        if (bytes >= reservoir && (int) frameBuffer.size() >= info) {
            memmove(frame + info, frame + info + reservoir, bytes - reservoir);

            bytes     -= reservoir;
            reservoir  = GetFrameSize(frameBuffer.data()) - info;

            frameOffsets.push_back((uint32_t) (driver->GetPos() - offset));
            driver->WriteData(frameBuffer.data(), info);

            memmove(frameBuffer.data(), frameBuffer.data() + info, frameBuffer.size() - info);
            frameBuffer.resize(frameBuffer.size() - info);
            continue;
        }

        /* Update main data offset and compute total reservoir size. */
        int total = reservoir;
        for (int i = 0; i < (int) frameBuffer.size(); i += info) {
            SetMainDataOffset(frame, GetMainDataOffset(frame) + GetFrameSize(frameBuffer.data() + i) - info);
            total += GetFrameSize(frameBuffer.data() + i) - info;
        }

        /* Adjust bitrate to stay under maximum reservoir size. */
        if (!cbrIndex) {
            SetBitrateIndex(frame, maxIndex);
            while (GetFrameSize(frame) - info + total - bytes > maxR) {
                if (GetBitrateIndex(frame) == minIndex) break;
                SetBitrateIndex(frame, GetBitrateIndex(frame) - 1);
            }
        }

        /* Increase reservoir if not enough bytes left. */
        int required = bytes - reservoir - (GetFrameSize(frame) - info);
        if (required > 0) {
            int prevRes = reservoir;
            IncreaseReservoir(required, frame);
            SetMainDataOffset(frame, GetMainDataOffset(frame) + reservoir - prevRes);
            driver->WriteData(frame + info + prevRes, reservoir - prevRes);
        }

        /* Revert changes if new data does not fit into frame. */
        if (bytes - reservoir - (GetFrameSize(frame) - info) > 0) {
            driver->Seek(driver->GetPos() - reservoir);
            frameCount--;
            return false;
        }

        /* Compute frame CRC. */
        if (GetHeaderLength(frame) == 6) FrameCRC::Update(frame);

        /* Process frame depending on reservoir size. */
        if (bytes >= reservoir) {
            frameOffsets.push_back((uint32_t) (driver->GetPos() - offset));
            driver->WriteData(frame, info);
            driver->WriteData(frame + info + reservoir, bytes - reservoir);
            reservoir += GetFrameSize(frame) - info - bytes;
        } else {
            frameBuffer.resize(frameBuffer.size() + info);
            memcpy(frameBuffer.data() + frameBuffer.size() - info, frame, info);
            reservoir -= bytes;
        }

        break;
    }

    return true;
}

bool SuperRepacker::Flush() {
    return FillReservoir();
}

bool SuperRepacker::FillReservoir(int threshold) {
    int total = reservoir;

    if (frameBuffer.size() > 0) {
        int info = GetHeaderLength(frameBuffer.data()) + GetSideInfoLength(frameBuffer.data());
        for (int i = 0; i < (int) frameBuffer.size(); i += info)
            total += GetFrameSize(frameBuffer.data() + i) - info;
    }

    u8 zero[maxFrameSize] = { 0 };

    while (total > threshold) {
        driver->WriteData(zero, std::min(total - threshold, reservoir));

        if (total - threshold <= reservoir) {
            reservoir -= total - threshold;
            total     -= total - threshold;
        } else {
            int info = GetHeaderLength(frameBuffer.data()) + GetSideInfoLength(frameBuffer.data());
            total     -= reservoir;
            reservoir  = GetFrameSize(frameBuffer.data()) - info;

            driver->WriteData(frameBuffer.data(), info);
            memmove(frameBuffer.data(), frameBuffer.data() + info, frameBuffer.size() - info);
            frameBuffer.resize(frameBuffer.size() - info);
        }
    }

    return true;
}

bool SuperRepacker::IncreaseReservoir(int bytes, u8 *reference, int depth) {
    /* Limit recursion depth to avoid stack overflow (BoCA 2021-04-14). */
    if (depth == 256) return false;

    /* Find last written frame. Read up to two frames' worth of context, and
     * never before the first frame (BoCA 2021-04-14). */
    u8 data[2 * maxFrameSize];
    int dataSize = (int) std::min((int64_t) (driver->GetPos() - offset), (int64_t) sizeof(data));

    driver->Seek(driver->GetPos() - dataSize);
    driver->ReadData(data, dataSize);

    for (int n = dataSize - 13; n >= 0; n--) {
        if (!IsValidFrame(data + n, reference)) continue;

        u8 *frame = data + n;

        int frameb  = GetFrameSize(frame);
        int nframeb = GetFrameSize(frame);

        /* Verify that frame size looks correct. */
        if (frameb != dataSize - n) continue;

        /* Verify that a frame precedes this one at the correct position
         * (BoCA 2019-12-22: robust frame synchronization). */
        if (!CheckPrecedingFrame(data, n, reference)) continue;

        int off  = (int) (driver->GetPos() - frameb);
        int info = GetHeaderLength(frame) + GetSideInfoLength(frame);
        int pre  = GetMainDataOffset(frame);
        int maxR = GetMode(frame) == 3 ? 511 : 255;

        if (reservoir + bytes >= maxR) return false;

        int prevRes = reservoir;

        /* Check if frame can be enlarged. */
        int brindex = GetBitrateIndex(frame);
        while ((brindex < maxIndex && !cbrIndex) || !GetPadding(frame)) {
            if ((brindex == maxIndex || cbrIndex) || (nframeb - frameb == bytes - 1 && !GetPadding(frame))) SetPadding(frame, true);
            else                                                                                            SetBitrateIndex(frame, ++brindex);

            nframeb = GetFrameSize(frame);

            if (nframeb - frameb >= bytes) {
                if (GetHeaderLength(frame) == 6) FrameCRC::Update(frame);

                driver->Seek(off);
                driver->WriteData(frame, std::max(info, frameb - prevRes));
                reservoir += nframeb - frameb;

                FillReservoir(maxR);
                driver->WriteData(frame + std::max(info, frameb - prevRes), std::min(prevRes, frameb - info));
                return true;
            }
        }

        /* Recursively try enlarging previous frames. */
        driver->Seek(off);
        reservoir = pre;

        bool result = IncreaseReservoir(bytes - (nframeb - frameb), reference, depth + 1);

        SetMainDataOffset(frame, reservoir);

        if (GetHeaderLength(frame) == 6) FrameCRC::Update(frame);

        /* Clamp the trailing write so main data bigger than a frame can't
         * overrun into the next header (BoCA 2019-12-26 -- THE desync fix). */
        int frameRes = std::min(frameb - info - (reservoir - pre), prevRes);

        driver->WriteData(frame + info, reservoir - pre);
        driver->WriteData(frame, info);
        driver->WriteData(frame + info + reservoir - pre, frameb - info - (reservoir - pre) - frameRes);

        reservoir += prevRes - pre + nframeb - frameb;

        FillReservoir(maxR);
        driver->WriteData(frame + frameb - frameRes, frameRes);
        return result;
    }

    return false;
}

bool SuperRepacker::CheckPrecedingFrame(u8 *data, int off, u8 *reference) {
    for (int n = off - 13; n >= 0; n--) {
        if (!IsValidFrame(data + n, reference)) continue;
        if (GetFrameSize(data + n) != off - n) continue;
        return true;
    }
    return false;
}
