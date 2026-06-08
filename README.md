# snd_mcp49x1_gpio

ALSA PCM playback driver for Microchip MCP4901, MCP4911 and MCP4921 DACs connected to Luckfox Pico Mini A/B by GPIO bit-bang.

This driver provides a normal ALSA playback device for simple sound output on Luckfox Pico Mini A/B.  It was designed for the smallest possible external circuit: one MCP49x1 DAC, one AC-coupling capacitor and one high-impedance speaker.  The goal is not Hi-Fi quality, but a practical, very small audio output for beeps, notifications, simple effects and lightweight playback.

Luckfox Pico Mini A/B does not provide a native analog audio output.  An MCP49x1 DAC is used as a minimal external DAC.  The driver receives normal ALSA PCM samples, converts them to the selected DAC resolution and sends a 16-bit MCP49x1 command frame by GPIO bit-bang.

This is intentionally a simple PCM driver.  It is not ASoC, not IIO and not a general-purpose SPI driver.

## Hardware used

Tested target:
- Luckfox Pico Mini A/B / RV1103
- MCP4901-E/SN, MCP4911-E/SN or MCP4921-E/SN, SOIC-8 package
- 220 uF electrolytic capacitor in series with the speaker
- 150 ohm speaker

The design assumes 3.3 V logic and 3.3 V DAC supply.

## Why MCP49x1

The MCP49x1 family was selected because it is small, inexpensive and simple to drive:
- MCP4901: 8-bit voltage-output DAC
- MCP4911: 10-bit voltage-output DAC
- MCP4921: 12-bit voltage-output DAC
- SPI-like serial input
- works from 2.7 V to 5.5 V supply
- accepts 3.3 V logic directly
- no extra microcontroller or audio codec is required

The trade-off is still the minimal analog circuit.  A 10-bit or 12-bit DAC can reduce quantization noise and improve quiet passages, but the output still has no real analog reconstruction filter or amplifier in this minimal circuit.

## MCP49x1-E/SN pinout

Top view of the SOIC-8 package.  Pin 1 is next to the notch / dot mark.

```text
             MCP49x1-E/SN
          top view, SOIC-8

              ┌───────┐
      VDD  1  │●      │  8  VOUT
      CS   2  │       │  7  VSS / GND
      SCK  3  │       │  6  VREF
      SDI  4  │       │  5  LDAC
              └───────┘
```

Pin meaning:

| MCP49x1 pin | Name | Function in this project |
|---|---|---|
| 1 | `VDD` | 3.3 V power |
| 2 | `CS` | Chip select from Luckfox GPIO52 |
| 3 | `SCK` | Serial clock from Luckfox GPIO42 |
| 4 | `SDI` | Serial data from Luckfox GPIO43 |
| 5 | `LDAC` | Tie to GND for immediate DAC update |
| 6 | `VREF` | Tie to 3.3 V |
| 7 | `VSS` | Ground |
| 8 | `VOUT` | Analog DAC output through 220 uF capacitor to speaker |

## Wiring

### Full wiring

**Luckfox Pico Mini A/B ↔ MCP49x1-E/SN ↔ speaker**

| Luckfox Pico Mini A/B | MCP49x1-E/SN | Function |
|---|---|---|
| `3V3` | pin 1 `VDD` | DAC power |
| `3V3` | pin 6 `VREF` | DAC reference voltage |
| `GND` | pin 7 `VSS` | Ground |
| `GND` | pin 5 `LDAC` | Immediate DAC update |
| GPIO52 / `GPIO1_C4` | pin 2 `CS` | DAC chip select |
| GPIO42 / `GPIO1_B2` | pin 3 `SCK` | DAC serial clock |
| GPIO43 / `GPIO1_B3` | pin 4 `SDI` | DAC serial data |
| — | pin 8 `VOUT` | DAC analog output |

Speaker connection:

