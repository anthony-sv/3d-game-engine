/* Implementation translation unit for the stb single-file libraries used by Strada. Compiled with warnings disabled
 * (third-party code); see strada_configure_third_party_target. */

/* Only the decoders of the formats the engine reads (textures: PNG, JPEG, TGA, BMP; environments: Radiance HDR): the
 * others would only add code that user files can reach. Images larger than GPUs sample (16384 pixels on a side) are
 * refused before any pixel is allocated. */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_HDR
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
