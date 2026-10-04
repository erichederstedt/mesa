#include "zgfx_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "ac_linux_drm.h"
#include "ac_cmdbuf.h"
#include "ac_cmdbuf_cp.h"
#include "ac_pm4.h"
#include "ac_shader_args.h"
#include "amd_family.h"
#include "sid.h"
#include "util/macros.h"
#include "util/u_math.h"

PUBLIC void zgfx_hello_world(void)
{
   puts("Hello, world!");
}

PUBLIC zgfx_device *device_create(void) {
   zgfx_device *dev = malloc(sizeof(zgfx_device));
   assert(dev);
   
   memset(dev, 0, sizeof(zgfx_device));
   // dev->fd = open("/dev/dri/card0", O_RDWR);
   dev->fd = open("/dev/dri/renderD128", O_RDWR);
   assert(dev->fd >= 0 && "failed to open card0");
   
   uint32_t major_version = 0;
   uint32_t minor_version = 0;
   int err = amdgpu_device_initialize(dev->fd, &major_version, &minor_version, &dev->device_handle);
   assert(err == 0 && "failed to amdgpu_device_initialize");
   printf("amdgpu initialized with major version: %d, minor version: %d\n", major_version, minor_version);
   
   ac_drm_device *query_dev = NULL;
   err = ac_drm_device_initialize(dev->fd, false, &dev->info.drm_major, &dev->info.drm_minor, &query_dev);
   assert(err == 0 && "failed to ac_drm_device_initialize");

   enum ac_query_gpu_info_result result = ac_query_gpu_info(dev->fd, query_dev, &dev->info, false, false);
   ac_drm_device_deinitialize(query_dev);
   assert(result == AC_QUERY_GPU_INFO_SUCCESS && "failed to ac_query_gpu_info");
   assert(dev->info.gfx_level >= GFX6 && "minimum required GFX level is 6");
   
   err = amdgpu_cs_ctx_create(dev->device_handle, &dev->context_handle);
   assert(err == 0 && "failed to amdgpu_cs_ctx_create");

   ac_print_gpu_info(stdout, &dev->info, dev->fd);
   printf("succesfully created device\n");
   
   return dev;
}

PUBLIC uint8_t format_is_block_compressed(zgfx_color_format format) {
   switch (format) {
      case ZGFX_COLOR_RGBA32_FLOAT:
      case ZGFX_COLOR_RGBA8_UNORM:
         return 0;
      default:
         assert(0 && "Unsupported format");
      return 0;
   }
}
PUBLIC uint64_t format_bit_size(zgfx_color_format format) {
   switch (format) {
      case ZGFX_COLOR_RGBA32_FLOAT:
         return 128;
      case ZGFX_COLOR_RGBA8_UNORM:
         return 32;
      default:
         assert(0 && "Unsupported format");
      return 0;
   }
}
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
PUBLIC uint64_t format_compute_row_pitch_size(zgfx_color_format format, uint64_t width) {
   if (format == ZGFX_COLOR_UNKOWN) {
      return width;
   } else if (format_is_block_compressed(format)) {
      return MAX(1, ((width + 3) / 4)) * ((format_bit_size(format) == 4) ? 8 : 16);
   } else {
      size_t bits_per_pixel = format_bit_size(format);
      if (bits_per_pixel == 0)
         return 0;
      size_t row_bytes = (width * bits_per_pixel + 7) / 8;
      return row_bytes;
   }
}
PUBLIC uint64_t format_compute_mip_size(zgfx_color_format format, uint64_t width, uint64_t height) {
   if (format_is_block_compressed(format)) {
      return MAX(1, ((width + 3) / 4)) * MAX(1, ((height + 3) / 4)) * ((format_bit_size(format) == 4) ? 8 : 16);
   } else {
      size_t bits_per_pixel = format_bit_size(format);
      if (bits_per_pixel == 0)
         return 0;
      size_t row_bytes = (width * bits_per_pixel + 7) / 8;
      return row_bytes * height;
   }
}

