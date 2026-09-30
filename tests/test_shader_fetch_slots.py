import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

from shader_fetch_slots import ALL_SLOTS, slot_mask  # noqa: E402

DEFINES = """#ifdef __spirv__
#define sampDiffuse_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0)
#define sampDiffuse_Texture3DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 128)
#define sampDiffuse_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 384)
#define sampShadow_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 48)
#define sampVertex_TextureCubeDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 320)
#else
\tuint sampUnused_Texture2DDescriptorIndex : packoffset(c1.y);
#endif
"""


class SlotMaskTest(unittest.TestCase):
    def test_body_references_select_slots(self):
        body = ("r0 = tfetch2D(sampDiffuse_Texture2DDescriptorIndex, "
                "sampDiffuse_SamplerDescriptorIndex, r1.xy, float2(0, 0));\n"
                "r2 = tfetchCube(sampVertex_TextureCubeDescriptorIndex, s0_SamplerDescriptorIndex, r3);\n")
        # s0 is not defined here: unknown names bind every slot.
        self.assertEqual(slot_mask(DEFINES + body), ALL_SLOTS)
        body = body.replace("s0_SamplerDescriptorIndex", "sampVertex_SamplerDescriptorIndex")
        defines = DEFINES + ("#define sampVertex_SamplerDescriptorIndex "
                             "vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 448)\n")
        self.assertEqual(slot_mask(defines + body), (1 << 0) | (1 << 16))

    def test_defines_and_cbuffer_alone_use_nothing(self):
        self.assertEqual(slot_mask(DEFINES), 0)

    def test_shadow_slot(self):
        body = "x = tfetch2D(sampShadow_Texture2DDescriptorIndex, sampShadow_Texture2DDescriptorIndex, uv);"
        self.assertEqual(slot_mask(DEFINES + body), 1 << 12)


if __name__ == "__main__":
    unittest.main()
