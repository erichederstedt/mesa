libzgfx = shared_library(
  'zgfx',
  'zgfx.c',
  'zgfx_shader.c',
  include_directories : [inc_include, inc_src, inc_amd, inc_amd_common],
  link_with : [libamd_common, libamd_common_llvm, libamdgpu_addrlib],
  dependencies : [
    dep_libdrm, dep_libdrm_amdgpu, idep_mesautil, idep_amd_generated_headers,
    dep_llvm, dep_m, idep_aco, idep_nir, idep_vtn,
  ],
  gnu_symbol_visibility : 'hidden',
  install : true,
)

install_headers('zgfx.h')

pkg.generate(
  libzgfx,
  name : 'zero_graphics',
  filebase : 'zgfx',
  description : 'zero_graphics userspace graphics library',
  version : '0.1.0',
)

executable(
  'zgfx_demo',
  'demo.c',
  link_with : libzgfx,
  dependencies : dependency('xcb'),
  install : false,
)
