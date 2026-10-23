# RSX Diagnostics 1.5

A standalone PS3 app (title RSXD00001) for checking the RSX graphics chip, its memory and the
video outputs. It grew out of the PPSSPP PS3 port's hardware-renderer tests. Useful when a console
shows artifacts, freezes or "GLOD" symptoms, or won't set a resolution.

## Menu
- **Run all tests** (about 2 minutes): video report, self-test, VRAM test, 60 s stress test.
  Combined report: `rsx_diag_report.txt`.
- **Video output report**: connected port (HDMI, component, composite, SCART, D-sub...), port
  state (a port stuck "BUSY" is a bad sign), colour space, display latency, RGB range, the modes
  the TV reports (EDID) and its chromaticity data, HDCP receiver count, which resolutions
  (480/576/720/1080, the 1080 widths, 3D) the system will accept per aspect ratio, the current
  configuration, the measured vblank (refresh) rate and temperatures. `video_report.txt`.
- **RSX self-test**: each kind of RSX command on its own, then rendering checks with pixel
  readback (clears, coordinates, vertex colours, texturing, blending, depth, alpha test, stencil,
  shader combiners, image scaling) and a triangle throughput test. `rsx_selftest.txt`.
- **VRAM test**: CPU and DMA bandwidth, a 32 MB pattern test, a fill-rate test, then a sweep of
  every free byte of RSX memory: CPU-written address patterns and their inverse, RSX-written
  fills, all read back by DMA. Bad words are reported with offset (in MB) and flipped bits.
  Errors only in the RSX-written passes point at the RSX / memory interface, errors in all
  passes at the memory chips. `rsx_vramtest.txt`.
- **RSX stress test** (60 s or 10 min): keeps the RSX at full load (a 1280x720 target covered
  many times per frame by a ~200-instruction shader, blended and depth-tested, two frames always
  queued) and once a second renders a fixed check scene that must be bit-identical to the first.
  Mismatches or hangs that appear once the chip is warm are typical of a failing RSX. The report
  gives frames/s, megapixels/s and the start, highest and final temperatures. `rsx_stresstest.txt`.
- **Screen test patterns**: colour bars, ramps, 1-pixel checkerboard, grid, solid colours - to
  look for lines, blocks or colour errors on the TV.

Reports and `diag.log` are saved to `/dev_hdd0/game/RSXD00001/USRDIR/`; the log is also copied to
the first USB drive (`RSX_DIAG.log`) if one is plugged in.

## New in 1.5
- The stress test now really loads the RSX: full-screen layers of a heavy shader on a 1280x720
  target, sized so each frame takes the chip ~25-50 ms, with two frames always queued. 1.4 drew
  a tiny 64x64 scene and waited for every frame, so the chip was mostly idle and cooled down.
- The stress report lists the RSX and Cell temperature at the start, the highest seen and at the end.

## New in 1.4
- The stress test (and every other test) now starts from a clean drawing state: the shader
  programs are re-uploaded and cached bindings dropped first. 1.3 could report "Scene actually
  draws ... 0 pixels" when the stress test ran after other tests.
- If the stress scene still draws nothing, the report says whether a plain single rectangle
  draws, to tell a state problem from a hardware one.
- The RSX line on the main screen is shorter (no memory addresses), so it fits.

## New in 1.3
- Reports are titled "RSX Diagnostics" (no more PPSSPP), and the video report only covers the
  console's own HDMI / AV multi output (the remote play output is left out).

## New in 1.2
- The screen no longer uses SDL at all. 1.1 still froze on the test patterns and during tests:
  the progress screen and patterns went through SDL's PS3 renderer, which hung the console. The
  app now draws its screens with the CPU straight into its own display buffers and only asks the
  RSX to flip, so the progress bar, the patterns and the PS button menu don't depend on the
  graphics engine that is being tested.
- Test patterns are drawn at the display's native resolution (a real 1-pixel checkerboard).

## New in 1.1
- Tests show a progress screen (step, percentage, elapsed time, live temperatures in the stress
  test) and can be cancelled with Circle.
- Fixed: test patterns froze the console. The RSX's scaled-copy engine hangs on sources wider or
  taller than 1024 pixels; patterns now use a 1024x576 image, and any larger copy is split.
- Self-test: the "render target in main memory" check now runs last, followed by a draw check,
  to find out whether it is what broke the drawing checks after it on some consoles.
- Stress test: fails if the scene doesn't actually draw (a blank frame also "matches" itself).

Temperatures use lv2 syscall 383, which some firmwares do not allow; the report then says so.
A failed test is not proof of a hardware fault on its own - run it again, and compare with the
console cold and warm.
