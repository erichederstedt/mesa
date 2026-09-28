| Need | Existing shared code |
|---|---|
| Identify GPU generation, capabilities, and hardware quirks | [`ac_gpu_info.h`](/home/dolf/dev/mesa/src/amd/common/ac_gpu_info.h): `radeon_info` `ac_query_gpu_info()` |
| Initial graphics and compute register setup | [`ac_cmdbuf.h`](/home/dolf/dev/mesa/src/amd/common/ac_cmdbuf.h:693): `ac_init_graphics_preamble_state()`, `ac_init_compute_preamble_state()` |
| Command-stream representation and register emission | [`ac_cmdbuf_base.h`](/home/dolf/dev/mesa/src/amd/common/ac_cmdbuf_base.h), [`ac_pm4.h`](/home/dolf/dev/mesa/src/amd/common/ac_pm4.h) |
| Cache flushes and hardware synchronization | [`ac_barrier.h`](/home/dolf/dev/mesa/src/amd/common/ac_barrier.h): `ac_emit_barrier()` |
| CP write/wait packets, scratch and ring setup | [`ac_cmdbuf_cp.h`](/home/dolf/dev/mesa/src/amd/common/ac_cmdbuf_cp.h) |
| Image layouts and descriptors | [`ac_surface.h`](/home/dolf/dev/mesa/src/amd/common/ac_surface.h), [`ac_descriptors.h`](/home/dolf/dev/mesa/src/amd/common/ac_descriptors.h) |