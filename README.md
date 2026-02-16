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
implementation, driver API struct, and DTS binding. All share a common base (`node.h`).

| Type | Header | Callback | Has children | Start/Stop |
|------|--------|----------|-------------|------------|
| **Source** | `source.h` | `generate(dev, buf)` | Yes | `zstreamer_source_start/stop()` |
| **Sink** | `sink.h` | `process(dev, buf)` | No (enforced at compile time) | N/A (always running) |
| **Processor** | `processor.h` | `process(dev, buf)` | Yes | N/A (always running) |
| **Filter** | `filter.h` | `filter(dev, buf)` → `bool` | Yes + `false_children` | N/A (always running) |

- **Source**: Allocates buffers, fills them via `generate`, and fans out to children.
  Only sources can be started/stopped at runtime.
- **Sink**: Terminal node. Consumes buffers from its FIFO and unrefs them. The sink
  config struct has no `children` field — attempting to add children is a compile error.
- **Processor**: Receives buffers, processes them in-place, and forwards to children.
- **Filter**: Receives buffers, runs a boolean `filter` callback. If true, distributes
  to `children`; if false, distributes to `false_children`.

All non-source nodes call `open()` during init and run their thread immediately at boot.

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

### Driver API structs

Each node type defines its own `__subsystem` driver API:

```c
/* Source */
__subsystem struct zstreamer_source_driver_api {
    int (*open)(const struct device *dev);
    int (*close)(const struct device *dev);
    int (*generate)(const struct device *dev, struct net_buf *buf);
};

/* Sink */
__subsystem struct zstreamer_sink_driver_api {
    int (*open)(const struct device *dev);
    int (*close)(const struct device *dev);
    int (*process)(const struct device *dev, struct net_buf *buf);
};

/* Processor */
__subsystem struct zstreamer_processor_driver_api {
    int (*open)(const struct device *dev);
    int (*close)(const struct device *dev);
    int (*process)(const struct device *dev, struct net_buf *buf);
};

/* Filter */
__subsystem struct zstreamer_filter_driver_api {
    int (*open)(const struct device *dev);
    int (*close)(const struct device *dev);
    bool (*filter)(const struct device *dev, struct net_buf *buf);
};
```

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

### Source driver

1. Add a binding under `dts/bindings/zstreamer/` that includes `zstreamer,src.yaml`.
2. Define config/data structs with the type-specific common struct as first member:
   ```c
   struct my_src_config {
       struct zstreamer_source_config common;
       /* driver-specific fields */
   };
   struct my_src_data {
       struct zstreamer_source_data common;
       /* driver-specific fields */
   };
   ```
3. Implement `generate(dev, buf)` (and optionally `open`/`close`).
4. Instantiate with `ZSTREAMER_SOURCE_DT_INST_DEFINE(...)`.

### Sink driver

1. Binding includes `zstreamer,sink.yaml`.
2. Config/data use `zstreamer_sink_config` / `zstreamer_sink_data`.
3. Implement `process(dev, buf)`.
4. Instantiate with `ZSTREAMER_SINK_DT_INST_DEFINE(...)`.

### Processor driver

1. Binding includes `zstreamer,processor.yaml`.
2. Config/data use `zstreamer_processor_config` / `zstreamer_processor_data`.
3. Implement `process(dev, buf)` — data is forwarded to children after processing.
4. Instantiate with `ZSTREAMER_PROCESSOR_DT_INST_DEFINE(...)`.

### Filter driver

1. Binding includes `zstreamer,filter.yaml`.
2. Config/data use `zstreamer_filter_config` / `zstreamer_filter_data`.
3. Implement `filter(dev, buf)` returning `true` (→ children) or `false` (→ false_children).
4. Instantiate with `ZSTREAMER_FILTER_DT_INST_DEFINE(...)`.

## Included drivers

| Driver | Type | Location |
|--------|------|----------|
| UART source | Source | `drivers/zstreamer/uart/src_uart.c` |
| UART sink | Sink | `drivers/zstreamer/uart/sink_uart.c` |
| SPI source | Source | `drivers/zstreamer/spi/src_spi.c` |
| SPI sink | Sink | `drivers/zstreamer/spi/sink_spi.c` |
| ADC source | Source | `drivers/zstreamer/adc/src_adc.c` |
| FS sink | Sink | `drivers/zstreamer/fs/sink_fs.c` |
| Numgen source (test) | Source | `drivers/zstreamer/test/src_numgen.c` |
| Fake sink (test) | Sink | `drivers/zstreamer/test/sink_fake.c` |

## Samples

| Sample | Description |
|--------|-------------|
| `samples/uart2uart` | UART RX → UART TX pipeline |
| `samples/spi2spi` | SPI RX → SPI TX pipeline |
| `samples/adc2fakesink` | ADC capture → fake sink (logging) |
| `samples/numgen2fakesink` | Number generator → fake sink (logging) |

## Tests

| Suite | Location | Notes |
|-------|----------|-------|
| Node/UART | `tests/drivers/node` | Has known hang in `test_numgen_fakesink_restart_cycle` |
| Node/SPI | `tests/drivers/node_spi` | Passing on native_sim/native/64 |

On macOS, use Docker (`zephyrprojectrtos/zephyr-build`) for `native_sim/native/64` builds.

## Build examples

```sh
# Sample build (hardware target)
west build -b nucleo_u575zi_q samples/uart2uart

# Test builds (native_sim)
west build -b native_sim/native/64 tests/drivers/node_spi
west build -b native_sim/native/64 tests/drivers/node
```

## Module integration

`zephyr/module.yml` points `dts_root: .`, so bindings under `dts/bindings/` are
discovered automatically by Zephyr's build system.

## License

Apache-2.0
