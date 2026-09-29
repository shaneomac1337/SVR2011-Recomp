// Used when no converted shader cache exists locally; the native renderer then
// stays off. See scripts/convert-shaders.ps1.

#include "native/shader_cache.h"

ShaderCacheEntry g_shaderCacheEntries[1] = {{0, 0, 0, 0, 0, 0}};
const size_t g_shaderCacheEntryCount = 0;

const uint8_t g_compressedSpirvCache[1] = {0};
const size_t g_spirvCacheCompressedSize = 0;
const size_t g_spirvCacheDecompressedSize = 0;