| Part | Connection |
|---|---|
| 220 uF capacitor `+` | MCP49x1 pin 8 `VOUT` |
| 220 uF capacitor `-` | speaker `+` side |
| speaker other side | GND |

Recommended additional component:

| Part | Connection | Purpose |
|---|---|---|
| 100 nF ceramic capacitor | between MCP49x1 pin 1 `VDD` and pin 7 `VSS` | local supply decoupling |

### Minimal schematic

```text
Luckfox Pico Mini A/B                  MCP49x1-E/SN
────────────────────                  ─────────────

3V3  ────────────────────────────────  1 VDD
                                      │
                                      ├── 100 nF ──┐
                                      │            │
3V3  ────────────────────────────────  6 VREF      │
GND  ────────────────────────────────  7 VSS  ─────┘
GND  ────────────────────────────────  5 LDAC

GPIO52 / GPIO1_C4  ─────────────────  2 CS
GPIO42 / GPIO1_B2  ─────────────────  3 SCK
GPIO43 / GPIO1_B3  ─────────────────  4 SDI

                                      8 VOUT ── +│ 220 uF │- ── speaker ── GND
```

Notes:
- `LDAC` is tied to GND so that every received DAC word updates the output immediately.
- `VREF` is tied to 3.3 V, so the DAC output range is approximately 0 V to 3.3 V.
- The 100 nF ceramic capacitor is the local supply decoupling capacitor for the MCP49x1 DAC. Place it as close as practical to pins `VDD` and `VSS`.
- The 220 uF capacitor removes the DC component from the DAC output before the speaker.
- The 220 uF capacitor polarity matters: the positive side goes to `VOUT`, the negative side goes to the speaker.

## Why the circuit is this simple

The project intentionally avoids:
- I2S DACs
- external audio amplifiers
- complex codecs
- extra power rails
- large analog filters
- additional control buses

The Luckfox Pico Mini A/B pins available in this build were limited.  Hardware SPI was not available for this audio circuit, so the driver sends the MCP49x1 serial frame by GPIO bit-bang.

The result is a very small audio output that is good enough for embedded UI sounds, alerts and simple playback.  It is not intended to replace a proper audio codec and amplifier.

## Driver architecture

Audio path:

```text
ALSA application
  ↓
ALSA PCM core
  ↓
snd_mcp49x1_gpio
  ↓
MMIO GPIO bit-bang on GPIO52/GPIO42/GPIO43
  ↓
MCP49x1 DAC
  ↓
220 uF coupling capacitor
  ↓
150 ohm speaker
```

The current baseline driver uses MMIO as the only method for driving GPIO in the time-critical playback path.  The kernel GPIO API is still used to request and configure the lines, but it is not used to generate every audio edge.  During playback, the driver writes the GPIO registers directly for the three signal lines, which gives enough timing margin for the supported sample rates.

The ALSA device accepts PCM data and converts it internally to the selected DAC resolution.  Every output sample is sent as a 16-bit MCP49x1 command word:

```text
0 0 1 1  D11 ... D0
│ │ │ │  └── DAC data field ──┘
│ │ │ └─ SHDN = 1, DAC active
│ │ └─── GA   = 1, 1x gain
│ └───── BUF  = 0
└─────── command bit for the MCP49x1 family frame
```

For shutdown / mute the driver sends a command with `SHDN=0`.

## Supported ALSA PCM parameters

Supported formats:
- `U8`
- `S16_LE`

Supported channels:
- `1` mono
- `2` stereo, mixed internally to mono

Supported sample rates:
- `8000`
- `11025`
- `16000`
- `22050`

Unsupported rates such as `32000` and `44100` were intentionally removed from the final baseline because they either required more aggressive timing or reduced audio quality on this circuit.

The driver uses ALSA rate constraints.  For example, if an application requests `22345 Hz`, ALSA chooses the nearest supported rate, normally `22050 Hz`.

## Module parameters

### `dac_bits`

