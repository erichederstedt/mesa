#ifndef ZGFX_PRIVATE_H
#define ZGFX_PRIVATE_H

#include "zgfx.h"

#include <amdgpu.h>

#include "ac_cmdbuf.h"
#include "ac_gpu_info.h"

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
static uint64_t find_buffer(struct zgfx_device *dev, void *ptr) {
    for (uint64_t i = 0; i < dev->buffers_count; i++) {
        if (dev->buffers_ptr[i] == ptr)
            return i;
    }
    return UINT64_MAX;
}

#endif
