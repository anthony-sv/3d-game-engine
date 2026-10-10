/* Implementation translation unit of miniaudio. Compiled with warnings disabled (third-party code); see
 * strada_configure_third_party_target. Ogg Vorbis is decoded by libvorbis through miniaudio's libvorbis backend, compiled
 * next to this file: the stb_vorbis copy miniaudio ships frees uninitialized pointers when the headers of a damaged file
 * fail to parse, so it stays out (miniaudio enables it only when its declarations precede the implementation). */

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
