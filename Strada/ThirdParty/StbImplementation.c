/* Implementation translation unit for the stb single-file libraries used by Strada. Compiled with warnings disabled
 * (third-party code); see strada_configure_third_party_target. */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
