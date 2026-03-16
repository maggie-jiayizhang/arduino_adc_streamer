# Arduino Sketches

This folder contains firmware sketches for different microcontroller units (MCUs) with various sensor configurations. All sketches communicate with the Python ADC streamer program through a serial connection using a unified API.

## Available Sketches

| MCU | Sketch | Purpose |
|-----|--------|---------|
| Teensy 4.X | `Teensy555_streamer.ino` | Measure resistance using 555 timer circuit through ADG-706 MUX |
| Teensy 4.X | `ADC_Streamer_binary_scan2.ino` | Read analog data using internal Teensy ADC inputs |
| MG-24 | `ADC_Streamer_binary_scan.ino` | Read analog data using internal MG-24 ADC inputs |
| MG-24 | `ADC_Streamer_binary_scan_with_ADG1206_mux.ino` | Read analog data through external ADG1206 multiplexer with optional charge-amp reset control |

## Legacy Sketches

Other sketch files in this directory represent older versions and are no longer actively maintained or used. Refer to the table above for current recommended sketches for your MCU.

---

## Serial API

This firmware exposes the same serial command API and binary streaming format on both:

- **Seeed XIAO MG24** (`mcu*` returns `# MG24`)
- **Teensy 4.1** (`mcu*` returns `# Teensy40`)

A host application can talk to either MCU using the same command set and the same binary parser.

### Serial settings

| Setting | Value |
|---------|-------|
| Baud rate | 460800 |
| Transport | USB serial |
| Command terminator | `*` |
| Text responses | ASCII |
| Binary blocks | Raw bytes on the same serial port |

---

### Command protocol

#### General rules

- Commands are ASCII text terminated by `*`
- Commands are case-insensitive
- Arguments are separated by spaces
- Comma-separated lists are supported where noted

**Example:**

```
channels 14,15,16,17,18*
repeat 20*
buffer 10*
run*
```

#### Command acknowledgment

Each command ends with one acknowledgment line.

Success:
```
#OK
```
or
```
#OK <args>
```

Failure:
```
#NOT_OK
```
or
```
#NOT_OK <args>
```

Additional informational or error lines may appear before the final `#OK` or `#NOT_OK`.

---

### Commands

#### `channels <pin_list>*`

Defines the channel acquisition sequence.

- `<pin_list>` is a comma-separated list of analog input pins
- Repeated pins are allowed
- Maximum sequence length: up to 16 entries

**Examples:**
```
channels 14,15,16,17,18*
channels 14,14,15,16*
```

The channel list defines the order of acquisition inside each sweep. Repeated channels are sampled again in sequence.

---

#### `ground <pin>*`

Sets the analog pin used as the ground/dummy sample channel and enables dummy ground insertion.

**Example:**
```
ground 19*
```

Sets the ground pin and enables `useGroundBeforeEach = true`.

#### `ground true*` / `ground false*`

Enables or disables insertion of a dummy ground sample before each new channel.

**Examples:**
```
ground true*
ground false*
```

> Ground samples are used internally only. They are **not** included in the binary sample payload. They affect acquisition timing but not the number of transmitted samples.

---

#### `repeat <n>*`

Sets the number of samples acquired per channel per sweep.

**Example:**
```
repeat 20*
```

If `channels 14,15,16*` and `repeat 20*`, one sweep produces 20 samples from each pin (60 total). Total samples sent per sweep = `channelCount × repeat`.

---

#### `buffer <n>*`

Sets the number of sweeps grouped into one binary block.

**Example:**
```
buffer 10*
```

Total binary samples per block = `samplesPerSweep × sweepsPerBlock`. Use `buffer 1` for lower latency, or `buffer > 1` for higher throughput.

---

#### `ref 1.2*` / `ref 3.3*` / `ref vdd*`

Sets ADC reference mode.

**Examples:**
```
ref 3.3*
ref vdd*
```

> - On MG24, `ref 1.2` may be supported depending on the sketch
> - On Teensy 4.1, `ref 1.2` is **not supported** and returns an error
> - On Teensy 4.1, `ref 3.3` and `ref vdd` are accepted and map to 3.3 V ADC behavior

---

#### `osr 2*` / `osr 4*` / `osr 8*`

Sets oversampling or averaging mode.

**Examples:**
```
osr 2*
osr 4*
osr 8*
```

> - On MG24, this corresponds to hardware oversampling behavior
> - On Teensy 4.1, this is mapped to ADC hardware averaging
> - Accepted values: `2`, `4`, `8`

---

#### `gain 1*` / `gain 2*` / `gain 3*` / `gain 4*`

Sets ADC gain mode.

**Examples:**
```
gain 1*
gain 2*
```

