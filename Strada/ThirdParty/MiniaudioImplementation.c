/* Implementation translation unit of miniaudio, with Ogg Vorbis decoding through the stb_vorbis copy miniaudio ships.
 * Compiled with warnings disabled (third-party code); see strada_configure_third_party_target. */

/* miniaudio enables its Vorbis decoder when stb_vorbis's declarations precede its implementation. */
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#if !defined(MA_HAS_VORBIS)
#error "miniaudio was built without Ogg Vorbis support"
#endif

/* stb_vorbis's implementation follows miniaudio's. */
#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
