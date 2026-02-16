# zstreamer

A graph-based streaming pipeline framework for [Zephyr RTOS](https://zephyrproject.org/).
Nodes are Zephyr devices defined in devicetree and connected with `children` phandles.
Each graph owns a shared `net_buf` pool used by all nodes in that graph.

## Current architecture

- `zstreamer,graph` is a container device that owns a `NET_BUF_POOL_FIXED` pool.
- Each `zstreamer,node` has:
  - a `k_fifo` input queue,
  - one thread created at boot,
  - optional `open`/`close` callbacks,
  - either `generate` (source) or `process` (non-source).
- Source nodes allocate/fill buffers and fan out to children.
- Non-source nodes consume from FIFO, process, then fan out.

## Node model

### Source node

- Implements `generate(dev, buf)`.
- Can be started/stopped through:
  - `zstreamer_node_start()`
  - `zstreamer_node_stop()`
- `start/stop` only apply to sources.

### Non-source node

- Implements `process(dev, buf)`.
- `zstreamer_node_start/stop()` return `-ENOTSUP`.
- `open()` is called during init for non-source nodes.

### Lifecycle contract

For a source node:

- first `zstreamer_node_start()` => `0`
- second `zstreamer_node_start()` => `-EALREADY`
- first `zstreamer_node_stop()` => `0`
- second `zstreamer_node_stop()` => `-EALREADY`

Tests should keep this strict contract.

## Public API

Declared in `include/zstreamer/node.h`:

```c
int zstreamer_node_start(const struct device *dev);
int zstreamer_node_stop(const struct device *dev);
struct net_buf *zstreamer_node_alloc_buf(const struct device *dev,
                                         k_timeout_t timeout);
```

Driver callbacks:

```c
__subsystem struct zstreamer_node_driver_api {
    int (*open)(const struct device *dev);
    int (*close)(const struct device *dev);
    int (*generate)(const struct device *dev, struct net_buf *buf);
    int (*process)(const struct device *dev, struct net_buf *buf);
};
```

## Devicetree conventions

### Binding hierarchy

- `zstreamer,node.yaml` (base node properties)
- `zstreamer,src.yaml` (source base)
- `zstreamer,sink.yaml` (non-source/sink base)
- Driver-specific bindings, e.g.:
  - `zstreamer,uart-src.yaml`
  - `zstreamer,uart-sink.yaml`
  - `zstreamer,spi-src.yaml`
  - `zstreamer,spi-sink.yaml`

### Important DTS rules

- Do not duplicate bus properties on zstreamer nodes.
  - UART baud, SPI frequency/mode, etc. stay on the referenced bus device.
- zstreamer nodes belong under a `zstreamer,graph` container node.
- Keep vendor registration present for custom compatibles:
  - `dts/bindings/vendor-prefixes.txt` must include `zstreamer`.

## Writing a new driver

1. Add a binding under `dts/bindings/zstreamer/`.
2. Define config/data structs with `common` as first member.
3. Implement either:
   - `generate` for a source, or
   - `process` for a non-source.
4. Instantiate with `DEVICE_DT_INST_DEFINE(...)` using zstreamer node helper macros.
5. Add Kconfig and CMake entries under `drivers/zstreamer/`.

## Included drivers

- UART: `drivers/zstreamer/uart/`
- SPI: `drivers/zstreamer/spi/`
- ADC source: `drivers/zstreamer/adc/`
- FS sink: `drivers/zstreamer/fs/`
- Test nodes (`numgen-src`, `fake-sink`): `drivers/zstreamer/test/`

## Tests

- UART/node tests: `tests/drivers/node`
- SPI/node tests: `tests/drivers/node_spi`

On macOS, use Docker for `native_sim/native/64` execution.

Known status:

- SPI suite is passing in Docker.
- UART suite currently has a hang in `test_numgen_fakesink_restart_cycle` and needs further fix.

## Build examples

```sh
# Sample build
west build -b nucleo_u575zi_q samples/uart2uart

# Driver tests
west build -b native_sim/native/64 tests/drivers/node_spi
west build -b native_sim/native/64 tests/drivers/node
```

## Module integration

`zephyr/module.yml` points `dts_root: .`, so bindings under `dts/bindings/` are discovered automatically.

## License

Apache-2.0
