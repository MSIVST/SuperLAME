/* superlame-mt: static member definitions for mp3::FrameCRC, plus the
 * Xing/Info-tag "music CRC" which is CRC-16/ARC (poly 0x8005, init 0x0000,
 * input+output reflected, no final xor) -- matching smooth's Hash::CRC16. */
#include "mp3frame.h"

namespace mp3 {
    u16  FrameCRC::table[256];
    bool FrameCRC::initialized = FrameCRC::InitTable();
}

/* CRC-16/ARC for the Info-tag music CRC. Reflected form. */
static unsigned short arc_table[256];
static bool arc_init = []() {
    for (int i = 0; i < 256; i++) {
        unsigned short c = (unsigned short) i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1); // 0xA001 = reflect(0x8005)
        arc_table[i] = c;
    }
    return true;
}();

unsigned short MusicCRC16(const unsigned char *data, long len, unsigned short crc) {
    (void) arc_init;
    while (len--) crc = (crc >> 8) ^ arc_table[(crc ^ *data++) & 0xFF];
    return crc;
}
