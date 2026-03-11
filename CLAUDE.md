# zstreamer Project Rules

## Architecture

Node type hierarchy — all extend `node.h` base config/data:
- **source** (`source.h`): active producer, start/stop lifecycle
- **sink** (`sink.h`): terminal consumer, unrefs buffers
- **filter** (`filter.h`): conditional routing via children + false-children
- **processor**: uses `node.h` directly, no extra header

Source drivers MUST use `zstreamer_source_common_init` (not `zstreamer_node_common_init`) — inits semaphores before thread creation.

Every driver must call `ZSTREAMER_*_DT_INST_PRE_DEFINE(inst)` BEFORE defining data/config structs.

## Naming

**Family_Action** everywhere: files, structs, functions, macros, DTS compatibles.
Examples: `numgen_src`, `count_sink`, `odd_filter`, `spi_src`, `zstreamer,numgen-src`.

## DTS

- Nodes live inside a `streaming-graph` container (no `#address-cells`/`reg`)
- Peripherals referenced via phandles; don't duplicate device config in zstnode DTS

## Tests

```sh
# Run all test suites via twister
west twister -T tests -p native_sim --inline-logs

# Or build/run a single suite manually
west build -b native_sim -d build/test-<name> tests/subsys/<name> -p auto
timeout 120s ./build/test-<name>/zephyr/zephyr.exe
```

## Design Rules

- Start/stop contract: first `start()` → 0, second → `-EALREADY`; same for `stop()`
- Thread stack size is fixed at `ZSTREAMER_THREAD_STACK_SIZE` (2048) in `node.h`. Do NOT increase it — if a driver needs extra memory, allocate it in the driver's data struct, not on the stack.
- Use public `net_buf` APIs only

## Environment

- Linux, no Docker. Board: `native_sim`
- No parallel `west build` against same Zephyr tree
- native_sim: simulated time advances only when ALL threads blocked — use `k_sleep`/`k_yield` in loops
