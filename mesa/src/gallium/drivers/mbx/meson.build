# SPDX-License-Identifier: MIT

files_libmbx = files(
  'mbx_bo.c',
  'mbx_bo.h',
  'mbx_compile.c',
  'mbx_context.c',
  'mbx_context.h',
  'mbx_cpuvs.c',
  'mbx_vgp_exec.c',
  'mbx_draw.c',
  'mbx_fallback.c',
  'mbx_fallback.h',
  'mbx_job.c',
  'mbx_nir.c',
  'mbx_program.c',
  'mbx_program.h',
  'mbx_resource.c',
  'mbx_resource.h',
  'mbx_screen.c',
  'mbx_screen.h',
  'mbx_state.c',
  'mbx_stencil.c',
)

libmbx = static_library(
  'mbx',
  files_libmbx,
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  gnu_symbol_visibility : 'hidden',
  dependencies : [dep_libdrm, idep_nir_headers, idep_mesautil],
  build_by_default : false,
)

driver_mbx = declare_dependency(
  compile_args : '-DGALLIUM_MBX',
  link_with : [libmbx, libmbxwinsys],
  dependencies : [idep_nir, driver_softpipe],
)

if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif
