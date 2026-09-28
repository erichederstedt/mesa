#include "zgfx.c"

#include "sid.h"

#define CHECK(condition) do { \
   if (!(condition)) { \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      abort(); \
   } \
} while (0)

struct amdgpu_bo {
   void *memory;
   uint64_t size;
};

int amdgpu_bo_alloc(amdgpu_device_handle dev,
                    struct amdgpu_bo_alloc_request *request,
                    amdgpu_bo_handle *handle)
{
   (void)dev;
   CHECK(request->preferred_heap == AMDGPU_GEM_DOMAIN_GTT);
   struct amdgpu_bo *bo = calloc(1, sizeof(*bo));
   CHECK(bo);
   size_t alignment = request->phys_alignment;
   if (alignment < 4096)
      alignment = 4096;
   CHECK(posix_memalign(&bo->memory, alignment, request->alloc_size) == 0);
   bo->size = request->alloc_size;
   memset(bo->memory, 0xcd, bo->size);
   *handle = bo;
   return 0;
}

int amdgpu_bo_cpu_map(amdgpu_bo_handle bo, void **cpu)
{
   *cpu = bo->memory;
   return 0;
}

int amdgpu_va_range_query(amdgpu_device_handle dev,
                          enum amdgpu_gpu_va_range type,
                          uint64_t *start, uint64_t *end)
{
   (void)dev;
   CHECK(type == amdgpu_gpu_va_range_general);
   *start = 0;
   *end = UINT64_MAX;
   return 0;
}

int amdgpu_va_range_alloc(amdgpu_device_handle dev,
                          enum amdgpu_gpu_va_range type,
                          uint64_t size, uint64_t alignment,
                          uint64_t required, uint64_t *allocated,
                          amdgpu_va_handle *handle, uint64_t flags)
{
   (void)dev;
   (void)size;
   (void)alignment;
   (void)flags;
   CHECK(type == amdgpu_gpu_va_range_general);
   CHECK(required);
   *allocated = required;
   *handle = (amdgpu_va_handle)(uintptr_t)required;
   return 0;
}

int amdgpu_bo_va_op_raw(amdgpu_device_handle dev, amdgpu_bo_handle bo,
                        uint64_t offset, uint64_t size, uint64_t address,
                        uint64_t flags, uint32_t operation)
{
   (void)dev;
   CHECK(offset == 0);
   CHECK(size <= bo->size);
   CHECK(address == (uintptr_t)bo->memory);
   CHECK(flags & AMDGPU_VM_PAGE_EXECUTABLE);
   CHECK(operation == AMDGPU_VA_OP_MAP);
   return 0;
}

static void init_device(zgfx_device *dev, bool type2, uint32_t alignment,
                        uint32_t pad_mask)
{
   memset(dev, 0, sizeof(*dev));
   dev->info.gfx_level = type2 ? GFX6 : GFX9;
   dev->info.gfx_ib_pad_with_type2 = type2;
   dev->info.ip[AMD_IP_GFX].ib_alignment = alignment;
   dev->info.ip[AMD_IP_GFX].ib_pad_dw_mask = pad_mask;
   dev->info.ip[AMD_IP_GFX].num_queues = 1;
   dev->info.has_graphics = true;
}

static uint32_t single_nop(const zgfx_device *dev)
{
   return dev->info.gfx_ib_pad_with_type2 ? PKT2_NOP_PAD : PKT3_NOP_PAD;
}

static void record_nops(zgfx_command *cmd, uint32_t count)
{
   CHECK(cmd->cs.cdw == 0);
   CHECK(count <= cmd->cs.max_dw);
   uint32_t i = 0;
   for (; i + 1 < count; i += 2) {
      cmd->cs.buf[i] = PKT3(PKT3_NOP, 0, 0);
      cmd->cs.buf[i + 1] = 0xa5100000u | i;
   }
   if (i < count)
      cmd->cs.buf[i] = single_nop(cmd->dev);
   cmd->cs.cdw = count;
}

static void check_recorded(const zgfx_device *dev, const uint32_t *words,
                            uint32_t count)
{
   uint32_t i = 0;
   for (; i + 1 < count; i += 2) {
      CHECK(words[i] == PKT3(PKT3_NOP, 0, 0));
      CHECK(words[i + 1] == (0xa5100000u | i));
   }
   if (i < count)
      CHECK(words[i] == single_nop(dev));
}

static void check_padding(const zgfx_device *dev, const uint32_t *words,
                          uint32_t begin, uint32_t end)
{
   uint32_t i = begin;
   while (i < end) {
      uint32_t header = words[i];
      if (PKT_TYPE_G(header) == 2) {
         CHECK(header == PKT2_NOP_PAD);
         ++i;
      } else {
         CHECK(PKT_TYPE_G(header) == 3);
         CHECK(PKT3_IT_OPCODE_G(header) == PKT3_NOP);
         uint32_t count = PKT_COUNT_G(header);
         if (count == 0x3fff) {
            CHECK(!dev->info.gfx_ib_pad_with_type2);
            ++i;
         } else {
            CHECK(count + 2 <= end - i);
            i += count + 2;
         }
      }
   }
   CHECK(i == end);
}

static void check_stream(zgfx_command *cmd, const uint32_t *recorded)
{
   const uint32_t pad_mask = cmd->dev->info.ip[AMD_IP_GFX].ib_pad_dw_mask;
   const uint32_t alignment = cmd->dev->info.ip[AMD_IP_GFX].ib_alignment;
   CHECK(cmd->finalized);
   CHECK(cmd->ib_count > 0);
   CHECK(cmd->ib_count <= cmd->ib_capacity);
   CHECK(cmd->cs.buf == cmd->ibs[cmd->ib_count - 1].ptr);
   CHECK(cmd->cs.cdw == cmd->ibs[cmd->ib_count - 1].size_dw);

   for (uint32_t i = 0; i < cmd->ib_count; ++i) {
      const struct zgfx_ib *ib = &cmd->ibs[i];
      CHECK(ib->size_dw > 0);
      CHECK((ib->size_dw & pad_mask) == 0);
      CHECK(ib->size_dw <= G_3F3_IB_SIZE(UINT32_MAX));
      CHECK(ib->size_dw * sizeof(uint32_t) <= ib->capacity_bytes);
      CHECK(ib->capacity_bytes <= (uint64_t)G_3F3_IB_SIZE(UINT32_MAX) * sizeof(uint32_t));
      CHECK(ib->capacity_bytes % alignment == 0);
      CHECK((uintptr_t)ib->ptr % alignment == 0);
      CHECK(recorded[i] <= ib->size_dw);
      check_recorded(cmd->dev, ib->ptr, recorded[i]);

      if (i + 1 < cmd->ib_count) {
         CHECK(ib->size_dw >= recorded[i] + 4);
         const uint32_t *chain = ib->ptr + ib->size_dw - 4;
         CHECK(chain[0] == PKT3(PKT3_INDIRECT_BUFFER, 2, 0));
         uint64_t address = chain[1] | ((uint64_t)chain[2] << 32);
         CHECK(address == (uintptr_t)cmd->ibs[i + 1].ptr);
         CHECK(G_3F3_CHAIN(chain[3]) == 1);
         CHECK(G_3F3_VALID(chain[3]) == 1);
         CHECK(G_3F3_IB_SIZE(chain[3]) == cmd->ibs[i + 1].size_dw);
         CHECK((chain[3] & C_3F3_IB_SIZE) == (S_3F3_CHAIN(1) | S_3F3_VALID(1)));
         check_padding(cmd->dev, ib->ptr, recorded[i], ib->size_dw - 4);
      } else {
         check_padding(cmd->dev, ib->ptr, recorded[i], ib->size_dw);
      }

      const struct amdgpu_bo *bo = cmd->dev->buffers_bo_handle[i];
      const unsigned char *end = (const unsigned char *)ib->ptr + ib->capacity_bytes;
      CHECK(end + 64 <= (const unsigned char *)bo->memory + bo->size);
      for (unsigned j = 0; j < 64; ++j)
         CHECK(end[j] == 0xcd);
   }
}

static void free_command(zgfx_command *cmd)
{
   zgfx_device *dev = cmd->dev;
   for (uint64_t i = 0; i < dev->buffers_count; ++i) {
      struct amdgpu_bo *bo = dev->buffers_bo_handle[i];
      free(bo->memory);
      free(bo);
   }
   free(cmd->ibs);
   free(cmd);
}

static void test_padding(bool type2, uint32_t alignment, uint32_t pad_mask)
{
   for (uint32_t sample = 0; sample < 11; ++sample) {
      zgfx_device dev;
      init_device(&dev, type2, alignment, pad_mask);
      zgfx_command *cmd = command_begin(&dev);
      CHECK(cmd);
      CHECK(cmd->dev == &dev);
      CHECK(cmd->ib_count == 1);
      CHECK(cmd->cs.cdw == 0);
      CHECK(cmd->cs.max_dw + 4 == cmd->ibs[0].capacity_bytes / sizeof(uint32_t));
      CHECK(cmd->cs.reserved_dw >= cmd->cs.max_dw);
      uint32_t recorded[3] = {sample, 0, 0};
      if (sample == 9)
         recorded[0] = cmd->cs.max_dw - 1;
      if (sample == 10)
         recorded[0] = cmd->cs.max_dw;
      cmd->cs.context_roll = true;
      record_nops(cmd, recorded[0]);
      CHECK(command_grow(cmd));
      CHECK(cmd->cs.context_roll);
      CHECK(cmd->ib_count == 2);
      recorded[1] = sample % 3;
      record_nops(cmd, recorded[1]);
      CHECK(command_grow(cmd));
      CHECK(cmd->cs.context_roll);
      recorded[2] = sample % 5;
      record_nops(cmd, recorded[2]);
      command_finalize(cmd);
      check_stream(cmd, recorded);
      uint32_t last_dw = cmd->cs.cdw;
      uint32_t first_dw = cmd->ibs[0].size_dw;
      uint64_t allocations = dev.buffers_count;
      command_finalize(cmd);
      CHECK(cmd->cs.cdw == last_dw);
      CHECK(cmd->ibs[0].size_dw == first_dw);
      CHECK(!command_grow(cmd));
      CHECK(dev.buffers_count == allocations);
      check_stream(cmd, recorded);
      free_command(cmd);
   }
}