PUBLIC void *galloc(zgfx_device *dev, uint64_t size) {
   return galloca(dev, size, 0);
}
PUBLIC void *galloca(zgfx_device *dev, uint64_t size, uint64_t align) {
   assert((align & (align - 1)) == 0 && "align must be a power of two (0 = none)");
   
   /* Over-allocate by `align` extra bytes so an aligned address is always
   * available somewhere inside the mapping, regardless of where the CPU's
   * mmap (page-granular only) happens to land. Then round up to the page
   * size: amdgpu_bo_va_op_raw (unlike amdgpu_bo_va_op) sends map_size to
   * the kernel unmodified, and the VA-map ioctl requires it page-aligned. */
   uint64_t page_size = (uint64_t)getpagesize();
   uint64_t padded_size = (size + align + page_size - 1) & ~(page_size - 1);
   
   amdgpu_bo_handle bo_handle = {0};
   amdgpu_va_handle va_handle = {0};
   uint64_t address = {0};
   
   struct amdgpu_bo_alloc_request alloc_request = {0};
   alloc_request.alloc_size = padded_size;
   alloc_request.phys_alignment = align;
   alloc_request.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
   int err = amdgpu_bo_alloc(dev->device_handle, &alloc_request, &bo_handle);
   assert(err == 0 && "failed to amdgpu_bo_alloc");
   void *raw_ptr = 0;
   amdgpu_bo_cpu_map(bo_handle, &raw_ptr);
   
   uint64_t raw_addr = (uint64_t)raw_ptr;
   void *ptr = (void *)(raw_addr + (align ? (align - raw_addr % align) % align : 0));
   
   uint64_t flags = 0;
   uint64_t start = 0;
   uint64_t end = 0;
   err = amdgpu_va_range_query(dev->device_handle, amdgpu_gpu_va_range_general, &start, &end);
   assert(err == 0 && "failed to amdgpu_va_range_query");
   assert((uint64_t)raw_ptr >= start && "ptr is in an unsupported range");
   if (((uint64_t)raw_ptr + padded_size) > end)
      flags = AMDGPU_VA_RANGE_HIGH;
   
   /* Pin the VA to raw_ptr (not the aligned ptr) and cover the whole padded
   * range with a single 1:1 mapping -- the aligned sub-pointer then gets a
   * matching GPU VA for free, without a second VA op. Alignment must be 0
   * here: we're deliberately requesting an address that raw_ptr's own
   * (page-granular) alignment doesn't satisfy for anything above 4096. */
   err = amdgpu_va_range_alloc(dev->device_handle, amdgpu_gpu_va_range_general, padded_size, 0, (uint64_t)raw_ptr, &address, &va_handle, flags);
   assert(err == 0 && "failed to amdgpu_va_range_alloc");
   
   err = amdgpu_bo_va_op_raw(dev->device_handle, bo_handle, 0, padded_size, address, AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP);
   if (err != 0)
      printf("error: %s\n", strerror(errno));
   assert(err == 0 && "failed to amdgpu_bo_va_op_raw");
   
   assert((uint64_t)raw_ptr == address && "failed to allocated the same address");
   
   uint64_t buffer_index = dev->buffers_count++;
   assert(buffer_index < MAX_BUFFER_COUNT && "failed to record buffer allocation");
   dev->buffers_raw_ptr[buffer_index] = raw_ptr;
   dev->buffers_ptr[buffer_index] = ptr; /* the aligned pointer -- what every caller actually uses */
   dev->buffers_size[buffer_index] = size;
   dev->buffers_align[buffer_index] = align;
   dev->buffers_bo_handle[buffer_index] = bo_handle;
   dev->buffers_va_handle[buffer_index] = va_handle;
   return ptr;
}
PUBLIC void *galloct(zgfx_device *dev, uint64_t width, uint64_t height, zgfx_color_format format, uint64_t mip_count, uint64_t array_count) {
   assert(mip_count >= 1 && "mip_count must be at least 1");
   assert(array_count >= 1 && "array_count must be at least 1");
   
   uint64_t bytes_per_pixel = format_bit_size(format) / 8;
   uint64_t align_pixels = 256 / bytes_per_pixel;
   
   uint64_t size = 0;
   for (uint64_t array_element = 0; array_element < array_count; array_element++) {
      for (uint64_t mip = 0; mip < mip_count; mip++) {
         uint64_t mip_width = MAX(1, width >> mip);
         uint64_t mip_height = MAX(1, height >> mip);
         uint64_t aligned_width = ((mip_width + align_pixels - 1) / align_pixels) * align_pixels;
         size += format_compute_mip_size(format, aligned_width, mip_height);
      }
   }
   
   return galloc(dev, size);
}

