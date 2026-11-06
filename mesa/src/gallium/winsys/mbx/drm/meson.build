# SPDX-License-Identifier: MIT

mbx_winsys_c_args = []
if with_gallium_kmsro
  mbx_winsys_c_args += '-DGALLIUM_KMSRO'
endif

libmbxwinsys = static_library(
  'mbxwinsys',
  files('mbx_drm_winsys.c'),
  dependencies : idep_mesautil,
  include_directories : [
    inc_src, inc_include,
    inc_gallium, inc_gallium_aux, inc_gallium_drivers, inc_gallium_winsys,
  ],
  c_args : [mbx_winsys_c_args],
  gnu_symbol_visibility : 'hidden',
)
