# zstreamer

A graph-based streaming pipeline framework for [Zephyr RTOS](https://zephyrproject.org/).
Inspired by the concepts of [GStreamer](https://gstreamer.freedesktop.org/),
adapted for the constraints and conventions of embedded RTOS development.

zstreamer models data pipelines as directed graphs of **zstnode** drivers
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
  └── zstnode.yaml              ← adds `children` (phandles)
        ├── zstreamer,zstsrc.yaml   ← adds thread-stack-size, thread-priority
        │     ├── zstreamer,zstsrc-uart.yaml
        │     ├── zstreamer,zstsrc-spi.yaml
        │     ├── zstreamer,zstsrc-adc.yaml
        │     └── zstreamer,zstsrc-numgen.yaml
        └── zstreamer,zstsink.yaml  ← adds thread-stack-size, thread-priority
              ├── zstreamer,zstsink-uart.yaml
              ├── zstreamer,zstsink-spi.yaml
              ├── zstreamer,zstsink-fs.yaml
              └── zstreamer,zstsink-fake.yaml
```

All zstnode bindings inherit from `zstnode.yaml`, which provides the
`children` property (phandle array). The `zstsrc` and `zstsink` base
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
            compatible = "zstreamer,zstsrc-uart";
            uart-device = <&usart2>;
            children = <&uart_sinker>;
        };

        uart_sinker: uart-sinker {
            compatible = "zstreamer,zstsink-uart";
            uart-device = <&usart3>;
        };
    };
};
```

Note that UART baud rate, SPI frequency, and other bus parameters are
configured on the bus device itself, **not** duplicated on the zstnode.
The zstnode references the bus device via phandle and uses its existing
configuration.

### Example: ADC capture to file with rotation

```dts
/ {
    streaming-graph {
        compatible = "zstreamer,graph";
        buffer-count = <8>;
        buffer-size = <1024>;

        adc_source: adc-source {
            compatible = "zstreamer,zstsrc-adc";
            io-channels = <&adc1 0>, <&adc1 1>;
            sample-rate-hz = <48000>;
            resolution = <12>;
            trigger-timer = <&timers6>;
            buffer-samples = <256>;
            children = <&fs_sinker>;
            thread-stack-size = <2048>;
        };

        fs_sinker: fs-sinker {
            compatible = "zstreamer,zstsink-fs";
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

Every zstnode driver embeds these as the **first member** named `common`:

```c
/* Populated at compile time from DTS via Z_ZSTNODE_COMMON_CONFIG_INIT */
struct zstnode_common_config {
    const struct device *graph;              /* parent graph device */
    const struct device * const *children;   /* downstream node array */
    size_t num_children;
    size_t thread_stack_size;
    int thread_priority;
    bool readonly;        /* copy-on-write: node won't modify buffers */
};

/* Populated at init time by zstnode_common_init() */
struct zstnode_common_data {
    const struct device *dev;       /* back-pointer */
    struct k_fifo fifo;             /* incoming buffer queue */
    struct k_work work;             /* for GENERIC nodes only */
    struct k_thread thread;         /* for SOURCE/SINK nodes */
    k_thread_stack_t *stack;        /* thread stack (from K_THREAD_STACK_DEFINE) */
    atomic_t running;               /* start/stop flag */
};
```

The `common` member must be first so the framework can cast `dev->config`
and `dev->data` to `zstnode_common_config *` / `zstnode_common_data *`.

### Driver API

```c
__subsystem struct zstnode_driver_api {
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

Create `dts/bindings/streaming/zstreamer,zstsink-mydev.yaml`:

```yaml
compatible: "zstreamer,zstsink-mydev"
include: zstreamer,zstsink.yaml

properties:
  my-device:
    type: phandle
    required: true
```

### 2. Driver source

```c
#define DT_DRV_COMPAT zstreamer_zstsink_mydev

#include <zephyr/device.h>
#include <zephyr/drivers/zstnode.h>
#include <zstreamer/zstreamer.h>

struct zstsink_mydev_config {
    struct zstnode_common_config common;   /* must be first */
    const struct device *hw_dev;
};

struct zstsink_mydev_data {
    struct zstnode_common_data common;     /* must be first */
    /* driver-private fields here */
};

static int zstsink_mydev_open(const struct device *dev)
{
    /* set up hardware */
    return 0;
}

static int zstsink_mydev_process(const struct device *dev,
                                 struct net_buf *buf)
{
    const struct zstsink_mydev_config *cfg = dev->config;
    /* write buf->data (buf->len bytes) to cfg->hw_dev */
    return 0;
}

static int zstsink_mydev_close(const struct device *dev)
{
    /* tear down hardware */
    return 0;
}

static const struct zstnode_driver_api zstsink_mydev_api = {
    .open    = zstsink_mydev_open,
    .process = zstsink_mydev_process,
    .close   = zstsink_mydev_close,
};

#define ZSTSINK_MYDEV_DEFINE(inst)                                         \
    Z_ZSTNODE_CHILDREN_DEFINE(inst, DT_DRV_INST(inst));                    \
    static K_THREAD_STACK_DEFINE(zstnode_stack_##inst,                      \
        DT_INST_PROP(inst, thread_stack_size));                             \
    static struct zstsink_mydev_data zstsink_mydev_data_##inst = {         \
        .common = Z_ZSTNODE_COMMON_DATA_INIT(inst,                         \
            zstnode_stack_##inst),                                          \
    };                                                                     \
    static const struct zstsink_mydev_config zstsink_mydev_cfg_##inst = {  \
        .common = { Z_ZSTNODE_COMMON_CONFIG_INIT(inst,                     \
            DT_DRV_INST(inst),                                             \
            DT_INST_PROP(inst, thread_stack_size),                         \
            DT_INST_PROP(inst, thread_priority)) },                        \
        .hw_dev = DEVICE_DT_GET(DT_INST_PHANDLE(inst, my_device)),        \
    };                                                                     \
    Z_ZSTNODE_INIT_WRAPPER_DEFINE(inst, NULL)                              \
    DEVICE_DT_INST_DEFINE(inst, zstnode_init_##inst, NULL,                 \
        &zstsink_mydev_data_##inst,                                        \
        &zstsink_mydev_cfg_##inst,                                         \
        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,                   \
        &zstsink_mydev_api);

DT_INST_FOREACH_STATUS_OKAY(ZSTSINK_MYDEV_DEFINE)
```

Key points:

- `Z_ZSTNODE_CHILDREN_DEFINE` generates the `children[]` array from DTS phandles.
- `Z_ZSTNODE_COMMON_CONFIG_INIT` populates the common config (graph pointer,
  children, thread params). Wrap it in `{ }` so you can append `.readonly = true`
  for nodes that promise not to modify buffers (copy-on-write optimisation).
- `Z_ZSTNODE_COMMON_DATA_INIT` sets the thread stack pointer.
- `Z_ZSTNODE_INIT_WRAPPER_DEFINE` creates an init function that optionally
  calls a driver init, then calls `zstnode_common_init()` (which sets the
  dev back-pointer, initializes the FIFO, and for generic nodes initializes
  the `k_work`).

### 3. Kconfig and CMake

```kconfig
# drivers/zstnode/mydev/Kconfig
config ZSTNODE_MYDEV_SINK
    bool "My device sink zstnode driver"
    default y
    depends on DT_HAS_ZSTREAMER_ZSTSINK_MYDEV_ENABLED
    depends on ZSTNODE
```

```cmake
# drivers/zstnode/mydev/CMakeLists.txt
zephyr_library()
zephyr_include_directories(${ZEPHYR_ZSTREAMER_MODULE_DIR}/include)
zephyr_library_sources(zstsink_mydev.c)
```

Then wire it in the parent:

- `drivers/zstnode/Kconfig`: add `rsource "mydev/Kconfig"`
- `drivers/zstnode/CMakeLists.txt`: add `if(CONFIG_ZSTNODE_MYDEV_SINK) add_subdirectory(mydev) endif()`

## Included drivers

### UART source/sink (`zstsrc-uart`, `zstsink-uart`)

References a UART device via `uart-device` phandle. Automatically uses
DMA/async transfers when `CONFIG_UART_ASYNC_API` is enabled and the UART
driver supports it, falling back to `uart_poll_in`/`uart_poll_out`
otherwise. A shared `uart_dma_context` allows both source (RX) and sink
(TX) to coexist on the same UART using a single `uart_callback_set()`.

### SPI source/sink (`zstsrc-spi`, `zstsink-spi`)

References an SPI device via `spi-device` phandle. Probes async support at
`open()` time using `k_poll_signal`; falls back to synchronous
`spi_read_dt()`/`spi_write_dt()` if async is unavailable. SPI bus
parameters (frequency, CPOL/CPHA, CS) come from the referenced SPI device
node, not from the zstnode.

### ADC source (`zstsrc-adc`)

Timer-triggered DMA capture for precise sample rates. Currently has an
STM32-specific backend (`CONFIG_ZSTNODE_ADC_STM32`) that configures
timer TRGO, ADC external trigger, and DMA circular buffer with
half-transfer/complete interrupts for ping-pong streaming. Supports
1-2 channels (mono/stereo). Key DTS properties: `io-channels`,
`sample-rate-hz`, `resolution`, `trigger-timer`, `buffer-samples`.

### File system sink (`zstsink-fs`)

Writes streamed data to files on any Zephyr-supported filesystem (LittleFS,
FAT, etc.). Rotates to a new file based on configurable thresholds:

| DTS property | Description |
|-------------|-------------|
| `mount-path` | Directory path (e.g., `/lfs/data`) |
| `size-threshold` | Max bytes per file before rotation |
| `delta-ms-threshold` | Max milliseconds before rotation |

At least one threshold must be non-zero (enforced by `BUILD_ASSERT`).
Default filenames are `<mount-path>/00000.bin`, `00001.bin`, etc. Override
with `zstsink_fs_set_filename_handler()` before starting the pipeline.

### Test nodes (`zstsrc-numgen`, `zstsink-fake`)

Software-only nodes for testing. `zstsrc-numgen` fills buffers with
sequential bytes 0-255. `zstsink-fake` discards all received data. No
hardware dependencies; suitable for `native_sim` and unit tests.

## Kconfig reference

| Symbol | Description | Depends on |
|--------|------------|-----------|
| `CONFIG_ZSTREAMER` | Enable the framework | `NET_BUF` (auto-selected) |
| `CONFIG_ZSTNODE` | Enable the driver subsystem | `ZSTREAMER` |
| `CONFIG_ZSTNODE_UART_SRC` | UART source driver | `DT_HAS_ZSTREAMER_ZSTSRC_UART_ENABLED`, `SERIAL` |
| `CONFIG_ZSTNODE_UART_SINK` | UART sink driver | `DT_HAS_ZSTREAMER_ZSTSINK_UART_ENABLED`, `SERIAL` |
| `CONFIG_ZSTNODE_UART_DMA` | UART DMA/async support | `UART_ASYNC_API` |
| `CONFIG_ZSTNODE_SPI_SRC` | SPI source driver | `DT_HAS_ZSTREAMER_ZSTSRC_SPI_ENABLED`, `SPI` |
| `CONFIG_ZSTNODE_SPI_SINK` | SPI sink driver | `DT_HAS_ZSTREAMER_ZSTSINK_SPI_ENABLED`, `SPI` |
| `CONFIG_ZSTNODE_ADC_SRC` | ADC source driver | `DT_HAS_ZSTREAMER_ZSTSRC_ADC_ENABLED`, `ADC` |
| `CONFIG_ZSTNODE_ADC_STM32` | STM32 ADC backend | `SOC_FAMILY_STM32`, `DMA` |
| `CONFIG_ZSTNODE_FS_SINK` | File system sink driver | `DT_HAS_ZSTREAMER_ZSTSINK_FS_ENABLED`, `FILE_SYSTEM` |
| `CONFIG_ZSTNODE_TEST_NUMGEN_SRC` | Number generator source | `DT_HAS_ZSTREAMER_ZSTSRC_NUMGEN_ENABLED` |
| `CONFIG_ZSTNODE_TEST_FAKESINK` | Fake sink | `DT_HAS_ZSTREAMER_ZSTSINK_FAKE_ENABLED` |

All driver Kconfig symbols default to `y` when the corresponding DTS
compatible is present (`DT_HAS_*_ENABLED`), following the Zephyr convention
for auto-enabling drivers based on devicetree.

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
├── CMakeLists.txt                          # Top-level: adds lib/ and drivers/
├── Kconfig                                 # Top-level: CONFIG_ZSTREAMER menuconfig
├── zephyr/module.yml                       # Zephyr module descriptor
├── west.yml                                # Standalone west manifest
├── include/
│   ├── zstreamer/
│   │   └── zstreamer.h                     # Framework API (start/stop/submit/alloc)
│   └── zephyr/drivers/
│       ├── zstnode.h                       # Driver API, common structs, DT macros
│       └── zstsink_fs.h                    # FS sink public API (filename callback)
├── lib/zstreamer/
│   ├── zstreamer_graph.c                   # Graph device: NET_BUF_POOL_FIXED per instance
│   └── zstreamer_node.c                    # Node lifecycle, threads, buffer routing
├── dts/bindings/streaming/
│   ├── zstnode.yaml                        # Base: children phandle array
│   ├── zstreamer,graph.yaml                # Graph: buffer-count, buffer-size
│   ├── zstreamer,zstsrc.yaml               # Source base: thread-stack-size, priority
│   ├── zstreamer,zstsink.yaml              # Sink base: thread-stack-size, priority
│   ├── zstreamer,zstsrc-uart.yaml          # UART source
│   ├── zstreamer,zstsink-uart.yaml         # UART sink
│   ├── zstreamer,zstsrc-spi.yaml           # SPI source
│   ├── zstreamer,zstsink-spi.yaml          # SPI sink
│   ├── zstreamer,zstsrc-adc.yaml           # ADC source (timer-triggered DMA)
│   ├── zstreamer,zstsink-fs.yaml           # File system sink
│   ├── zstreamer,zstsrc-numgen.yaml        # Test: number generator
│   └── zstreamer,zstsink-fake.yaml         # Test: /dev/null sink
├── drivers/zstnode/
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
    ├── drivers/zstnode/                    # UART node ztest suite
    └── drivers/zstnode_spi/                # SPI node ztest suite
```

## License

Apache-2.0