static void command_emit_compute(zgfx_command *cmd, bool is_compute_queue) {
   const struct radeon_info *info = &cmd->dev->info;
   struct ac_pm4_state *pm4 = ac_pm4_create_sized(info, false, 64, is_compute_queue);
   assert(pm4);

   const struct ac_preamble_state preamble_state = {
      .border_color_va = 0,
      .gfx11.compute_dispatch_interleave = 64,
   };

   ac_init_compute_preamble_state(&preamble_state, pm4);

   ac_pm4_set_reg(pm4, R_00B810_COMPUTE_START_X, 0);
   ac_pm4_set_reg(pm4, R_00B814_COMPUTE_START_Y, 0);
   ac_pm4_set_reg(pm4, R_00B818_COMPUTE_START_Z, 0);

   if (info->gfx_level >= GFX12) {
      if (is_compute_queue) {
         ac_pm4_set_reg(pm4, R_00B8BC_COMPUTE_DISPATCH_INTERLEAVE,
                        S_00B8BC_INTERLEAVE_1D(preamble_state.gfx11.compute_dispatch_interleave));
      } else {
         ac_pm4_set_reg_custom(pm4, R_00B8BC_COMPUTE_DISPATCH_INTERLEAVE - SI_SH_REG_OFFSET,
                               S_00B8BC_INTERLEAVE_1D(preamble_state.gfx11.compute_dispatch_interleave),
                               PKT3_SET_SH_REG_INDEX, 2);
      }
   }

   ac_pm4_finalize(pm4);
   assert(cmd->cs.cdw + pm4->ndw <= cmd->cs.max_dw);
   ac_pm4_emit_commands(&cmd->cs, pm4);
   ac_pm4_free_state(pm4);
}

static unsigned pack_float_12p4(float x) {
   return x <= 0 ? 0 : x >= 4096 ? 0xffff : x * 16;
}

static void command_emit_graphics(zgfx_command *cmd) {
   const struct radeon_info *info = &cmd->dev->info;
   const bool has_clear_state = info->has_clear_state;
   struct ac_pm4_state *pm4 = ac_pm4_create_sized(info, false, 512, false);
   assert(pm4);

   ac_pm4_cmd_add(pm4, PKT3(PKT3_CONTEXT_CONTROL, 1, 0));
   ac_pm4_cmd_add(pm4, S_281_UPDATE_LOAD_ENABLES(1));
   ac_pm4_cmd_add(pm4, S_282_UPDATE_SHADOW_ENABLES(1));

   if (has_clear_state) {
      ac_pm4_cmd_add(pm4, PKT3(PKT3_CLEAR_STATE, 0, 0));
      ac_pm4_cmd_add(pm4, 0);
   }

   const struct ac_preamble_state preamble_state = {
      .border_color_va = 0,
   };

   ac_init_graphics_preamble_state(&preamble_state, pm4);

   if (!has_clear_state) {
      for (unsigned i = 0; i < 16; i++) {
         ac_pm4_set_reg(pm4, R_0282D0_PA_SC_VPORT_ZMIN_0 + i * 8, 0);
         ac_pm4_set_reg(pm4, R_0282D4_PA_SC_VPORT_ZMAX_0 + i * 8, fui(1.0));
      }
      ac_pm4_set_reg(pm4, R_028230_PA_SC_EDGERULE, 0xAAAAAAAA);
   }

   if (info->gfx_level <= GFX8)
      ac_pm4_set_reg(pm4, R_00B324_SPI_SHADER_PGM_HI_ES, S_00B324_MEM_BASE(info->address32_hi >> 8));

   if (info->gfx_level < GFX11)
      ac_pm4_set_reg(pm4, R_00B124_SPI_SHADER_PGM_HI_VS, S_00B124_MEM_BASE(info->address32_hi >> 8));

   if (info->gfx_level >= GFX10) {
      unsigned vertex_reuse_depth = info->gfx_level >= GFX10_3 ? 30 : 0;
      ac_pm4_set_reg(pm4, R_028838_PA_CL_NGG_CNTL,
                     S_028838_INDEX_BUF_EDGE_FLAG_ENA(0) | S_028838_VERTEX_REUSE_DEPTH(vertex_reuse_depth));
   }

   unsigned tmp = (unsigned)(1.0 * 8.0);
   ac_pm4_set_reg(pm4, R_028A00_PA_SU_POINT_SIZE, S_028A00_HEIGHT(tmp) | S_028A00_WIDTH(tmp));
   ac_pm4_set_reg(pm4, R_028A04_PA_SU_POINT_MINMAX,
                  S_028A04_MIN_SIZE(pack_float_12p4(0)) | S_028A04_MAX_SIZE(pack_float_12p4(8191.875 / 2)));

   if (info->family >= CHIP_POLARIS10) {
      unsigned small_prim_filter_cntl = S_028830_SMALL_PRIM_FILTER_ENABLE(1) |
                                        S_028830_LINE_FILTER_DISABLE(info->family <= CHIP_POLARIS12) |
                                        S_028830_SC_1XMSAA_COMPATIBLE_DISABLE(info->gfx_level >= GFX10);

      ac_pm4_set_reg(pm4, R_028830_PA_SU_SMALL_PRIM_FILTER_CNTL, small_prim_filter_cntl);
   }

   if (info->gfx_level >= GFX12) {
      ac_pm4_set_reg(pm4, R_028644_SPI_INTERP_CONTROL_0,
                     S_0286D4_FLAT_SHADE_ENA(1) | S_0286D4_PNT_SPRITE_ENA(1) |
                        S_0286D4_PNT_SPRITE_OVRD_X(V_0286D4_SPI_PNT_SPRITE_SEL_S) |
                        S_0286D4_PNT_SPRITE_OVRD_Y(V_0286D4_SPI_PNT_SPRITE_SEL_T) |
                        S_0286D4_PNT_SPRITE_OVRD_Z(V_0286D4_SPI_PNT_SPRITE_SEL_0) |
                        S_0286D4_PNT_SPRITE_OVRD_W(V_0286D4_SPI_PNT_SPRITE_SEL_1) |
                        S_0286D4_PNT_SPRITE_TOP_1(0));
   } else {
      ac_pm4_set_reg(pm4, R_0286D4_SPI_INTERP_CONTROL_0,
                     S_0286D4_FLAT_SHADE_ENA(1) | S_0286D4_PNT_SPRITE_ENA(1) |
                        S_0286D4_PNT_SPRITE_OVRD_X(V_0286D4_SPI_PNT_SPRITE_SEL_S) |
                        S_0286D4_PNT_SPRITE_OVRD_Y(V_0286D4_SPI_PNT_SPRITE_SEL_T) |
                        S_0286D4_PNT_SPRITE_OVRD_Z(V_0286D4_SPI_PNT_SPRITE_SEL_0) |
                        S_0286D4_PNT_SPRITE_OVRD_W(V_0286D4_SPI_PNT_SPRITE_SEL_1) |
                        S_0286D4_PNT_SPRITE_TOP_1(0));
   }

   ac_pm4_set_reg(pm4, R_028BE4_PA_SU_VTX_CNTL,
                  S_028BE4_PIX_CENTER(1) | S_028BE4_ROUND_MODE(V_028BE4_X_ROUND_TO_EVEN) |
                     S_028BE4_QUANT_MODE(V_028BE4_X_16_8_FIXED_POINT_1_256TH));

   if (info->gfx_level >= GFX12) {
      ac_pm4_set_reg(pm4, R_028814_PA_CL_VTE_CNTL,
                     S_028818_VTX_W0_FMT(1) | S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
                        S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) | S_028818_VPORT_Z_SCALE_ENA(1) |
                        S_028818_VPORT_Z_OFFSET_ENA(1));
   } else {
      ac_pm4_set_reg(pm4, R_028818_PA_CL_VTE_CNTL,
                     S_028818_VTX_W0_FMT(1) | S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
                        S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) | S_028818_VPORT_Z_SCALE_ENA(1) |
                        S_028818_VPORT_Z_OFFSET_ENA(1));
   }

   ac_pm4_set_reg(pm4, R_028828_PA_SU_LINE_STIPPLE_SCALE, 0x3f800000);

   if (info->gfx_level >= GFX12)
      ac_pm4_set_reg(pm4, R_028000_DB_RENDER_CONTROL, 0);

   if (info->family >= CHIP_NAVI31 && info->family <= CHIP_STRIX1)
      ac_pm4_set_reg(pm4, R_028424_CB_FDCC_CONTROL, S_028424_DISABLE_CONSTANT_ENCODE_SINGLE(1));

   ac_pm4_finalize(pm4);
   assert(cmd->cs.cdw + pm4->ndw <= cmd->cs.max_dw);
   ac_pm4_emit_commands(&cmd->cs, pm4);
   ac_pm4_free_state(pm4);

   command_emit_compute(cmd, false);
}