> - On MG24, this may control real ADC analog gain
> - On Teensy 4.1, this command is accepted for API compatibility and reflected in status, but does **not** change actual hardware gain
> - Accepted values: `1`, `2`, `3`, `4`

---

#### `run*`

Starts continuous streaming.

```
run*
```

Acquisition starts immediately. Binary blocks are continuously written to the serial port until `stop*` is received.

#### `run <ms>*`

Starts time-limited streaming.

```
run 100*
```

Starts acquisition and stops automatically after approximately `<ms>` milliseconds.

---

#### `stop*`

Stops acquisition.

```
stop*
```

---

#### `status*`

Prints the current configuration in human-readable text. Typical fields include: running state, timed run state, channel list, repeat count, ground pin, ground insertion enable, ADC reference, OSR setting, gain setting, samples per sweep, scan entries per sweep, sweeps per block, and buffer limits.

> This output is intended for debugging and UI inspection, not strict machine parsing.

---

#### `mcu*`

Prints the MCU identifier used by the host to distinguish implementations.

```
mcu*
```

Returns one of:
```
# MG24
```
or
```
# Teensy4.1
```

Host software should use this command to identify the connected device.

---

#### `help*`

Prints command help.

```
help*
```

---

### Binary streaming format

When streaming is active, the device writes binary blocks to the same serial port. Each block has the following layout:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 byte | Magic `0xAA` |
| 1 | 1 byte | Magic `0x55` |
| 2–3 | uint16 LE | `sample_count` |
| 4 … 4+(sample_count×2)−1 | uint16 LE × sample_count | Sample payload |
| next | uint16 LE | `avg_dt_us` |
| next | uint32 LE | `block_start_us` |
| next | uint32 LE | `block_end_us` |

#### Field definitions

**Magic bytes** — `0xAA 0x55` — two-byte block marker used by the host to find frame boundaries.

**`sample_count`** — unsigned 16-bit integer, little-endian. Number of ADC samples in the block. Does not include dummy ground samples.

**Sample payload** — `sample_count` samples, each an unsigned 16-bit little-endian value. Samples are ordered exactly as acquired.

**`avg_dt_us`** — unsigned 16-bit integer, little-endian. Average time per transmitted sample in microseconds, computed over the whole block.

**`block_start_us`** — unsigned 32-bit integer, little-endian. Timestamp at start of block capture in microseconds (`micros()`).

**`block_end_us`** — unsigned 32-bit integer, little-endian. Timestamp at end of block capture in microseconds (`micros()`).

> Timestamps are 32-bit microsecond counters that wrap around naturally. The host should treat them as unsigned wrap-safe counters.

---

### Sample ordering

For each sweep, channels are visited in the configured `channels` order. For each channel, `repeat` samples are taken. If `ground true*` is enabled, a dummy ground read is taken before each new channel (not transmitted).

**Example — `channels 14,15,16*`, `repeat 2*`, `buffer 1*`, `ground false*`:**

```
ch14 s1, ch14 s2, ch15 s1, ch15 s2, ch16 s1, ch16 s2
```

**Example — `channels 14,14,15*`, `repeat 2*`:**

One sweep becomes:
```
ch14 s1, ch14 s2, ch14 s3, ch14 s4, ch15 s1, ch15 s2
```

The host should interpret sample order based on the configured channel sequence and repeat count.

---

### Host integration guidelines

#### Device detection

Recommended startup sequence:

1. Open serial port
2. Send `mcu*`
3. Read text response
4. Detect either `# MG24` or `# Teensy4.1`

#### Common parser

The binary block format is the same for both devices, so the same parser can be reused.

#### Mixed text and binary on one port

The serial link carries both ASCII command/status output and binary streaming blocks. Recommended approach:

- Use text mode while configuring the device
- After `run*`, switch receiver logic to binary frame parsing
- After `stop*`, expect text output again

#### Error handling

Invalid commands or arguments produce text error lines beginning with `#`, followed by a final `#NOT_OK`. Host software should rely on the final `#OK` / `#NOT_OK` line, not on exact free-form error text.

**Examples:**
```
# ERROR: unknown command 'foo'. Type 'help'.
#NOT_OK
```
```
# ERROR: channel out of range (0-255)
#NOT_OK 300
```

---

### Teensy 4.X compatibility notes

The Teensy 4.X sketch preserves the MG24 API, but some commands are compatibility mappings rather than identical hardware behavior:

| Feature | Teensy 4.1 behavior |
|---------|---------------------|
| `mcu*` | Returns `# Teensy40` |
| `ref 1.2*` | Not supported — returns error |
| `osr` | Maps to ADC hardware averaging |
| `gain` | Accepted for API compatibility and status reporting; does not change actual ADC gain |
