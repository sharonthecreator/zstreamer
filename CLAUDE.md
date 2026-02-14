# zstreamer Project Memory

## Design Rules

- **No DTS duplication**: Properties configurable through a normal device's DTS (e.g., SPI frequency, UART baud rate) must NOT be duplicated in the zstnode's device DTS. The zstnode should reference the device and use its existing config.

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