PUBLIC zgfx_command *command_begin(zgfx_device *dev) {
   zgfx_command* cmd = malloc(sizeof(zgfx_command));
   assert(cmd);

   uint64_t capacity_bytes = 4096;
   cmd->cs = (struct ac_cmdbuf){
      .buf = galloc(dev, capacity_bytes),
      .max_dw = (capacity_bytes / sizeof(uint32_t)) - 4, // reserves last 4 dwords for chaining
      .reserved_dw = (capacity_bytes / sizeof(uint32_t)) - 4,
   };

   cmd->ib_ptrs = calloc(8, sizeof(void*));
   cmd->ib_ptrs[0] = cmd->cs.buf;
   cmd->ib_sizes = calloc(8, sizeof(uint64_t));
   cmd->ib_sizes[0] = capacity_bytes;
   cmd->ib_cwds = calloc(8, sizeof(uint64_t));
   cmd->ib_count = 1;
   cmd->ib_capacity = 8;
   cmd->dev = dev;
   cmd->compute_shader = NULL;

   command_emit_graphics(cmd);

   return cmd;
}
static void command_chain(zgfx_command *cmd, uint64_t old_cdw) {
   assert(cmd->ib_count >= 2);

   uint64_t old_index = cmd->ib_count - 2;
   uint64_t new_index = cmd->ib_count - 1;
   uint32_t *old_ib = cmd->ib_ptrs[old_index];
   uint64_t next_va = (uint64_t)(uintptr_t)cmd->ib_ptrs[new_index];

   uint64_t pad_mask = cmd->dev->info.ip[AMD_IP_GFX].ib_pad_dw_mask;
   uint64_t final_cdw = (old_cdw + 4 + pad_mask) & ~pad_mask;

   assert(final_cdw <= cmd->ib_sizes[old_index] / sizeof(uint32_t));
   assert(final_cdw <= G_3F3_IB_SIZE(UINT32_MAX));
   assert(next_va % cmd->dev->info.ip[AMD_IP_GFX].ib_alignment == 0);

   uint32_t nop = cmd->dev->info.gfx_ib_pad_with_type2
                    ? PKT2_NOP_PAD : PKT3_NOP_PAD;

   struct ac_cmdbuf old_cs = {
      .buf = old_ib,
      .cdw = old_cdw,
      .max_dw = final_cdw,
      .reserved_dw = final_cdw,
   };

   while (old_cs.cdw + 2 <= final_cdw - 4)
      ac_emit_cp_nop(&old_cs, 0);

   if (old_cs.cdw < final_cdw - 4)
      old_cs.buf[old_cs.cdw++] = nop;

   ac_emit_cp_indirect_buffer(
      &old_cs, next_va, 0,
      AC_CP_INDIRECT_BUFFER_CHAIN | AC_CP_INDIRECT_BUFFER_VALID,
      false);

   old_cdw = old_cs.cdw;

   cmd->ib_cwds[old_index] = old_cdw;

   if (old_index > 0) {
      uint32_t *previous_ib = cmd->ib_ptrs[old_index - 1];
      uint64_t size_word = cmd->ib_cwds[old_index - 1] - 1;

      previous_ib[size_word] =
         (previous_ib[size_word] & C_3F3_IB_SIZE) |
         S_3F3_IB_SIZE(old_cdw);
   }
}
static void command_grow(zgfx_command *cmd) {
   if (cmd->ib_count >= cmd->ib_capacity)
   {
      uint64_t new_ib_capacity = cmd->ib_capacity * 2;
      void** new_ib_ptrs = calloc(new_ib_capacity, sizeof(void*));
      uint64_t* new_ib_sizes = calloc(new_ib_capacity, sizeof(uint64_t));
      uint64_t* new_ib_cwds = calloc(new_ib_capacity, sizeof(uint64_t));
      assert(new_ib_ptrs);
      assert(new_ib_sizes);
      assert(new_ib_cwds);
      memcpy(new_ib_ptrs, cmd->ib_ptrs, sizeof(void*) * cmd->ib_capacity);
      memcpy(new_ib_sizes, cmd->ib_sizes, sizeof(uint64_t) * cmd->ib_capacity);
      memcpy(new_ib_cwds, cmd->ib_cwds, sizeof(uint64_t) * cmd->ib_capacity);
      free(cmd->ib_ptrs);
      free(cmd->ib_sizes);
      free(cmd->ib_cwds);
      cmd->ib_capacity = new_ib_capacity;
      cmd->ib_ptrs = new_ib_ptrs;
      cmd->ib_cwds = new_ib_cwds;
      cmd->ib_sizes = new_ib_sizes;
   }
   
   uint64_t old_cdw = cmd->cs.cdw;
   uint64_t capacity_bytes = MIN(cmd->ib_sizes[cmd->ib_count-1] * 2, 1048576);
   cmd->cs = (struct ac_cmdbuf){
      .buf = galloc(cmd->dev, capacity_bytes),
      .max_dw = (capacity_bytes / sizeof(uint32_t)) - 4, // reserves last 4 dwords for chaining
      .reserved_dw = (capacity_bytes / sizeof(uint32_t)) - 4,
      .context_roll = cmd->cs.context_roll
   };

   cmd->ib_ptrs[cmd->ib_count] = cmd->cs.buf;
   cmd->ib_sizes[cmd->ib_count] = capacity_bytes;
   cmd->ib_cwds[cmd->ib_count] = 0;
   cmd->ib_count++;

   command_chain(cmd, old_cdw);
}

