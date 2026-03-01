# zstreamer

A graph-based streaming pipeline framework for [Zephyr RTOS](https://zephyrproject.org/).
Nodes are Zephyr devices defined in devicetree and connected with `children` phandles.
Each graph owns a shared `net_buf` pool used by all nodes in that graph.

## Architecture

### Graph

`zstreamer,graph` is a container device that owns a `NET_BUF_POOL_FIXED` pool.
All streaming nodes are children of a graph in the devicetree.

### Node types

zstreamer provides four compile-time node types, each with its own header, subsystem
implementation, and DTS binding. All share a single unified driver API struct
(`struct zstreamer_node_driver_api` in `node.h`).

| Type | Header | Callback | Has children | Start/Stop |
|------|--------|----------|-------------|------------|
| **Source** | `source.h` | `process(dev, buf)` | Yes | `zstreamer_source_start/stop()` |
| **Sink** | `sink.h` | `process(dev, buf)` | No (enforced at compile time) | N/A (always running) |
| **Processor** | `node.h` | `process(dev, buf)` | Yes | N/A (always running) |
| **Filter** | `filter.h` | `process(dev, buf)` → `int` (0/1) | Yes + `false_children` | N/A (always running) |

- **Source**: Allocates buffers, fills them via `process`, and fans out to children.
  Only sources can be started/stopped at runtime.
- **Sink**: Terminal node. Consumes buffers from its FIFO and unrefs them. The sink
  DTS binding has no `children` property — attempting to add children is a build error.
- **Processor**: Receives buffers, processes them in-place, and forwards to children.
  Uses the base `node.h` types directly.
- **Filter**: Receives buffers, runs `process` callback. If it returns 1 (true),
  distributes to `children`; if 0 (false), distributes to `false_children`;
  if negative, logs the error and drops the buffer.

All non-source nodes run their thread immediately at boot. Driver setup belongs in the Zephyr init function.

### Lifecycle contract (source nodes)

```
first  zstreamer_source_start() → 0
second zstreamer_source_start() → -EALREADY
first  zstreamer_source_stop()  → 0
second zstreamer_source_stop()  → -EALREADY
```

## Public API

### Common (all node types)

Declared in `include/zstreamer/node.h`:

```c
struct net_buf *zstreamer_node_alloc_buf(const struct device *dev,
                                         k_timeout_t timeout);
```

### Source

Declared in `include/zstreamer/source.h`:

```c
int zstreamer_source_start(const struct device *dev);
int zstreamer_source_stop(const struct device *dev);
```

### Driver API struct

All node types share a single unified driver API:

```c
__subsystem struct zstreamer_node_driver_api {
    int (*process)(const struct device *dev, struct net_buf *buf);
};
```

Driver setup belongs in the Zephyr init function, not in the API struct.

For filters, the `process` callback uses the return value convention:
`1` = true (route to children), `0` = false (route to false_children),
`<0` = error (buffer is dropped).

## Devicetree conventions

### Binding hierarchy

```
zstreamer,node.yaml           (base: thread-stack-size, thread-priority)
├── zstreamer,src.yaml         (adds: children)
│   ├── zstreamer,uart-src.yaml
│   ├── zstreamer,spi-src.yaml
│   ├── zstreamer,adc-src.yaml
│   └── zstreamer,numgen-src.yaml
├── zstreamer,sink.yaml        (no children)
│   ├── zstreamer,uart-sink.yaml
│   ├── zstreamer,spi-sink.yaml
│   ├── zstreamer,fake-sink.yaml
│   └── zstreamer,fs-sink.yaml
├── zstreamer,processor.yaml   (adds: children)
└── zstreamer,filter.yaml      (adds: children, false-children)
```

### DTS rules

- Do not duplicate bus properties on zstreamer nodes.
  UART baud, SPI frequency/mode, etc. stay on the referenced bus device.
- Streaming nodes belong under a `zstreamer,graph` container node.
- Graph child nodes use plain names (no `@N` suffix, no `reg`).
- Vendor registration in `dts/bindings/vendor-prefixes.txt` must include `zstreamer`.

### Example overlay

```dts
/ {
    streaming_graph: streaming-graph {
        compatible = "zstreamer,graph";
        pool-count = <16>;
        pool-size = <128>;

        spi_source: spi-source {
            compatible = "zstreamer,spi-src";
            spi-dev = <&spi1>;
            children = <&spi_sink>;
            thread-stack-size = <2048>;
            thread-priority = <5>;
        };

        spi_sink: spi-sink {
            compatible = "zstreamer,spi-sink";
            spi-dev = <&spi2>;
            thread-stack-size = <2048>;
            thread-priority = <5>;
        };
    };
};
```

## Writing a new driver

Every zstreamer driver follows the same pattern:

1. Set `DT_DRV_COMPAT` and include the type-specific header.
2. Call the type's `PRE_DEFINE` macro to emit stack/children symbols.
3. Define config/data structs using the type's `CONFIG_INIT` / `DATA_INIT` macros.
4. Implement a `struct zstreamer_node_driver_api` with `.process`.
5. Register with `DEVICE_DT_INST_DEFINE(...)`, passing `zstreamer_node_common_init`
   directly (or a custom init function that calls it at the end).
   Drivers that need hardware setup should use a custom init function.

If the driver adds no extra config or data fields, use the base type structs
directly (e.g. `struct zstreamer_sink_config`). If it does, embed the base
type as `.common` and initialize it with the corresponding `CONFIG_INIT` /
`DATA_INIT` macro.

### Source driver

Binding: includes `zstreamer,src.yaml`.

Minimal example (no extra fields):

```c
#define DT_DRV_COMPAT zstreamer_my_src

#include <zstreamer/source.h>

static int my_src_process(const struct device *dev, struct net_buf *buf) {
    /* Fill buf with data. */
    return 0;
}

static const struct zstreamer_node_driver_api my_src_api = {
    .process = my_src_process,
};

#define MY_SRC_DEFINE(inst)                                           \
  ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                          \
  static struct zstreamer_source_data my_src_data_##inst =            \
      ZSTREAMER_SOURCE_DATA_INIT(inst);                             \
  static const struct zstreamer_source_config my_src_config_##inst =  \
      ZSTREAMER_SOURCE_CONFIG_INIT(inst);                           \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_node_common_init, NULL,     \
                        &my_src_data_##inst, &my_src_config_##inst,   \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
                        &my_src_api);

DT_INST_FOREACH_STATUS_OKAY(MY_SRC_DEFINE)
```

With extra data fields:

```c
struct my_src_data {
    struct zstreamer_source_data common;
    uint8_t counter;
};

#define MY_SRC_DEFINE(inst)                                           \
  ZSTREAMER_SOURCE_DT_INST_PRE_DEFINE(inst);                          \
  static struct my_src_data my_src_data_##inst = {                    \
      .common = ZSTREAMER_SOURCE_DATA_INIT(inst),                   \
      .counter = 0,                                                   \
  };                                                                  \
  static const struct zstreamer_source_config my_src_config_##inst =  \
      ZSTREAMER_SOURCE_CONFIG_INIT(inst);                           \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_node_common_init, NULL,     \
                        &my_src_data_##inst, &my_src_config_##inst,   \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
                        &my_src_api);
```

### Sink driver

Binding: includes `zstreamer,sink.yaml`.

Minimal example:

```c
#define DT_DRV_COMPAT zstreamer_my_sink

#include <zstreamer/sink.h>

static int my_sink_process(const struct device *dev, struct net_buf *buf) {
    /* Consume buf. */
    return 0;
}

static const struct zstreamer_node_driver_api my_sink_api = {
    .process = my_sink_process,
};

#define MY_SINK_DEFINE(inst)                                            \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                              \
  static struct zstreamer_sink_data my_sink_data_##inst =               \
      ZSTREAMER_SINK_DATA_INIT(inst);                                 \
  static const struct zstreamer_sink_config my_sink_config_##inst =     \
      ZSTREAMER_SINK_CONFIG_INIT(inst);                               \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_node_common_init, NULL,         \
                        &my_sink_data_##inst, &my_sink_config_##inst,   \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
                        &my_sink_api);

DT_INST_FOREACH_STATUS_OKAY(MY_SINK_DEFINE)
```

With custom init logic (e.g. hardware setup):

```c
static int my_sink_init(const struct device *dev) {
    /* Driver-specific init (e.g. configure a peripheral). */
    /* ... */

    /* Always call common_init last — it starts the node thread. */
    return zstreamer_node_common_init(dev);
}

/* In the DEFINE macro, pass my_sink_init instead of
   zstreamer_node_common_init:                                        */
  DEVICE_DT_INST_DEFINE(inst, my_sink_init, NULL, ...);
```

### Processor (through-node) driver

Binding: includes `zstreamer,processor.yaml`.

Processors receive buffers, optionally transform them in-place, and forward
to children automatically. Use `node.h` types and macros.

```c
#define DT_DRV_COMPAT zstreamer_my_processor

#include <zstreamer/node.h>

static int my_processor_process(const struct device *dev,
                                struct net_buf *buf) {
    /* Transform buf in-place. Return 0 to forward, <0 to drop. */
    return 0;
}

static const struct zstreamer_node_driver_api my_processor_api = {
    .process = my_processor_process,
};

#define MY_PROCESSOR_DEFINE(inst)                                        \
  ZSTREAMER_NODE_DT_INST_PRE_DEFINE(inst);                               \
  static struct zstreamer_node_data my_processor_data_##inst =           \
      ZSTREAMER_NODE_DATA_INIT(inst);                                  \
  static const struct zstreamer_node_config my_processor_config_##inst = \
      ZSTREAMER_NODE_CONFIG_INIT(inst);                                \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_node_common_init, NULL,      \
                        &my_processor_data_##inst,                       \
                        &my_processor_config_##inst, POST_KERNEL,        \
                        CONFIG_KERNEL_INIT_PRIORITY_DEVICE,              \
                        &my_processor_api);

DT_INST_FOREACH_STATUS_OKAY(MY_PROCESSOR_DEFINE)
```

### Filter driver

Binding: includes `zstreamer,filter.yaml`.

The `process` callback returns an `int` with the convention:
`1` = true (forward to `children`), `0` = false (forward to
`false_children`), `<0` = error (buffer is dropped).

```c
#define DT_DRV_COMPAT zstreamer_my_filter

#include <zstreamer/filter.h>

static int my_filter_process(const struct device *dev, struct net_buf *buf) {
    /* Return 1 to route to children, 0 for false_children, <0 for error. */
    return (buf->data[0] > 127) ? 1 : 0;
}

static const struct zstreamer_node_driver_api my_filter_api = {
    .process = my_filter_process,
};

#define MY_FILTER_DEFINE(inst)                                            \
  ZSTREAMER_FILTER_DT_INST_PRE_DEFINE(inst);                              \
  static struct zstreamer_filter_data my_filter_data_##inst =             \
      ZSTREAMER_FILTER_DATA_INIT(inst);                                 \
  static const struct zstreamer_filter_config my_filter_config_##inst =   \
      ZSTREAMER_FILTER_CONFIG_INIT(inst);                               \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_node_common_init, NULL,         \
                        &my_filter_data_##inst, &my_filter_config_##inst, \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,  \
                        &my_filter_api);

DT_INST_FOREACH_STATUS_OKAY(MY_FILTER_DEFINE)
```

### Macro quick-reference

| Macro | Purpose |
|-------|---------|
| `ZSTREAMER_<TYPE>_DT_INST_PRE_DEFINE(inst)` | Emit stack/children symbols (call first) |
| `ZSTREAMER_<TYPE>_CONFIG_INIT(inst)` | Braced initializer for the type's config struct |
| `ZSTREAMER_<TYPE>_DATA_INIT(inst)` | Braced initializer for the type's data struct |
| `zstreamer_node_common_init` | Init function to pass to `DEVICE_DT_INST_DEFINE` (all types) |

Where `<TYPE>` is `NODE`, `SOURCE`, `SINK`, or `FILTER` (upper-case for macros).

## Included drivers

| Driver | Type | Location |
|--------|------|----------|
| UART source | Source | `drivers/zstreamer/uart/uart_src.c` |
| UART sink | Sink | `drivers/zstreamer/uart/uart_sink.c` |
| SPI source | Source | `drivers/zstreamer/spi/spi_src.c` |
| SPI sink | Sink | `drivers/zstreamer/spi/spi_sink.c` |
| ADC source | Source | `drivers/zstreamer/adc/adc_src.c` |
| FS sink | Sink | `drivers/zstreamer/fs/fs_sink.c` |
| Numgen source (test) | Source | `drivers/zstreamer/test/numgen_src.c` |
| Fake sink (test) | Sink | `drivers/zstreamer/test/fake_sink.c` |
| Count sink (test) | Sink | `drivers/zstreamer/test/count_sink.c` |
| Passthrough (test) | Processor | `drivers/zstreamer/test/passthrough_node.c` |
| Odd filter (test) | Filter | `drivers/zstreamer/test/odd_filter.c` |

## Samples

| Sample | Description |
|--------|-------------|
| `samples/uart2uart` | UART RX → UART TX pipeline |
| `samples/spi2spi` | SPI RX → SPI TX pipeline |
| `samples/adc2fakesink` | ADC capture → fake sink (logging) |
| `samples/numgen2fakesink` | Number generator → fake sink (logging) |

## Tests

All test suites target `native_sim` only. On macOS, use Docker
(`zephyrprojectrtos/zephyr-build`) for builds.

Shared test helpers live in `tests/include/zstreamer_test/helpers.h`
(`pattern_byte()` deterministic byte generator used by UART and SPI stress tests).

### UART (`tests/drivers/uart/`)

Pipeline: `uart-src` (UART emulator) -> `uart-sink` (UART emulator).
Twister ID: `drivers.zstreamer.uart`

| # | Test | Description |
|---|------|-------------|
| 1 | `test_devices_ready` | Graph, source, sink, UART src, UART sink devices are ready |
| 2 | `test_start_stop` | Strict start/stop: first start->0, second->-EALREADY, first stop->0, second->-EALREADY |
| 3 | `test_buf_alloc` | Allocate buffer from graph pool, verify tailroom, free |
| 4 | `test_uart_relay` | Relay 5 bytes ("hello") through pipeline, verify exact match |
| 5 | `test_long_transfer` | 200-byte multi-buffer transfer |
| 6 | `test_burst_writes` | 5x3-byte writes with delays, verify concatenated output |
| 7 | `test_restart_cycle` | 5 start/transfer/stop cycles with 2-byte pattern |
| 8 | `test_throughput_stress` | 256-byte max-throughput transfer with pattern verification |
| 9 | `test_1mb_transfer` | 1 MiB streamed in 4 KiB chunks, every byte verified via `pattern_byte()` |
| 10 | `test_2mb_varied_chunks` | 2 MiB with alternating chunk sizes (137, 4096, 1, 8192, 53, 2048, 7, 512) |

### SPI (`tests/drivers/spi/`)

Pipeline: `spi-src` (SPI emulator) -> `spi-sink` (SPI emulator).
Uses a custom `spi-test-peripheral` emulator with 4 KiB ring buffers.
Twister ID: `drivers.zstreamer.spi`

| # | Test | Description |
|---|------|-------------|
| 1 | `test_devices_ready` | Graph, SPI source, SPI sink, SPI rx/tx peripherals are ready |
| 2 | `test_start_stop` | Strict start/stop state machine |
| 3 | `test_buf_alloc` | Allocate buffer, verify tailroom, free |
| 4 | `test_spi_relay` | 9-byte relay through SPI, verify first N bytes match |
| 5 | `test_spi_long_transfer` | 256-byte multi-frame transfer (4x 64-byte reads) |
| 6 | `test_spi_restart_cycle` | 5 start/stop/transfer cycles with 8-byte pattern |
| 7 | `test_spi_stress` | 1024-byte / 16-frame stress test |
| 8 | `test_spi_large_verified` | 4 KiB with per-byte pattern verification |
| 9 | `test_spi_64kb_streamed` | 64 KiB continuous feed/drain in 1536-byte chunks |
| 10 | `test_spi_96kb_varied_chunks` | 96 KiB with mixed chunk sizes (64, 137, 511, 1024, 2048, 777, 1536) |

### Source (`tests/drivers/source/`)

Pipeline: `numgen-src` -> `count-sink`.
Twister ID: `drivers.zstreamer.source`

| # | Test | Description |
|---|------|-------------|
| 1 | `test_devices_ready` | Graph, numgen source, count sink are ready |
| 2 | `test_start_stop` | Strict: first start->0, second->-EALREADY, first stop->0, second->-EALREADY |
| 3 | `test_buf_alloc` | Allocate buffer from pool, verify tailroom, free |
| 4 | `test_data_flow` | Run pipeline 200 ms, verify count sink received >0 buffers |
| 5 | `test_restart_cycle` | 5 start/stop cycles, verify buffers flow each time |

### Sink (`tests/drivers/sink/`)

Pipeline: `numgen-src` -> `count-sink`.
Twister ID: `drivers.zstreamer.sink`

| # | Test | Description |
|---|------|-------------|
| 1 | `test_devices_ready` | Graph, numgen source, count sink are ready |
| 2 | `test_buf_alloc` | Allocate buffer, verify tailroom, free |
| 3 | `test_sink_processes_data` | Run pipeline 200 ms, verify sink received >0 buffers |
| 4 | `test_sink_byte_count` | Verify bytes == bufs x buffer_size (64) |

### Node / Processor (`tests/drivers/node/`)

Pipeline: `numgen-src` -> `passthrough` (processor) -> `count-sink`.
Twister ID: `drivers.zstreamer.node`

| # | Test | Description |
|---|------|-------------|
| 1 | `test_devices_ready` | Graph, source, passthrough processor, count sink are ready |
| 2 | `test_pipeline_flow` | Run 200 ms, verify buffers passed through processor to sink |
| 3 | `test_processor_restart_cycle` | 5 start/stop cycles, verify buffers flow through processor each time |

### Filter (`tests/drivers/filter/`)

Pipeline: `numgen-src` -> `odd-filter` -> `{true-sink, false-sink}`.
Twister ID: `drivers.zstreamer.filter`

| # | Test | Description |
|---|------|-------------|
| 1 | `test_devices_ready` | Graph, source, odd-filter, true sink, false sink are ready |
| 2 | `test_filter_routing` | Run 200 ms, verify both true and false paths received buffers |
| 3 | `test_filter_balanced` | Verify true/false paths differ by at most 1 buffer |
| 4 | `test_filter_restart_cycle` | 5 start/stop cycles, verify both paths active each time |

## Build examples

```sh
# Sample build (hardware target)
west build -b nucleo_u575zi_q samples/uart2uart

# Test builds (native_sim)
west build -b native_sim tests/drivers/uart
west build -b native_sim tests/drivers/spi
west build -b native_sim tests/drivers/source
west build -b native_sim tests/drivers/sink
west build -b native_sim tests/drivers/node
west build -b native_sim tests/drivers/filter
```

## Module integration

`zephyr/module.yml` points `dts_root: .`, so bindings under `dts/bindings/` are
discovered automatically by Zephyr's build system.

## License

Apache-2.0
