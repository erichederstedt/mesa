#include "zgfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <amdgpu.h>
#include <unistd.h>

#include "ac_gpu_info.h"
#include "ac_linux_drm.h"
#include "ac_cmdbuf.h"
#include "ac_cmdbuf_cp.h"
#include "amd_family.h"
#include "util/macros.h"

PUBLIC void zgfx_hello_world(void)
{
   puts("Hello, world!");
}

#define MAX_BUFFER_COUNT 128
struct zgfx_device {
   int fd;
   amdgpu_device_handle device_handle;
   amdgpu_context_handle context_handle;
   struct radeon_info info;
   
   void *buffers_raw_ptr[MAX_BUFFER_COUNT]; // SAO
   void *buffers_ptr[MAX_BUFFER_COUNT];
   uint64_t buffers_size[MAX_BUFFER_COUNT];
   uint64_t buffers_align[MAX_BUFFER_COUNT];
   amdgpu_bo_handle buffers_bo_handle[MAX_BUFFER_COUNT];
   amdgpu_va_handle buffers_va_handle[MAX_BUFFER_COUNT];
   uint64_t buffers_count;
};
struct zgfx_command {
   struct ac_cmdbuf cs;
   void** ib_ptrs;
   uint64_t* ib_sizes;
   uint64_t* ib_cwds;
   uint64_t ib_count;
   uint64_t ib_capacity;
   zgfx_device* dev;
};

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
PUBLIC void command_nop(zgfx_command *cmd) {
   ac_emit_cp_nop(&cmd->cs, 0);
}
PUBLIC void command_clear(zgfx_command *cmd, void *backbuffer, uint32_t color, uint64_t size);

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
