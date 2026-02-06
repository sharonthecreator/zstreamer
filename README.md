# zstreamer

Graph-based streaming pipeline framework for [Zephyr RTOS](https://zephyrproject.org/).

zstreamer lets you build data pipelines from reusable **node** drivers wired
together in the devicetree. Each graph owns a shared `net_buf` pool; buffers
flow from source nodes through optional processing stages to sink nodes.

## Concepts

| Term    | Description |
|---------|-------------|
| Graph   | Container device that owns the `net_buf` pool. |
| Source  | Threaded node that produces buffers (`run` callback). |
| Sink    | Threaded node that consumes buffers (`process` callback). |
| Generic | Workqueue node that transforms buffers (`process` callback). |

## Repository layout

```
├── include/
│   ├── zstreamer/zstreamer.h        # Framework API
│   └── zephyr/drivers/zstnode.h     # Driver API & definition macros
├── lib/zstreamer/                   # Core: graph pool, node lifecycle
├── drivers/zstnode/                 # Node driver implementations
│   └── uart/                        # UART source & sink
├── dts/bindings/streaming/          # Devicetree bindings
├── samples/
│   └── uart2uart/                   # UART-to-UART relay sample
└── tests/
    └── drivers/zstnode/             # Ztest suite
```

## Quick start

Add zstreamer as a Zephyr module (west manifest or `ZEPHYR_EXTRA_MODULES`),
then enable it in your `prj.conf`:

```conf
CONFIG_ZSTREAMER=y
CONFIG_ZSTNODE=y
CONFIG_SERIAL=y
```

Define a graph in your devicetree overlay:

```dts
/ {
    streaming-graph {
        compatible = "zstreamer,graph";
        #address-cells = <1>;
        #size-cells = <0>;

        source: uart-source@0 {
            compatible = "zstreamer,zstsrc-uart";
            reg = <0>;
            uart-device = <&usart2>;
            children = <&sink>;
        };

        sink: uart-sink@1 {
            compatible = "zstreamer,zstsink-uart";
            reg = <1>;
            uart-device = <&usart3>;
        };
    };
};
```

Start the pipeline from your application:

```c
#include <zstreamer/zstreamer.h>

const struct device *src  = DEVICE_DT_GET(DT_NODELABEL(source));
const struct device *sink = DEVICE_DT_GET(DT_NODELABEL(sink));

zstreamer_start(sink);
zstreamer_start(src);
```

## Writing a new node driver

1. Create a devicetree binding that includes `zstnode.yaml` (or one of
   `zstreamer,zstsrc.yaml` / `zstreamer,zstsink.yaml`).
2. Implement the `zstnode_driver_api` callbacks.
3. Use `Z_ZSTNODE_CHILDREN_DEFINE`, `Z_ZSTNODE_COMMON_CONFIG_INIT`,
   `Z_ZSTNODE_COMMON_DATA_INIT`, and `Z_ZSTNODE_INIT_WRAPPER_DEFINE`
   to register the device.

See `drivers/zstnode/uart/` for a reference implementation.

## Building the sample

```sh
west build -b nucleo_u575zi_q samples/uart2uart
west flash
```

## Running tests

```sh
west twister -T tests/
```

## License

Apache-2.0
