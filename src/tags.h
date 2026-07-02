/* superlame-mt: ID3 tag rendering.
 *
 * SuperFast assembles the MP3 from many worker streams, so we can't let the
 * encoder emit tags inline. Instead the frontend renders the tag bytes via
 * LAME's lame_get_id3v2_tag / lame_get_id3v1_tag (which just serialize the
 * tag, no encoding) and writes ID3v2 BEFORE and ID3v1 AFTER the stream. */
#ifndef SUPERLAME_TAGS_H
#define SUPERLAME_TAGS_H

#include "lame_dispatch.h"
#include <lame.h>
#include <string>
#include <vector>

struct TagConfig {
    std::string title, artist, album, year, comment, track, genre;
    bool any   = false;     // any text field set
    bool v1    = true;      // write ID3v1 (default on when tagging)
    bool v2    = true;      // write ID3v2 (default on when tagging)
};

/* Render the requested tags into v2bytes (to prepend) and v1bytes (to append).
 * Uses a throwaway lame context just to serialize the tag. */
inline void RenderTags(const LameEngine &e, const TagConfig &t,
                       std::vector<unsigned char> &v2bytes,
                       std::vector<unsigned char> &v1bytes) {
    v2bytes.clear();
    v1bytes.clear();
    if (!t.any) return;

    lame_t g = e.init();
    e.id3tag_init(g);
    if (t.v2) {
        /* Use ID3v2.4 + UTF-8 so non-Latin-1 tag text (accents, CJK, etc.)
         * round-trips correctly. Our argv strings are already UTF-8. With this
         * flag the plain set_* setters route through LAME's UTF-8 tag path. */
        e.id3tag_add_v2_4_UTF8(g);
    }

    if (!t.title.empty())   e.id3tag_set_title(g, t.title.c_str());
    if (!t.artist.empty())  e.id3tag_set_artist(g, t.artist.c_str());
    if (!t.album.empty())   e.id3tag_set_album(g, t.album.c_str());
    if (!t.year.empty())    e.id3tag_set_year(g, t.year.c_str());
    if (!t.comment.empty()) e.id3tag_set_comment(g, t.comment.c_str());
    if (!t.track.empty())   e.id3tag_set_track(g, t.track.c_str());
    if (!t.genre.empty())   e.id3tag_set_genre(g, t.genre.c_str());

    if (t.v2) {
        size_t need = e.lame_get_id3v2_tag(g, nullptr, 0);
        if (need > 0) {
            v2bytes.resize(need);
            size_t got = e.lame_get_id3v2_tag(g, v2bytes.data(), need);
            v2bytes.resize(got <= need ? got : 0);
        }
    }
    if (t.v1) {
        size_t need = e.lame_get_id3v1_tag(g, nullptr, 0);
        if (need > 0) {
            v1bytes.resize(need);
            size_t got = e.lame_get_id3v1_tag(g, v1bytes.data(), need);
            v1bytes.resize(got <= need ? got : 0);
        }
    }
    e.close(g);
}

#endif
