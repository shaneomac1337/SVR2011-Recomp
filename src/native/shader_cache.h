// Symbols XenosRecomp emits into cache/shader-native/shader_cache.cpp
// (scripts/convert-shaders.ps1). The generated file names them unqualified,
// so they stay in the global namespace with the emitter's field order.
// Builds without a local cache compile shader_cache_empty.cpp instead.

#pragma once

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry {
  const uint64_t hash;  // XXH3-64 of the whole shader container
  const uint32_t dxilOffset;
  const uint32_t dxilSize;
  const uint32_t spirvOffset;  // into the decompressed SPIR-V blob, smol-v encoded
  const uint32_t spirvSize;
  const uint32_t specConstantsMask;
};

// Sorted by hash (the emitter walks a std::map).
extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
