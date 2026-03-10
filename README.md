# ZStreamer

A streaming pipeline framework for [Zephyr RTOS](https://zephyrproject.org/),
inspired by [GStreamer](https://gstreamer.freedesktop.org/).
Define data pipelines entirely in devicetree — wire nodes with phandles,
and data flows automatically through shared `net_buf` pools. No application code required.

## How it works

```mermaid
graph LR
    SRC[Source] -->|buf| PROC[Processor]
    PROC -->|buf| FILT[Filter]
    FILT -->|"true"| SINK1[Sink A]
    FILT -->|"false"| SINK2[Sink B]
```

Every node is a Zephyr device with its own **thread** and **k_fifo** input queue.
A `streaming-graph` container owns a single `net_buf_pool` shared by all its nodes.

**Data flow:** Sources allocate buffers from the pool, fill them via a `process` callback,
and push refs into their children's FIFOs. Each downstream thread blocks on its FIFO,
processes the buffer, and either forwards to its own children or unrefs it (sinks).

**Copy-on-write distribution:** Readonly children share a `net_buf_ref`;
readwrite children get a `net_buf_clone`. When the last consumer unrefs,
the buffer returns to the pool.

**Four node types:**

| Type | What it does | Example |
|------|-------------|---------|
| **Source** | Produces buffers, start/stop via semaphores | `uart_src`, `spi_src`, `adc_src` |
| **Processor** | Transforms buffers in-place, forwards to children | `passthrough_node` |
| **Filter** | Routes to `children` (returns 1) or `false-children` (returns 0) | `odd_filter` |
| **Sink** | Terminal consumer, unrefs buffers (no children allowed) | `fs_sink`, `pwm_sink`, `uart_sink` |

## Example: DTS pipeline

```dts
/ {
    streaming-graph {
        compatible = "zstreamer,graph";
        buffer-count = <8>;
        buffer-size = <1>;

        sine_source: sine-source {
            compatible = "zstreamer,sine-src";
            cycle-ms = <4000>;
            children = <&pwm_sink>;
            autostart;
        };

        pwm_sink: pwm-sink {
            compatible = "zstreamer,pwm-sink";
            pwms = <&pwm2 2 PWM_MSEC(1) PWM_POLARITY_NORMAL>;
        };
    };
};
```

That's it. A 4-second sine wave drives a PWM LED at boot. No C code needed.

## Writing a driver

Every driver follows the same pattern:

```c
#define DT_DRV_COMPAT zstreamer_my_sink
#include <zstreamer/sink.h>

static int my_sink_process(const struct device *dev, struct net_buf *buf) {
    /* consume buf */
    return 0;
}

static const struct zstreamer_node_driver_api my_sink_api = {
    .process = my_sink_process,
};

#define MY_SINK_DEFINE(inst)                                          \
  ZSTREAMER_SINK_DT_INST_PRE_DEFINE(inst);                            \
  static struct zstreamer_sink_data my_sink_data_##inst =             \
      ZSTREAMER_SINK_DATA_INIT(inst);                                \
  static const struct zstreamer_sink_config my_sink_config_##inst =   \
      ZSTREAMER_SINK_CONFIG_INIT(inst);                              \
  DEVICE_DT_INST_DEFINE(inst, zstreamer_sink_common_init, NULL,       \
                        &my_sink_data_##inst, &my_sink_config_##inst,  \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
                        &my_sink_api);

DT_INST_FOREACH_STATUS_OKAY(MY_SINK_DEFINE)
```

**Key rules:**
- Call `PRE_DEFINE(inst)` **before** data/config structs
- Each node type has its own `common_init`: sources use `zstreamer_source_common_init`, sinks use `zstreamer_sink_common_init`, filters use `zstreamer_filter_common_init`
- Sources support `autostart` DTS property for boot-time start
- For custom init, call `common_init` last (it starts the node thread)
- Thread stack is **fixed** at 2048 bytes — large data belongs in the `net_buf` or the driver's data struct, not on the stack

## Building & testing

```sh
# Run all tests across all suites
west twister -T tests -p native_sim --inline-logs

# Build a single test
west build -b native_sim -d build/test-node tests/subsys/node -p auto
timeout 120s ./build/test-node/zephyr/zephyr.exe

# Build a sample for hardware
west build -b nucleo_u575zi_q samples/uart2uart
```

## Roadmap

### Subsystem

- [ ] **Buffer metadata** — standardized metadata header on `net_buf` (timestamps, sequence numbers, sample rate)
- [ ] **Buffer typing** — typed buffers (audio, raw, encoded, …) with type-negotiation between nodes, allowing drivers to register per-type `process` callbacks (preferably at compile-time)
- [ ] **Runtime settings** — configure driver parameters (e.g. LoRa frequency, ADC sample rate) at runtime via Zephyr's `settings` subsystem, persisted across reboots
- [ ] **Pipeline analyzer** — thread stack usage, buffer pool utilization, per-node throughput counters, and shell commands for live pipeline inspection

### Drivers

- [x] **UART source/sink** — serial data streaming
- [x] **SPI source/sink** — SPI bus data relay
- [x] **ADC source** — analog-to-digital capture
- [x] **FS sink** — write pipeline data to filesystem
- [x] **PWM sink** — drive PWM duty cycle from pipeline data
- [x] **DAC sink** — analog output (`sine_src -> dac_sink` = function generator)
- [x] **LoRa source/sink** — wireless data over LoRa radio
- [ ] **I2S source/sink** — audio streaming (mic-to-speaker, recording, DSP)
- [ ] **BLE GATT sink/source** — wireless data over BLE notifications
- [ ] **Sensor source** — wraps Zephyr sensor API (works with hundreds of existing drivers)
- [ ] **USB CDC ACM** — USB serial streaming / data acquisition
- [ ] **CAN bus** — automotive/industrial logging
- [ ] **TCP/UDP sockets** — network telemetry
- [ ] **Threshold filter** — pass/block on configurable value
- [ ] **Chunker processor** — buffer size adaptation between protocols
- [ ] **Shell sink** — dump pipeline data to Zephyr shell for debugging

## Contact

Sharon Naim :maple_leaf: — sharonthecreator@gmail.com
