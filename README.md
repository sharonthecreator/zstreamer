# zstreamer

A graph-based streaming pipeline framework for [Zephyr RTOS](https://zephyrproject.org/).
Inspired by the concepts of [GStreamer](https://gstreamer.freedesktop.org/),
adapted for the constraints and conventions of embedded RTOS development.

zstreamer models data pipelines as directed graphs of **zstreamer_node** drivers
described entirely in devicetree. Nodes are connected with phandle references,
buffers are managed by a shared `net_buf` pool owned by the graph, and the
framework handles thread creation, buffer routing, and lifecycle
(start/stop) automatically. The design follows Zephyr conventions: DTS
bindings, Kconfig, `DEVICE_DT_DEFINE`, and the standard driver model.

## Architecture overview

```
                         ┌─────────────────────────────┐
                         │   streaming-graph device    │
                         │   (owns NET_BUF_POOL_FIXED) │
                         └─────────────────────────────┘
                                       │
                     ┌─────────────────┼──────────────────┐
                     │                 │                  │
               ┌─────┴─────┐    ┌──────┴──────┐    ┌──────┴──────┐
               │  SOURCE   │    │   GENERIC   │    │    SINK     │
               │ (thread)  │───>│ (workqueue) │───>│  (thread)   │
               │  api->run │    │api->process │    │ api->process│
               └───────────┘    └─────────────┘    └─────────────┘
```

A **graph** (`zstreamer,graph`) is a Zephyr device that owns a fixed-size
`net_buf_pool`. Every node inside it allocates buffers from this pool.
Nodes are children of the graph in DTS and are wired together via a
`children` phandle array that defines the data-flow edges.

### Node types

| Type | Thread model | Driver callbacks | Typical use |
|------|-------------|-----------------|-------------|
| **Source** | Dedicated `k_thread` | `open`, `run` (loop), `close` | UART RX, SPI read, ADC capture, test generators |
| **Sink** | Dedicated `k_thread` | `open`, `process` (per-buf), `close` | UART TX, SPI write, file system writer |
| **Generic** | System workqueue (`k_work`) | `open`, `process` (per-buf), `close` | Filters, format converters, tee/fan-out |

Source threads call `api->run()` in a tight loop; the driver is responsible
for blocking (e.g., `k_sem_take` on a DMA completion, or `uart_poll_in`).
Sink threads block on `k_fifo_get(&data->fifo, K_MSEC(100))` and call
`api->process()` for each buffer. Generic nodes have no thread; when a
buffer arrives the framework calls `k_work_submit()`, and the system
workqueue drains the FIFO.

### Buffer flow

1. A source calls `zstreamer_alloc_buf()` to get a `net_buf` from the
   graph pool, fills it, then calls `zstreamer_submit_buffer()`.
2. `zstreamer_submit_buffer()` walks the node's `children[]` array:
   - First child receives a `net_buf_ref()` (zero-copy).
   - Additional children receive a `net_buf_clone()` (fan-out).
   - Each buffer is placed on the child's `k_fifo`.
   - For generic children, `k_work_submit()` is called.
3. The caller's reference is released after distribution.

Buffer lifetime is fully reference-counted through `net_buf`. The framework
never copies data for single-child pipelines.

### Pipeline lifecycle

`zstreamer_start()` is **recursive**: calling it on any node first starts
all downstream children (depth-first), then opens and starts the node
itself. This means sinks are always ready before sources begin producing.
Only the root source needs to be started from application code:

```c
const struct device *src = DEVICE_DT_GET(DT_NODELABEL(my_source));
zstreamer_start(src);  /* starts entire pipeline */
```

`zstreamer_stop()` is the reverse: it stops the current node first (clears
the `running` atomic, joins the thread, drains the FIFO, calls `close`),
then recursively stops children. Children already in the target state
(`-EALREADY`) are silently skipped.

The `running` flag is an `atomic_t` checked each iteration of the source
and sink thread loops. Setting it to 0 causes the thread to exit on its
next iteration; the caller then `k_thread_join()`s to ensure clean
shutdown before calling `api->close()`.

## Devicetree model

### Binding hierarchy

```
base.yaml
  └── zstreamer,node.yaml              ← adds `children` (phandles)
        ├── zstreamer,src.yaml   ← adds thread-stack-size, thread-priority
        │     ├── zstreamer,uart-src.yaml
        │     ├── zstreamer,spi-src.yaml
        │     ├── zstreamer,adc-src.yaml
        │     └── zstreamer,numgen-src.yaml
        └── zstreamer,sink.yaml  ← adds thread-stack-size, thread-priority
              ├── zstreamer,uart-sink.yaml
              ├── zstreamer,spi-sink.yaml
              ├── zstreamer,fs-sink.yaml
              └── zstreamer,fake-sink.yaml
```

All node bindings inherit from `zstreamer,node.yaml`, which provides the
`children` property (phandle array). The `src` and `sink` base
bindings add `thread-stack-size` (default 1024) and `thread-priority`
(default 5). Driver-specific bindings add hardware references
(e.g., `uart-device`, `spi-device`, `io-channels`).

### Graph properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `buffer-count` | int | 8 | Number of buffers in the `net_buf` pool |
| `buffer-size` | int | 2048 | Size of each buffer in bytes |

### Example: UART-to-UART relay with DMA

```dts
&usart2 {
    status = "okay";
    current-speed = <115200>;
    dmas = <&gpdma1 0 27 STM32_DMA_PERIPH_TX>,
           <&gpdma1 1 26 STM32_DMA_PERIPH_RX>;
    dma-names = "tx", "rx";
};

&usart3 {
    status = "okay";
    current-speed = <115200>;
    dmas = <&gpdma1 2 29 STM32_DMA_PERIPH_TX>,
           <&gpdma1 3 28 STM32_DMA_PERIPH_RX>;
    dma-names = "tx", "rx";
};

/ {
    streaming-graph {
        compatible = "zstreamer,graph";
        buffer-count = <8>;
        buffer-size = <128>;

        uart_source: uart-source {
            compatible = "zstreamer,uart-src";
            uart-device = <&usart2>;
            children = <&uart_sinker>;
        };

        uart_sinker: uart-sinker {
            compatible = "zstreamer,uart-sink";
            uart-device = <&usart3>;
        };
    };
};
```

Note that UART baud rate, SPI frequency, and other bus parameters are
configured on the bus device itself, **not** duplicated on the node.
The node references the bus device via phandle and uses its existing
configuration.

### Example: ADC capture to file with rotation

```dts
/ {
    streaming-graph {
        compatible = "zstreamer,graph";
        buffer-count = <8>;
        buffer-size = <1024>;

        adc_source: adc-source {
            compatible = "zstreamer,adc-src";
            io-channels = <&adc1 0>, <&adc1 1>;
            sample-rate-hz = <48000>;
            resolution = <12>;
            trigger-timer = <&timers6>;
            buffer-samples = <256>;
            children = <&fs_sinker>;
            thread-stack-size = <2048>;
        };

        fs_sinker: fs-sinker {
            compatible = "zstreamer,fs-sink";
            mount-path = "/lfs/data";
            size-threshold = <65536>;
            delta-ms-threshold = <60000>;
            thread-stack-size = <2048>;
        };
    };
};
```

## Data structures and driver API

### Common config and data

Every node driver embeds these as the **first member** named `common`:

```c
/* Populated at compile time from DTS via Z_ZSTREAMER_NODE_CONFIG_INIT */
struct zstreamer_node_config {
    const struct device *graph;              /* parent graph device */
    const struct device * const *children;   /* downstream node array */
    size_t num_children;
    size_t thread_stack_size;
    int thread_priority;
    bool readonly;        /* copy-on-write: node won't modify buffers */
};

/* Populated at init time by zstreamer_node_common_init() */
struct zstreamer_node_data {
    const struct device *dev;       /* back-pointer */
    struct k_fifo fifo;             /* incoming buffer queue */
    struct k_work work;             /* for GENERIC nodes only */
    struct k_thread thread;         /* for SOURCE/SINK nodes */
    k_thread_stack_t *stack;        /* thread stack (from K_THREAD_STACK_DEFINE) */
    atomic_t running;               /* start/stop flag */
};
```

The `common` member must be first so the framework can cast `dev->config`
and `dev->data` to `zstreamer_node_config *` / `zstreamer_node_data *`.

### Driver API

```c
__subsystem struct zstreamer_node_driver_api {
    int (*open)(const struct device *dev);       /* optional, called on start */
    int (*close)(const struct device *dev);      /* optional, called on stop */
    int (*run)(const struct device *dev);        /* source only: called in loop */
    int (*process)(const struct device *dev,     /* sink/generic: per-buffer */
                   struct net_buf *buf);
};
```

- `open()` / `close()` are called by `zstreamer_start()` / `zstreamer_stop()`.
  Use them to set up DMA channels, enable interrupts, open files, etc.
- `run()` is called repeatedly from the source thread. Return 0 to continue,
  non-zero to break out of the loop. The driver should block internally
  (semaphore, poll, sleep) to avoid spinning.
- `process()` receives one buffer at a time. The framework unrefs the buffer
  after `process()` returns.

### Framework API

```c
/* Start node + all downstream children (recursive, depth-first) */
int zstreamer_start(const struct device *dev);

/* Stop node + all downstream children (recursive) */
int zstreamer_stop(const struct device *dev);

/* Allocate a buffer from the node's graph pool */
struct net_buf *zstreamer_alloc_buf(const struct device *dev,
                                    k_timeout_t timeout);

/* Distribute buffer to all children, then unref caller's reference */
int zstreamer_submit_buffer(const struct device *dev, struct net_buf *buf);
```

## Writing a new node driver

This walkthrough creates a minimal sink driver. Source drivers follow the
same pattern but implement `run()` instead of `process()`.

### 1. DTS binding

Create `dts/bindings/zstreamer/zstreamer,mydev-sink.yaml`:

```yaml
compatible: "zstreamer,mydev-sink"
include: zstreamer,sink.yaml

properties:
  my-device:
    type: phandle
    required: true
```

### 2. Driver source

```c
#define DT_DRV_COMPAT zstreamer_mydev_sink

#include <zephyr/device.h>
#include <zstreamer/node.h>
#include <zstreamer/graph.h>

struct sink_mydev_config {
    struct zstreamer_node_config common;   /* must be first */
    const struct device *hw_dev;
};

struct sink_mydev_data {
    struct zstreamer_node_data common;     /* must be first */
    /* driver-private fields here */
};

static int sink_mydev_open(const struct device *dev)
{
    /* set up hardware */
    return 0;
}

static int sink_mydev_process(const struct device *dev,
                                 struct net_buf *buf)
{
    const struct sink_mydev_config *cfg = dev->config;
    /* write buf->data (buf->len bytes) to cfg->hw_dev */
    return 0;
}

static int sink_mydev_close(const struct device *dev)
{
    /* tear down hardware */
    return 0;
}

static const struct zstreamer_node_driver_api sink_mydev_api = {
    .open    = sink_mydev_open,
    .process = sink_mydev_process,
    .close   = sink_mydev_close,
};

#define SINK_MYDEV_DEFINE(inst)                                         \
    Z_ZSTREAMER_NODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
    static K_THREAD_STACK_DEFINE(zstreamer_node_stack_##inst,                      \
        DT_INST_PROP(inst, thread_stack_size));                             \
    static struct sink_mydev_data sink_mydev_data_##inst = {         \
        .common = Z_ZSTREAMER_NODE_DATA_INIT(inst,                         \
            zstreamer_node_stack_##inst),                                          \
    };                                                                     \
    static const struct sink_mydev_config sink_mydev_cfg_##inst = {  \
        .common = { Z_ZSTREAMER_NODE_CONFIG_INIT(inst,                     \
            DT_DRV_INST(inst),                                             \
            DT_INST_PROP(inst, thread_stack_size),                         \
            DT_INST_PROP(inst, thread_priority)) },                        \
        .hw_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, my_device)),        \
    };                                                                     \
    Z_ZSTREAMER_NODE_INIT_WRAPPER_DEFINE(inst, NULL)                              \
    DEVICE_DT_INST_DEFINE(inst, zstreamer_node_init_##inst, NULL,                 \
        &sink_mydev_data_##inst,                                        \
        &sink_mydev_cfg_##inst,                                         \
        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,                   \
        &sink_mydev_api);

DT_INST_FOREACH_STATUS_OKAY(SINK_MYDEV_DEFINE)
```

Key points:

- `Z_ZSTREAMER_NODE_CHILDREN_DEFINE` generates the `children[]` array from DTS phandles.
- `Z_ZSTREAMER_NODE_CONFIG_INIT` populates the common config (graph pointer,
  children, thread params). Wrap it in `{ }` so you can append `.readonly = true`
  for nodes that promise not to modify buffers (copy-on-write optimisation).
- `Z_ZSTREAMER_NODE_DATA_INIT` sets the thread stack pointer.
- `Z_ZSTREAMER_NODE_INIT_WRAPPER_DEFINE` creates an init function that optionally
  calls a driver init, then calls `zstreamer_node_common_init()` (which sets the
  dev back-pointer, initializes the FIFO, and for generic nodes initializes
  the `k_work`).

### 3. Kconfig and CMake

```kconfig
# drivers/zstreamer/mydev/Kconfig
config ZSTREAMER_MYDEV_SINK
    bool "My device sink driver"
    depends on DT_HAS_ZSTREAMER_MYDEV_SINK_ENABLED
    depends on ZSTREAMER
```

```cmake
# drivers/zstreamer/mydev/CMakeLists.txt
zephyr_library()
zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)
zephyr_library_sources(sink_mydev.c)
```

Then wire it in the parent:

- `drivers/zstreamer/Kconfig`: add `rsource "mydev/Kconfig"`
- `drivers/zstreamer/CMakeLists.txt`: add `add_subdirectory_ifdef(CONFIG_ZSTREAMER_MYDEV mydev)`

## Included drivers

### UART source/sink (`uart-src`, `uart-sink`)

References a UART device via `uart-device` phandle. Automatically uses
DMA/async transfers when `CONFIG_UART_ASYNC_API` is enabled and the UART
driver supports it, falling back to `uart_poll_in`/`uart_poll_out`
otherwise. A shared `uart_dma_context` allows both source (RX) and sink
(TX) to coexist on the same UART using a single `uart_callback_set()`.

### SPI source/sink (`spi-src`, `spi-sink`)

References an SPI device via `spi-device` phandle. Probes async support at
`open()` time using `k_poll_signal`; falls back to synchronous
`spi_read_dt()`/`spi_write_dt()` if async is unavailable. SPI bus
parameters (frequency, CPOL/CPHA, CS) come from the referenced SPI device
node, not from the zstreamer node.

### ADC source (`adc-src`)

Timer-triggered DMA capture for precise sample rates. Currently has an
STM32-specific backend (`CONFIG_ZSTREAMER_ADC_STM32`) that configures
timer TRGO, ADC external trigger, and DMA circular buffer with
half-transfer/complete interrupts for ping-pong streaming. Supports
1-2 channels (mono/stereo). Key DTS properties: `io-channels`,
`sample-rate-hz`, `resolution`, `trigger-timer`, `buffer-samples`.

### File system sink (`fs-sink`)

Writes streamed data to files on any Zephyr-supported filesystem (LittleFS,
FAT, etc.). Rotates to a new file based on configurable thresholds:

| DTS property | Description |
|-------------|-------------|
| `mount-path` | Directory path (e.g., `/lfs/data`) |
| `size-threshold` | Max bytes per file before rotation |
| `delta-ms-threshold` | Max milliseconds before rotation |

At least one threshold must be non-zero (enforced by `BUILD_ASSERT`).
Default filenames are `<mount-path>/00000.bin`, `00001.bin`, etc. Override
with `sink_fs_set_filename_handler()` before starting the pipeline.

### Test nodes (`numgen-src`, `fake-sink`)

Software-only nodes for testing. `numgen-src` fills buffers with
sequential bytes 0-255. `fake-sink` discards all received data. No
hardware dependencies; suitable for `native_sim` and unit tests.

## Kconfig reference

| Symbol | Description | Depends on |
|--------|------------|-----------|
| `CONFIG_ZSTREAMER` | Enable the framework | `NET_BUF` (auto-selected) |
| `CONFIG_ZSTREAMER_UART_SRC` | UART source driver | `DT_HAS_ZSTREAMER_UART_SRC_ENABLED`, `SERIAL` |
| `CONFIG_ZSTREAMER_UART_SINK` | UART sink driver | `DT_HAS_ZSTREAMER_UART_SINK_ENABLED`, `SERIAL` |
| `CONFIG_ZSTREAMER_UART_DMA` | UART DMA/async support | `UART_ASYNC_API` |
| `CONFIG_ZSTREAMER_SPI_SRC` | SPI source driver | `DT_HAS_ZSTREAMER_SPI_SRC_ENABLED`, `SPI` |
| `CONFIG_ZSTREAMER_SPI_SINK` | SPI sink driver | `DT_HAS_ZSTREAMER_SPI_SINK_ENABLED`, `SPI` |
| `CONFIG_ZSTREAMER_ADC_SRC` | ADC source driver | `DT_HAS_ZSTREAMER_ADC_SRC_ENABLED`, `ADC` |
| `CONFIG_ZSTREAMER_ADC_STM32` | STM32 ADC backend | `SOC_FAMILY_STM32`, `DMA` |
| `CONFIG_ZSTREAMER_FS_SINK` | File system sink driver | `DT_HAS_ZSTREAMER_FS_SINK_ENABLED`, `FILE_SYSTEM` |
| `CONFIG_ZSTREAMER_TEST_NUMGEN_SRC` | Number generator source | `DT_HAS_ZSTREAMER_NUMGEN_SRC_ENABLED` |
| `CONFIG_ZSTREAMER_TEST_FAKE_SINK` | Fake sink | `DT_HAS_ZSTREAMER_FAKE_SINK_ENABLED` |

Driver Kconfig symbols depend on the corresponding DTS compatible
(`DT_HAS_*_ENABLED`) and must be explicitly enabled in `prj.conf`.

## Module integration

zstreamer is a Zephyr module. The `zephyr/module.yml` sets `dts_root: .`
so all bindings under `dts/bindings/` are automatically picked up. The
module Kconfig and CMake entry points are at the repository root.

**west.yml** (standalone manifest):

```yaml
manifest:
  remotes:
    - name: zephyrproject-rtos
      url-base: https://github.com/zephyrproject-rtos
  projects:
    - name: zephyr
      revision: v4.2.0
      import:
        name-allowlist:
          - cmsis_6
          - hal_stm32
  self:
    path: modules/zstreamer
```

Or add to an existing manifest as an extra module:

```yaml
projects:
  - name: zstreamer
    url: https://github.com/yourorg/zstreamer
    revision: main
    path: modules/zstreamer
```

## Building and testing

```sh
# Build a sample
west build -b nucleo_u575zi_q samples/uart2uart
west flash

# Run the test suite
west twister -T tests/
```

## Repository layout

```
zstreamer/
├── CMakeLists.txt                          # Top-level: adds subsys/ and drivers/
├── Kconfig                                 # Top-level: CONFIG_ZSTREAMER menuconfig
├── zephyr/module.yml                       # Zephyr module descriptor
├── west.yml                                # Standalone west manifest
├── include/
│   └── zstreamer/
│       ├── graph.h                          # Graph device types (config, data)
│       ├── node.h                           # Driver API, common structs, DT macros
│       └── sink_fs.h                     # FS sink public API (filename callback)
├── subsys/zstreamer/
│   ├── zstreamer_graph.c                   # Graph device: NET_BUF_POOL_FIXED per instance
│   └── zstreamer_node.c                    # Node lifecycle, threads, buffer routing
├── dts/bindings/zstreamer/
│   ├── zstreamer,node.yaml                        # Base: children phandle array
│   ├── zstreamer,graph.yaml                # Graph: buffer-count, buffer-size
│   ├── zstreamer,src.yaml               # Source base: thread-stack-size, priority
│   ├── zstreamer,sink.yaml              # Sink base: thread-stack-size, priority
│   ├── zstreamer,uart-src.yaml          # UART source
│   ├── zstreamer,uart-sink.yaml         # UART sink
│   ├── zstreamer,spi-src.yaml           # SPI source
│   ├── zstreamer,spi-sink.yaml          # SPI sink
│   ├── zstreamer,adc-src.yaml           # ADC source (timer-triggered DMA)
│   ├── zstreamer,fs-sink.yaml           # File system sink
│   ├── zstreamer,numgen-src.yaml        # Test: number generator
│   └── zstreamer,fake-sink.yaml         # Test: /dev/null sink
├── drivers/zstreamer/
│   ├── uart/                               # UART src + sink + DMA context
│   ├── spi/                                # SPI src + sink (async probe)
│   ├── adc/                                # ADC src (STM32 timer+DMA backend)
│   ├── fs/                                 # File system sink (rotation)
│   └── test/                               # numgen source + fake sink
├── samples/
│   ├── uart2uart/                          # UART relay
│   ├── spi2spi/                            # SPI relay
│   ├── adc2fakesink/                       # ADC capture to fake sink
│   └── numgen2fakesink/                    # Software-only test pipeline
└── tests/
    ├── drivers/zstreamer/                    # UART node ztest suite
    └── drivers/node_spi/                # SPI node ztest suite
```

## License

Apache-2.0
