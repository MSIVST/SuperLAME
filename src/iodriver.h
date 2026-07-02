/* superlame-mt: standalone SuperFast multithreaded LAME frontend.
 *
 * Minimal seekable output driver, replacing BoCA's IO::Driver. The repacker
 * needs random-access seek/read/write over the output MP3 (it retroactively
 * rewrites earlier frames to grow the bit reservoir), so we keep the whole
 * output in a growable in-memory buffer and flush it to disk at the end.
 *
 * Semantics intentionally match IO::Driver as used by SuperRepacker:
 *   GetPos()  -> current cursor
 *   GetSize() -> logical end of data written so far
 *   Seek(p)   -> move cursor (absolute)
 *   WriteData(buf,n) -> overwrite/extend at cursor, advancing it
 *   ReadData(buf,n)  -> read at cursor, advancing it; returns bytes read
 */
#ifndef SUPERLAME_IODRIVER_H
#define SUPERLAME_IODRIVER_H

#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>

class MemDriver {
private:
    std::vector<unsigned char> data;
    size_t pos = 0;

public:
    int64_t GetPos() const  { return (int64_t) pos; }
    int64_t GetSize() const { return (int64_t) data.size(); }

    void Seek(int64_t p) { pos = (size_t) p; }

    /* Write n bytes at cursor, overwriting existing data and extending as
     * needed. Advances cursor. Returns n. */
    int WriteData(const unsigned char *buf, int n) {
        if (n <= 0) return 0;
        if (pos + (size_t) n > data.size()) data.resize(pos + (size_t) n);
        memcpy(&data[pos], buf, (size_t) n);
        pos += (size_t) n;
        return n;
    }

    /* Read up to n bytes at cursor. Advances cursor. Returns bytes read. */
    int ReadData(unsigned char *buf, int n) {
        if (n <= 0 || pos >= data.size()) return 0;
        int avail = (int) std::min((size_t) n, data.size() - pos);
        memcpy(buf, &data[pos], (size_t) avail);
        pos += (size_t) avail;
        return avail;
    }

    const std::vector<unsigned char> &Bytes() const { return data; }

    /* Write the buffered output to an already-open binary FILE (caller owns it).
     * Opening is done by the caller so it can use a Unicode-safe path open. */
    bool WriteToFILE(FILE *f) const {
        if (!f) return false;
        return data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    }

    bool FlushToFile(const char *path) const {
        FILE *f = fopen(path, "wb");
        if (!f) return false;
        bool ok = WriteToFILE(f);
        fclose(f);
        return ok;
    }
};

#endif
