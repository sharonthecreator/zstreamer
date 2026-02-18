# zstreamer Project Memory

## Design Rules

- **No DTS duplication**: Properties configurable through a normal device's DTS (e.g., SPI frequency, UART baud rate) must NOT be duplicated in the zstnode's device DTS. The zstnode should reference the device and use its existing config.
- **Source/non-source API contract is strict**: First `zstreamer_node_start()` on a source should return `0`, second `-EALREADY`; first `zstreamer_node_stop()` should return `0`, second `-EALREADY`. Do not weaken tests to accept both codes for the same call sequence.
- **Do not depend on `struct net_buf` internals**: avoid direct checks on fields like `buf->ref`; use public net_buf APIs and explicit state in zstreamer where needed.
- **Keep DTS vendor registration present**: module-local compatibles using `zstreamer,*` require `dts/bindings/vendor-prefixes.txt` with a `zstreamer` entry to avoid dtc warnings/errors in stricter paths.

## Environment

- Zephyr source tree: `~/Projects/zephyrcreator/zephyr`
- Docker container `amazing_wescoff` (zephyrprojectrtos/zephyr-build) for native_sim builds (macOS can't build POSIX arch)
- Docker workspace: `/workdir/zstreamer` (bind-mounted)
- On aarch64 Docker, use `native_sim/native/64` — 32-bit `native_sim` fails
- Samples/tests need `native_sim_native_64.overlay` (symlinks to `native_sim.overlay` work)
- Twister in Docker uses `/usr/bin/dtc` which errors on vendor prefixes; `west build` uses SDK dtc which only warns — build tests directly for reliability
- UART emulator has RING_BUFFER_MAX_SIZE assertion failure on native_sim/native/64 — pre-existing upstream issue
- Avoid running multiple `west build` commands in parallel against the same Zephyr tree; cache writes can collide (`ToolchainCapabilityDatabase` errors)
- Avoid `rm -rf build` and excessive pristine builds to save time

## Architecture Notes

- zstnode drivers live inside a `streaming-graph` in DTS, not as children of bus controllers
- The `streaming-graph` node does NOT use `#address-cells`/`#size-cells`/`reg` — child zstnodes use plain names (no `@N` suffix)
- Zephyr SPI emulator (`zephyr,spi-emul-controller`) does NOT support async/signal mode - only sync `transceive`. Tests on native_sim can only test the polling path.
- Zephyr SPI async uses `k_poll_signal` (not callbacks). Pattern: init signal, pass to `spi_*_signal()`, wait with `k_poll()`, reset signal before reuse.
- Fixed: `tests/drivers/node` previously hung in `test_numgen_fakesink_restart_cycle` because numgen's tight generate loop prevented simulated time from advancing on native_sim. Fix: added `k_sleep(K_MSEC(1))` in `src_numgen_process()`.
- Fixed: All driver DEFINE macros had a forward-reference issue — data/config structs referenced stack/children arrays defined later by `ZSTREAMER_*_DT_DEFINE`. Fix: added `ZSTREAMER_*_DT_INST_PRE_DEFINE()` macros that emit stack/children first; every driver calls this before its data/config struct definitions.