#define CP_DMA_SYNC (1 << 0)
#define CP_DMA_RAW_WAIT (1 << 1)
#define CP_DMA_USE_L2 (1 << 2)
#define CP_DMA_CLEAR (1 << 3)
#define SI_CPDMA_ALIGNMENT 32
static unsigned cp_dma_max_byte_count(enum amd_gfx_level gfx_level) {
   unsigned max = gfx_level >= GFX11 ? 32767 : gfx_level >= GFX9 ? S_506_BYTE_COUNT(~0u) : S_415_BYTE_COUNT(~0u);
   return max & ~(SI_CPDMA_ALIGNMENT - 1);
}
static void command_emit_cp_dma(zgfx_command *cmd, bool predicating, uint64_t dst_va,
                                uint64_t src_va, unsigned size, unsigned flags) {
   const struct radeon_info *info = &cmd->dev->info;
   const bool cp_dma_use_L2 = (flags & CP_DMA_USE_L2) && info->cp_dma_use_L2;
   const bool cp_dma_use_mall = info->gfx_level == GFX12;
   const bool cp_dma_tc_l2_flag = cp_dma_use_L2 || cp_dma_use_mall;
   uint32_t header = 0, command = 0;

   assert(size <= cp_dma_max_byte_count(info->gfx_level));

   unsigned packet_dw = info->gfx_level >= GFX7 ? 7 : 6;
   if (flags & CP_DMA_SYNC)
      packet_dw += 2;
   if (cmd->cs.cdw + packet_dw > cmd->cs.max_dw)
      command_grow(cmd);

   if (info->gfx_level >= GFX9)
      command |= S_506_BYTE_COUNT(size);
   else
      command |= S_415_BYTE_COUNT(size);

   if (flags & CP_DMA_SYNC)
      header |= S_501_CP_SYNC(1);

   if (flags & CP_DMA_RAW_WAIT)
      command |= S_506_RAW_WAIT(1);

   if (cp_dma_tc_l2_flag)
      header |= S_501_DST_SEL(V_501_DST_ADDR_USING_L2);

   if (flags & CP_DMA_CLEAR)
      header |= S_501_SRC_SEL(V_501_DATA);
   else if (cp_dma_tc_l2_flag)
      header |= S_501_SRC_SEL(V_501_SRC_ADDR_USING_L2);

   ac_cmdbuf_begin(&cmd->cs);
   if (info->gfx_level >= GFX7) {
      ac_cmdbuf_emit(PKT3(PKT3_DMA_DATA, 5, predicating));
      ac_cmdbuf_emit(header);
      ac_cmdbuf_emit(src_va);
      ac_cmdbuf_emit(src_va >> 32);
      ac_cmdbuf_emit(dst_va);
      ac_cmdbuf_emit(dst_va >> 32);
      ac_cmdbuf_emit(command);
   } else {
      assert(!cp_dma_tc_l2_flag);
      header |= S_412_SRC_ADDR_HI(src_va >> 32);
      ac_cmdbuf_emit(PKT3(PKT3_CP_DMA, 4, predicating));
      ac_cmdbuf_emit(src_va);
      ac_cmdbuf_emit(header);
      ac_cmdbuf_emit(dst_va);
      ac_cmdbuf_emit((dst_va >> 32) & 0xffff);
      ac_cmdbuf_emit(command);
   }
   ac_cmdbuf_end();

   if (flags & CP_DMA_SYNC)
      ac_emit_cp_pfp_sync_me(&cmd->cs, predicating);
}