Selects the DAC resolution and therefore the supported chip variant.

Values:
- `8` — MCP4901, 8-bit DAC
- `10` — MCP4911, 10-bit DAC
- `12` — MCP4921, 12-bit DAC

Default:
- `dac_bits=8`

Example commands, choose one:

| Chip | Command |
| --- | --- |
| MCP4901 | `insmod snd_mcp49x1_gpio.ko dac_bits=8` |
| MCP4911 | `insmod snd_mcp49x1_gpio.ko dac_bits=10` |
| MCP4921 | `insmod snd_mcp49x1_gpio.ko dac_bits=12` |

The chip cannot be detected automatically because this connection uses only `CS`, `SCK` and `SDI`; there is no return data line and no readable device ID.  The selected value controls how ALSA samples are scaled into the 16-bit MCP49x1 command frame:
- MCP4901 uses the upper 8 data bits.
- MCP4911 uses the upper 10 data bits.
- MCP4921 uses all 12 data bits.


### `dither`

Optional TPDF dither before the final 8-bit DAC conversion.

This affects only `dac_bits=8` / MCP4901. It is ignored for `dac_bits=10` and `dac_bits=12`.

Parameter:
- `dither=0` - off, default; previous behaviour
- `dither=1..4` - enabled, increasing amount of dither

Example:

```sh
insmod snd_mcp49x1_gpio.ko dac_bits=8 dither=1
```


### `gain_percent`

Internal gain before final conversion to the selected DAC output resolution.

Default:
- `gain_percent=100`

Example commands, choose one:

| Setting | Command |
| --- | --- |
| Default gain | `insmod snd_mcp49x1_gpio.ko gain_percent=100` |
| Higher gain | `insmod snd_mcp49x1_gpio.ko gain_percent=120` |

Notes:
- higher values make the output louder
- too much gain may increase distortion
- normal user volume should be controlled through ALSA `Master Playback Volume`

### `limiter_enable`

Enables the software limiter.

Values:
- `0` — disabled
- `1` — enabled

Default:
- `limiter_enable=1`

The limiter reduces hard clipping when gain and psychoacoustic bass enhancement increase the signal level.

### `highpass_enable`

Enables a simple high-pass filter.

Values:
- `0` — disabled
- `1` — enabled

Default:
- `highpass_enable=1`

This helps reduce very low-frequency content that a small speaker cannot reproduce well.

### `highpass_q15`

High-pass filter coefficient in Q15-like form.

Default:
- `highpass_q15=30000`

Example commands, choose one:

| Setting | Command |
| --- | --- |
| Stronger high-pass | `insmod snd_mcp49x1_gpio.ko highpass_q15=30000` |
| Milder high-pass | `insmod snd_mcp49x1_gpio.ko highpass_q15=31200` |

Lower values make the high-pass effect stronger.  Higher values make it more subtle.

### `fade_ms`

Fade-in and fade-out time in milliseconds.

Default:
- `fade_ms=24`

This reduces audible clicks at playback start and stop.

### `psycho_bass_enable`

Enables psychoacoustic bass enhancement.

Values:
- `0` — disabled
- `1` — enabled

Default:
- `psycho_bass_enable=1`

This does not create real deep bass.  It adds a controlled low-frequency enhancement that can make small speakers sound fuller.

### `psycho_bass_level`

Strength of psychoacoustic bass enhancement.

Default:
- `psycho_bass_level=60`

Typical useful range:
- `30` to `80`

### `psycho_bass_shift`

Controls the speed of the low-frequency tracking filter used by psychoacoustic bass enhancement.

Default:
- `psycho_bass_shift=5`

Meaning:
- smaller value — stronger / faster effect
- larger value — more subtle / slower effect

### `mmio_gpio1_base`

Physical base address of GPIO1 registers.

Default:
- `mmio_gpio1_base=0xff530000`

This should normally not be changed on Luckfox Pico Mini A/B / RV1103.

