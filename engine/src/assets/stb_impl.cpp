// stb_image implementation TU.
// Cap decoded image dimensions so a hostile header cannot trigger huge
// allocations (STBI_MAX_DIMENSIONS makes stb fail loads above this).
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