PUBLIC void command_nop(zgfx_command *cmd) {
   if ((cmd->cs.cdw + 2) > cmd->cs.max_dw)
      command_grow(cmd);
   
   ac_emit_cp_nop(&cmd->cs, 0);
}
PUBLIC void command_clear(zgfx_command *cmd, void *backbuffer, uint32_t color, uint64_t size) {
   uint64_t remaining = size;
   if (size == 0) {
      uint64_t buffer_index = find_buffer(cmd->dev, backbuffer);
      assert(buffer_index != UINT64_MAX && "failed to find_buffer");
      remaining = cmd->dev->buffers_size[buffer_index];
   }
   uint64_t offset = 0;
   const uint64_t max_chunk = cp_dma_max_byte_count(cmd->dev->info.gfx_level);
   
   while (remaining > 0) {
      uint64_t chunk = remaining < max_chunk ? remaining : max_chunk;
      uint64_t dst_va = (uint64_t)backbuffer + offset;
      
      command_emit_cp_dma(cmd, false, dst_va, color, chunk, CP_DMA_CLEAR | CP_DMA_SYNC);
      
      offset += chunk;
      remaining -= chunk;
   }
}
PUBLIC void command_set_compute_shader(zgfx_command *cmd, zgfx_shader* shader) {
   assert(cmd && shader && cmd->dev == shader->dev);
   assert(shader->code && !shader->config.scratch_bytes_per_wave);

   const struct radeon_info *info = &cmd->dev->info;
   uint64_t va = (uint64_t)(uintptr_t)shader->code;
   assert((va & 255) == 0);

   unsigned threads = shader->workgroup_size[0] * shader->workgroup_size[1] * shader->workgroup_size[2];
   unsigned waves = DIV_ROUND_UP(threads, shader->wave_size);
   unsigned threadgroups_per_cu = info->gfx_level >= GFX10 && waves == 1 ? 2 : 1;

   struct ac_pm4_state pm4 = {0};
   ac_pm4_clear_state(&pm4, info, false, false);
   ac_pm4_set_reg(&pm4, R_00B830_COMPUTE_PGM_LO, va >> 8);
   ac_pm4_set_reg(&pm4, R_00B834_COMPUTE_PGM_HI, S_00B834_DATA(va >> 40));
   ac_pm4_set_reg(&pm4, R_00B848_COMPUTE_PGM_RSRC1, shader->config.rsrc1);
   ac_pm4_set_reg(&pm4, R_00B84C_COMPUTE_PGM_RSRC2, shader->config.rsrc2);
   if (info->gfx_level >= GFX10)
      ac_pm4_set_reg(&pm4, R_00B8A0_COMPUTE_PGM_RSRC3, shader->config.rsrc3);

   ac_pm4_set_reg(&pm4, R_00B854_COMPUTE_RESOURCE_LIMITS,
                  ac_get_compute_resource_limits(info, waves, 0, threadgroups_per_cu));
   if (info->gfx_level >= GFX12) {
      ac_pm4_set_reg(&pm4, R_00B81C_COMPUTE_NUM_THREAD_X,
                     S_00B81C_NUM_THREAD_FULL_GFX12(shader->workgroup_size[0]));
      ac_pm4_set_reg(&pm4, R_00B820_COMPUTE_NUM_THREAD_Y,
                     S_00B820_NUM_THREAD_FULL_GFX12(shader->workgroup_size[1]));
   } else {
      ac_pm4_set_reg(&pm4, R_00B81C_COMPUTE_NUM_THREAD_X,
                     S_00B81C_NUM_THREAD_FULL_GFX6(shader->workgroup_size[0]));
      ac_pm4_set_reg(&pm4, R_00B820_COMPUTE_NUM_THREAD_Y,
                     S_00B820_NUM_THREAD_FULL_GFX6(shader->workgroup_size[1]));
   }
   ac_pm4_set_reg(&pm4, R_00B824_COMPUTE_NUM_THREAD_Z,
                  S_00B824_NUM_THREAD_FULL(shader->workgroup_size[2]));

   ac_pm4_finalize(&pm4);
   if (cmd->cs.cdw + pm4.ndw > cmd->cs.max_dw)
      command_grow(cmd);
   ac_pm4_emit_commands(&cmd->cs, &pm4);
   cmd->compute_shader = shader;
}
PUBLIC void command_set_compute_shader_args(zgfx_command *cmd, zgfx_shader* shader, void* data) {
   assert(cmd && shader && cmd->dev == shader->dev);
   assert(shader->argument_ptr.used);

   unsigned offset = R_00B900_COMPUTE_USER_DATA_0 +
                     shader->args.args[shader->argument_ptr.arg_index].offset * 4;
   uint64_t va = (uint64_t)(uintptr_t)data;
   struct ac_pm4_state pm4 = {0};
   ac_pm4_clear_state(&pm4, &cmd->dev->info, false, false);
   ac_pm4_set_reg(&pm4, offset, va);
   ac_pm4_set_reg(&pm4, offset + 4, va >> 32);
   ac_pm4_finalize(&pm4);

   if (cmd->cs.cdw + pm4.ndw > cmd->cs.max_dw)
      command_grow(cmd);
   ac_pm4_emit_commands(&cmd->cs, &pm4);
}
PUBLIC void command_dispatch(zgfx_command *cmd, int x, int y, int z) {
   assert(cmd && cmd->compute_shader);
   assert(x >= 0 && y >= 0 && z >= 0);
   if (!x || !y || !z)
      return;

   const zgfx_shader *shader = cmd->compute_shader;
   const struct radeon_info *info = &cmd->dev->info;
   struct ac_pm4_state pm4 = {0};
   ac_pm4_clear_state(&pm4, info, false, false);
   if (shader->args.num_work_groups.used) {
      unsigned offset = R_00B900_COMPUTE_USER_DATA_0 +
                        shader->args.args[shader->args.num_work_groups.arg_index].offset * 4;
      ac_pm4_set_reg(&pm4, offset, x);
      ac_pm4_set_reg(&pm4, offset + 4, y);
      ac_pm4_set_reg(&pm4, offset + 8, z);
   }
   ac_pm4_finalize(&pm4);

   if (cmd->cs.cdw + pm4.ndw + 5 > cmd->cs.max_dw)
      command_grow(cmd);
   ac_pm4_emit_commands(&cmd->cs, &pm4);

   assert(shader->wave_size == 64 || (shader->wave_size == 32 && info->gfx_level >= GFX10));
   unsigned initiator = S_00B800_COMPUTE_SHADER_EN(1) | S_00B800_FORCE_START_AT_000(1) |
                        S_00B800_CS_W32_EN(shader->wave_size == 32);
   if (info->gfx_level >= GFX7 && (info->family < CHIP_GFX940 || info->has_graphics))
      initiator |= S_00B800_ORDER_MODE(1);
   if (info->gfx_level >= GFX10)
      initiator |= S_00B800_TUNNEL_ENABLE(1);

   ac_cmdbuf_begin(&cmd->cs);
   ac_cmdbuf_emit(PKT3(PKT3_DISPATCH_DIRECT, 3, 0) | PKT3_SHADER_TYPE_S(1));
   ac_cmdbuf_emit(x);
   ac_cmdbuf_emit(y);
   ac_cmdbuf_emit(z);
   ac_cmdbuf_emit(initiator);
   ac_cmdbuf_end();
}