static void test_many_chunks(bool type2)
{
   zgfx_device dev;
   init_device(&dev, type2, 256, 7);
   zgfx_command *cmd = command_begin(&dev);
   CHECK(cmd);
   uint32_t recorded[14];
   for (uint32_t i = 0; i < 14; ++i) {
      recorded[i] = i + 1 == 14 ? cmd->cs.max_dw : i % 8;
      record_nops(cmd, recorded[i]);
      if (i + 1 < 14)
         CHECK(command_grow(cmd));
   }
   CHECK(cmd->ib_count == 14);
   CHECK(cmd->ib_capacity >= 14);
   CHECK(cmd->ibs[12].capacity_bytes == cmd->ibs[13].capacity_bytes);
   command_finalize(cmd);
   check_stream(cmd, recorded);
   free_command(cmd);
}

static void test_single_chunk(bool type2)
{
   zgfx_device dev;
   init_device(&dev, type2, 256, 7);
   zgfx_command *cmd = command_begin(&dev);
   CHECK(cmd);
   const uint32_t recorded[1] = {0};
   command_finalize(cmd);
   check_stream(cmd, recorded);
   command_finalize(cmd);
   check_stream(cmd, recorded);
   free_command(cmd);
}

static void test_rejected_operations(void)
{
   CHECK(command_begin(NULL) == NULL);

   zgfx_device dev;
   init_device(&dev, false, 256, 7);
   dev.info.has_graphics = false;
   CHECK(command_begin(&dev) == NULL);
   CHECK(dev.buffers_count == 0);

   dev.info.has_graphics = true;
   dev.buffers_count = MAX_BUFFER_COUNT;
   CHECK(command_begin(&dev) == NULL);
   dev.buffers_count = 0;

   dev.info.ip[AMD_IP_GFX].ib_alignment = 384;
   CHECK(command_begin(&dev) == NULL);
   CHECK(dev.buffers_count == 0);

   dev.info.ip[AMD_IP_GFX].ib_alignment = 1u << 22;
   CHECK(command_begin(&dev) == NULL);
   CHECK(dev.buffers_count == 0);

   dev.info.ip[AMD_IP_GFX].ib_alignment = 256;
   zgfx_command *cmd = command_begin(&dev);
   CHECK(cmd);
   const uint32_t recorded[2] = {3, 5};
   record_nops(cmd, recorded[0]);
   CHECK(command_grow(cmd));
   record_nops(cmd, recorded[1]);
   uint64_t allocations = dev.buffers_count;
   uint32_t *ptr = cmd->cs.buf;
   uint32_t first_dw = cmd->ibs[0].size_dw;
   uint32_t *size_ptr = cmd->ib_size_ptr;
   uint32_t chain[4];
   memcpy(chain, cmd->ibs[0].ptr + first_dw - 4, sizeof(chain));
   dev.buffers_count = MAX_BUFFER_COUNT;
   CHECK(!command_grow(cmd));
   CHECK(cmd->ib_count == 2);
   CHECK(cmd->cs.buf == ptr);
   CHECK(cmd->cs.cdw == recorded[1]);
   CHECK(cmd->ibs[0].size_dw == first_dw);
   CHECK(cmd->ibs[1].size_dw == 0);
   CHECK(cmd->ib_size_ptr == size_ptr);
   CHECK(memcmp(chain, cmd->ibs[0].ptr + first_dw - 4, sizeof(chain)) == 0);
   CHECK(!cmd->finalized);
   check_recorded(&dev, ptr, recorded[1]);
   dev.buffers_count = allocations;
   command_finalize(cmd);
   check_stream(cmd, recorded);
   free_command(cmd);
}

int main(void)
{
   for (unsigned type2 = 0; type2 < 2; ++type2) {
      test_single_chunk(type2);
      test_padding(type2, 256, 7);
      test_padding(type2, 8192, 63);
      test_many_chunks(type2);
   }
   test_rejected_operations();
   return 0;
}
