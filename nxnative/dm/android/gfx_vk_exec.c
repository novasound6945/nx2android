// Android port, Vulkan backend: the command replay (mtl_exec, see gfx_vk.h and nvn_obj.h's record format).
// State records update a shadow of the NVN state; a draw or dispatch turns it into a pipeline (cached by everything that shapes it), descriptor sets (one per
// stage, filled from the shader's reflection: uniform slots, the shader's c1, the driver's c0 / c2 segments, textures and samplers by handle, images, storage
// buffers) and the draw. Render passes open lazily at the first draw or clear after SetRenderTargets and close before anything that is not drawing.
// Version 1 orders everything with whole-pipeline barriers around passes, copies and dispatches.
#include "gfx_vk.h"
#include "gfx_vk_slot.h"
#include "vk_occ.h"
#include "vk_gpl.h"
#include "nvn_hmap.h"
#include "game_profile.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

VkPipelineLayout vk_pipeline_layout(VkDescriptorSetLayout sets[5], int nsets);
VkPipelineLayout vk_pipeline_layout_gfx(VkDescriptorSetLayout sets[5], int nsets);   // a graphics pipeline's (with V.gpl: independent sets, gfx_vk_gpl.inc)
void vk_shader_c1(vkshader_t *s, VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *len);
VkBufferView vk_tbuf_view(const tex_t *t, int storage);
int vk_tbuf_float(const tex_t *t);   // gfx_vk_tex.c: 32-bit integer texels read through a float view (DM_VK_TBUF_FLOAT): no integer-texel variant
VkBufferView vk_tbuf_view_f(const tex_t *t, int storage, int as_float);
void vk_sub_update(uint64_t dst, const void *data, uint64_t n);
int prog_key(const prog_t *p, unsigned st, uint8_t key[8]);

#include "gfx_vk_exec_state.inc"   // the NVN state shadow: what the game set, read by the walker and the draws
#include "gfx_vk_exec_render_cond.inc"   // conditional rendering (profile render_cond)
#include "gfx_vk_exec_passes.inc"   // render passes: attachments, aliases, LRZ touches, clears at pass start, pass splits
#include "gfx_vk_exec_formats.inc"   // formats and enums: NVN to Vulkan
#include "gfx_vk_exec_pipelines.inc"   // programs and pipelines: creation, the prewarm record, the per-thread build note for crash dumps
#include "gfx_vk_exec_descriptors.inc"   // descriptors: sets, the descriptor hash, texture and sampler handles
#include "gfx_vk_exec_draws.inc"   // draws: vertex input, index and indirect draws, the BC stand-in draw
#include "gfx_vk_exec_xfb.inc"   // transform feedback and the geometry-shader split
#include "gfx_vk_exec_copies.inc"   // copies and clears
#include "gfx_vk_exec_walker.inc"   // the record walker: the switch over recorded NVN commands, x_read, the profiler