#define QUEUE_SUBMIT_FENCE_TIMEOUT_NS (10ull * 1000 * 1000 * 1000)
PUBLIC void queue_submit(zgfx_device *dev, zgfx_command *cmd) {
   uint64_t last_index = cmd->ib_count - 1;
   uint64_t cdw = cmd->cs.cdw;
   uint64_t pad_mask = cmd->dev->info.ip[AMD_IP_GFX].ib_pad_dw_mask;
   uint64_t final_cdw = (MAX(cdw, 1) + pad_mask) & ~pad_mask;

   assert(final_cdw <= cmd->ib_sizes[last_index] / sizeof(uint32_t));
   assert(final_cdw <= G_3F3_IB_SIZE(UINT32_MAX));

   uint32_t nop = cmd->dev->info.gfx_ib_pad_with_type2 ? PKT2_NOP_PAD : PKT3_NOP_PAD;

   struct ac_cmdbuf last_cs = cmd->cs;
   last_cs.max_dw = final_cdw;
   last_cs.reserved_dw = final_cdw;

   while (last_cs.cdw + 2 <= final_cdw)
      ac_emit_cp_nop(&last_cs, 0);

   if (last_cs.cdw < final_cdw)
      last_cs.buf[last_cs.cdw++] = nop;

   cmd->ib_cwds[last_index] = final_cdw;

   if (last_index > 0) {
      uint32_t *previous_ib = cmd->ib_ptrs[last_index - 1];
      uint64_t size_word = cmd->ib_cwds[last_index - 1] - 1;

      previous_ib[size_word] =
         (previous_ib[size_word] & C_3F3_IB_SIZE) |
         S_3F3_IB_SIZE(final_cdw);
   }

   struct amdgpu_cs_ib_info ib_info = {0};
   ib_info.flags = 0;
   ib_info.ib_mc_address = (uint64_t)cmd->ib_ptrs[0];
   ib_info.size = cmd->ib_cwds[0];

   amdgpu_bo_list_handle bo_list_handle = 0;
   amdgpu_bo_list_create(dev->device_handle, dev->buffers_count, dev->buffers_bo_handle, NULL, &bo_list_handle);
   
   struct amdgpu_cs_request request = {0};
   request.flags = 0;
   request.ip_type = AMDGPU_HW_IP_GFX;
   request.ip_instance = 0;
   request.ring = 0;
   request.resources = bo_list_handle;
   request.number_of_dependencies = 0;
   request.dependencies = NULL;
   request.number_of_ibs = 1;
   request.ibs = &ib_info;
   request.seq_no = 0;
   request.fence_info.handle = NULL;
   int err = amdgpu_cs_submit(dev->context_handle, 0, &request, 1);
   assert(err == 0 && "failed to amdgpu_cs_submit");

   struct amdgpu_cs_fence fence = {0};
   fence.context = dev->context_handle;
   fence.ip_type = request.ip_type;
   fence.ip_instance = request.ip_instance;
   fence.ring = request.ring;
   fence.fence = request.seq_no;
   uint32_t expired = 0;
   err = amdgpu_cs_query_fence_status(&fence, QUEUE_SUBMIT_FENCE_TIMEOUT_NS, 0, &expired);
   assert(err == 0 && "failed to amdgpu_cs_query_fence_status");
   if (!expired) {
      fprintf(stderr, "queue_submit: IB did not complete within %llus -- CP may be stuck\n",
               QUEUE_SUBMIT_FENCE_TIMEOUT_NS / 1000000000ull);
   }
   assert(expired == 1 && "failed to completely execute IB");
   amdgpu_bo_list_destroy(bo_list_handle);
}
