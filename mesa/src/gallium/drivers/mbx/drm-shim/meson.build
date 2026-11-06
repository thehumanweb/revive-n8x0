# SPDX-License-Identifier: MIT
# The sim: a drm-shim backend running the emulator core (copied in as emu/ by
# mesa/scripts/prepare-mesa.sh) behind the render node's ioctls, for the host.

libmbx_sim_drm_shim = shared_library(
  'mbx_sim_drm_shim',
  [
    'mbx_sim.c',
    'emu/core.c', 'emu/ta.c', 'emu/vgp.c', 'emu/render.c', 'emu/rcptab.c',
    'emu/texture.c', 'emu/twod.c', 'emu/pb.c',
  ],
  include_directories: [inc_include, inc_src],
  dependencies: [dep_drm_shim, cc.find_library('m')],
  gnu_symbol_visibility : 'hidden',
  install : true,
)