## ALSA mixer control

The driver exposes:

```text
Master Playback Volume
```

Example:
```sh
amixer -c 1 set Master 80%
```

The mixer volume is intended for normal user volume control.  `gain_percent` is more of a calibration parameter.

## Building

```sh
make clean; make
```

The output module is:

```text
snd_mcp49x1_gpio.ko
```

## Loading

```sh
insmod ./snd_mcp49x1_gpio.ko
```

Check ALSA cards:

```sh
cat /proc/asound/cards
aplay -l
```

Example expected card:

```text
1 [Audio          ]: MCP49x1GPIO - MCP49x1 GPIO Audio
                      MCP49x1 GPIO bit-bang PCM
```

## Parameter status helper

The repository includes a small shell helper that displays the useful module parameters once and exits. It reads values directly from:

```text
/sys/module/snd_mcp49x1_gpio/parameters/
```

Example output:

```text
MCP49x1 ALSA driver parameters

┌────────────────────────────────────┐
│ gpio_cs            : 52            │
│ gpio_sck           : 42            │
│ gpio_sdi           : 43            │
│ dac_bits           : 8             │
│ dither             : 0             │
│ gain_percent       : 100           │
│ limiter_enable     : 1             │
│ highpass_enable    : 1             │
│ highpass_q15       : 30000         │
│ fade_ms            : 24            │
│ psycho_bass_enable : 1             │
│ psycho_bass_level  : 60            │
│ psycho_bass_shift  : 5             │
│ mmio_gpio1_base    : 0xff530000    │
└────────────────────────────────────┘
```

Run it:

```sh
chmod +x sndstat
./sndstat
```

## Playback examples

### Native ALSA playback

```sh
aplay -D hw:1,0 audio.wav
```

This requires the WAV file to match one of the supported formats and rates.

### Playback through ALSA plug

```sh
aplay -D plughw:1,0 audio.wav
```

This allows ALSA userspace to convert format, channel count or rate when available in the root filesystem.

### MP3 playback with mpg123

```sh
mpg123 -a hw:1,0 -r 22050 -m -e s16 file.mp3
```

Meaning:
- `-a hw:1,0` — use this ALSA card directly
- `-r 22050` — resample to 22050 Hz
- `-m` — output mono
- `-e s16` — output signed 16-bit PCM

## Suggested /etc/asound.conf

For normal use it is convenient to make ALSA convert application audio to a stable format accepted by the driver:

```conf
pcm.!default {
    type plug
    slave {
        pcm "hw:1,0"
        format S16_LE
        channels 1
        rate 22050
    }
}

ctl.!default {
    type hw
    card 1
}
```

After this, applications using the default ALSA device should play through the MCP49x1 driver without manual format options.

## Checking rate constraints

Example:

```sh
aplay -v --disable-resample --disable-format --disable-channels -D hw:1,0 -f U8 -c 1 -r 22345 /dev/zero
```

Expected result:

```text
rate       : 22050
exact rate : 22050 (22050/1)
```

This means ALSA did not use `22345 Hz`; it selected the supported `22050 Hz` rate.

## Limitations

This audio output is intentionally minimal.  Expected limitations:
- DAC resolution depends on `dac_bits`: 8, 10 or 12 bits
- mono physical output
- no analog amplifier
- no reconstruction filter
- limited low-frequency response
- audible noise in quiet passages compared to real audio hardware

For better quality, use a proper audio DAC, amplifier and speaker.  This driver is intended for simple embedded audio where minimal hardware is more important than Hi-Fi quality.

## Default parameters

The driver is loaded as a kernel module.

Default module parameters:

```text
gain_percent=100
limiter_enable=1
highpass_enable=1
highpass_q15=30000
fade_ms=24
psycho_bass_enable=1
psycho_bass_level=60
psycho_bass_shift=5
mmio_gpio1_base=0xff530000
dac_bits=8
dither=0
```
