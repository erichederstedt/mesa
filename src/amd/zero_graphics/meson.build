libzgfx = shared_library(
  'zgfx',
  'zgfx.c',
  'zgfx_shader.c',
  'zgfx_wsi_x11.c',
  include_directories : [inc_include, inc_src, inc_amd, inc_amd_common],
  link_with : [libamd_common, libamd_common_llvm, libamdgpu_addrlib],
  dependencies : [
    dep_libdrm, dep_libdrm_amdgpu, idep_mesautil, idep_amd_generated_headers,
    dep_llvm, dep_m, idep_aco, idep_nir, idep_vtn,
    dependency('xcb'), dependency('xcb-dri3'), dependency('xcb-present'),
  ],
  gnu_symbol_visibility : 'hidden',
  install : true,
)

install_headers('zgfx.h', 'zgfx_wsi_x11.h')

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
  dependencies : [dep_libdrm, dependency('xcb'), dependency('xcb-dri3'), dependency('xcb-present'), dependency('x11'), dependency('x11-xcb')],
  install : false,
)

zgfx_glfw_dep = dependency('glfw3', version : '>=3.4', required : false)
zgfx_xlib_xcb_dep = dependency('x11-xcb', required : false)
if zgfx_glfw_dep.found() and zgfx_xlib_xcb_dep.found()
  executable(
    'zgfx_demo_glfw',
    'demo_glfw.c',
    link_with : libzgfx,
    dependencies : [zgfx_glfw_dep, zgfx_xlib_xcb_dep],
    install : false,
  )
endif
