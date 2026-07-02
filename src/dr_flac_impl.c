/* Single translation unit that compiles the dr_flac implementation.
 * Everywhere else includes ../dr_flac/dr_flac.h as a plain header (decls only).
 * dr_flac auto-selects SSE2/etc. at runtime, so this one object is portable. */
#define DR_FLAC_IMPLEMENTATION
#include "../dr_flac/dr_flac.h"
