# TODO

Open work, newest first. Each item says what is known and what the next step is. Facts cite JOURNEY.md and mbx/README.md assumption numbers.

## Display updates: replace the idle poll with a power-friendly design

**Where it stands.** A manual (RFBI) display update stalls when the CPU idles between arming RFBI on the tearing-effect trigger and the
FRAMEDONE interrupt: about a third of the updates took 3 ms plus a multiple of 10 ms, up to the 250 ms timeout, with every software stage
at about 100 microseconds (a ring-buffer trace; printk hides it). Kernel patch 0066 (`omapdrm.idle_poll`, default on) makes the idle loop
poll from arming to FRAMEDONE: a small partial update then takes 23-25 ms (40 fps) instead of 96 ms, a whole frame 75 ms instead of
205-225 ms. It works but burns CPU for the whole update, including the wait for the tearing-effect pulse (up to a refresh period).
Not yet measured: what that costs in CPU busy time and power.

**What is known about the cause.**
- Not full-chip retention (`sleep_while_idle=0` changes nothing): the plain idle WFI is enough to stall it.
- On the unit (PRCM at 0x48008000, readable with `devmem`): `PM_WKEN1_CORE` (0x480082a0) = 0x04667FF8 has bit 0, the display wake event,
  clear; `PM_WKDEP_MPU` (0x480081c8) = 0x14 has EN_CORE clear. Setting either alone changed nothing; both together helped (65 ms a
  frame, 10 of 40 flips slow instead of 15-17) but did not fix it. A missing wake path is part of the cause, not all of it.
- The FRAMEDONE interrupt does arrive (trace); the open question is whether the transfer itself waits or only the delivery.

**Design (proper and power saving).**
1. Hold the dependency, not the CPU: keep the clock domain and wake path the transfer needs awake while an update is in flight and
   let the CPU WFI. Find the missing piece by diffing `CM_CLKSTCTRL_*` and the idle status registers during a stalled update and a
   healthy one; then use `clkdm` wake/hold calls over the update window (rfbi arm to FRAMEDONE or timeout).
2. Fallback if no hold works: an hrtimer-bounded poll that covers only the transfer, not the tearing-effect wait (the TE time is
   predictable from the panel timing: wake just before it, poll through the transfer).
3. Send less, less often: partial renders and the damage report (done), render and flip nothing when nothing changed, coalesce a
   stale frame behind one in flight, start small rectangles right behind the scan (Nokia's `enable_tearsync()`), half-size mode.
4. One wakeup per update: the 250 ms timeout stays a one-shot cancelled by the interrupt; queue flip N+1 before the event for N
   (userspace pipelining) so the interrupt releases the next update directly (`gbm_damage -P` to measure it, not written yet).
5. Let full-chip retention work between updates (nothing in flight, `sleep_while_idle`).

**Next steps, in order.**
1. Quantify the stopgap: CPU busy time from `/proc/stat` during `gbm_damage`, with and without `omapdrm.idle_poll`.
2. Diff the clock-domain registers (stalled vs healthy update) to name the missing dependency.
3. Replace the poll with the hold if it works; otherwise the hrtimer-bounded poll.
4. Re-run `gbm_damage -D 0/1 -n 40 -l` for the flip distribution (target: no flip over 100 ms, CPU asleep during the wait).

Tools: `mesa/tests/gbm_damage.c` (`-D 0|1`, `-z size`, `-l` per-flip log, `-v` checks the last frame), `kernel/debug/omapdrm-trace-updates.patch`
(ring trace, `omapdrm.trace_updates` 1 records, 2 prints; apply on top of the series; build incrementally, do not re-run
`prepare-6.18.sh`), `devmem` on the unit.

## Weston

- Pipeline (client to screen), measured 2026-11-07: the panel runs at 40 Hz by default (user), so a small update costs one 25 ms period and
  a client can reach 40 fps; the DRM mode still says 51 Hz, so Weston expected a refresh every 19.6 ms and started its repaints too late
  (`repaint-window=16` in the image's `weston.ini` fixes the timing: 26.6 -> 40 fps for a windowed client). A cleaner fix is for the
  mode to report the real rate (the panel timings the omapdrm mode is made from), so Weston's defaults, the clients' frame callbacks and
  the presentation timestamps agree. Fullscreen is still 20 fps (a client that redraws everything: 8 ms of chip time for 1500 tiles, and
  Weston's 416-tile textured draw 6.7 ms, each job with 1-3 ms of kernel CPU on the one core); `MBX_PROF=1` (per process) and the module
  parameter `mbx.prof=1` (per phase, in dmesg) measure it.

Runs on the unit with the GL renderer, every draw on the chip, shm and dmabuf clients both verified by screenshot (JOURNEY.md,
2026-11-07, "Weston on the unit" and "A Wayland client in Weston"; `docs/weston-simple-egl.png`). Touch input works (user, by hand on the unit, with the calibration rule). Weston starts at boot (`S90weston`, `--idle-time=0`; `touch /etc/noweston` skips it). `weston-terminal` next to `weston-simple-egl` fits easily: 41 MB used before, 45 MB with the terminal (47 MB free of 123; measured, not yet looked at by eye). Open: other clients, the frame rate of Weston
itself against the display update, the cost of the red-blue swapped texture copy (write-combined reads; track the dirty box instead
of the whole texture), the colour of the unused byte when a client renders fragments with alpha below 1 into an XRGB buffer, a
the debug code's per-draw `getenv` calls, and a feature test for a
dma-buf export and re-import in one process (the shared-handle bug had no test).

- The client's `DRM_IOCTL_MODE_CREATE_DUMB failed: Permission denied` (Mesa's `renderonly.c`, so our scanout path): the wayland EGL
  platform asks for scanout-capable buffers, the client has no right to create dumb buffers on the KMS device, and the buffer then
  arrives anyway (dmabuf, confirmed in Weston's log), which suggests the state tracker retries without the scanout bind. Not
  verified, harmless so far; a quiet fix is to skip the renderonly path in a process that is not the display owner.
- `weston-simple-egl -f` at about 20 fps is consistent with the display update (a whole-frame manual update took 75 ms in `gbm_damage`),
  not the render; unmeasured for Weston itself.

## Other open items

- `gles2_feat point_sizes` failed once in a full host-sim run ("(16,32) is 0 0 0") and passed in 3 solo runs and the next full run: an
  intermittent failure, not investigated (per-point objects; a race in the sim shim?).
- Fast stencil, stage 2: the stencil WRITES on the chip (README assumption 53 has the tests): render the draw's coverage into a mask target on the chip
  with a white fragment shader, merge it into the CPU stencil plane (REPLACE, ZERO; INCR/DECR/INVERT need counts); the clip write on the CPU is
  most of the remaining time of `gles2_fps stencil`. Also batching consecutive draws under one stencil state (each is its own job now).
- The per-point size word inside one list draw (untested; one object per point today).
- PVRTC textures, the 2D core for blits and `glGenerateMipmap`, constant-colour blend (no factor code on the chip).
- A frame-diff fallback (`mbx_frame_damage`) for apps that give no damage.
- A real dma-fence on the imported scanout buffer so omapdrm waits on the MBX job instead of Mesa blocking (scheduling and CPU use only).
- Frames rendered and not flipped rely on omapdrm's running damage union (patch 0065); half-size mode with renderer damage is untried.
- The kernel partition has about 13 KB free (the loader kernel is 2 MB): the next kernel patch may need something trimmed.
