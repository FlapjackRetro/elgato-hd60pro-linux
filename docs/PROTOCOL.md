# HD60 Pro (YUAN MZ0380) host interface

Notes reconstructed from the Windows driver `e60MZ0380.X64.SYS` 1.1.0.194 and from the card-side
PCIe endpoint module (`ep.ko`, shipped inside the card's firmware update package), then confirmed on
hardware.

## Hardware

- **SoC:** VATICS Mozart (ARM926, embedded Linux 2.6.28) running from on-board flash. It boots on its own;
  the host never needs to upload firmware. The `.HEX` files in the Windows package are gzipped tar
  archives used only to update the flash.
- **HDMI receiver:** MStar MST3367, 8-bit I2C address `0x9c`, programmed by the host through the
  card's I2C mailbox commands. Register page select is register `0x00`.
- **PCIe:** Gen1 x1. BAR0 = 32 MiB window into card RAM (mailbox at offset 0), BAR5 = 4 KiB card
  PCIe controller registers.

## Registers

| Offset | Meaning |
|---|---|
| BAR0 `0x00` | doorbell: write `0x800` = command posted, `0x400` = interrupt acknowledged |
| BAR0 `0x04` | command id |
| BAR0 `0x08`–`0x2c` | parameters / response (up to 10 dwords; the 10th overlaps the ack word) |
| BAR0 `0x2c` | acknowledge word written by the card (host clears to 0 before posting) |
| BAR0 `0x30` | interrupt status (host clears to 0): bit 0–7 video channel done, bit 11 command done, bits 16–19 audio buffer 0–3 of channel 0 full |
| BAR0 `0x40`/`0x44`/`0x48` | video done info, one nibble per channel; nibble = index of the filled buffer |
| BAR0 `0x4c` | audio done info |
| BAR0 `0x50` (byte) | host writes 0 after taking a video frame; otherwise the card throttles to one frame per 100 ms |
| BAR0 `0x50`–`0x5c` | cleared before starting capture |
| BAR5 `0xdc` | write 2 to de-assert the legacy interrupt |
| BAR5 `0x30`/`0x38` | inbound window start/limit = BAR0 bus address + 4 / + 0x5f |

### Interrupt handling

Read BAR0 `0x30`; if non-zero and not `0xffffffff`: write BAR5 `0xdc`=2, BAR0 `0x30`=0,
BAR0 `0x00`=`0x400`, read the done registers, write byte 0 to BAR0 `0x50`.

### Command protocol

Write BAR0 `0x2c`=0, the command id to `0x04`, parameters from `0x08`, then `0x800` to `0x00`.
Completion is signalled by status bit 11 and, for most commands, a non-zero ack word.

## Commands

| Id | Name | Parameters |
|---|---|---|
| `0x01` | firmware ready | — (ack `0xdddddddd`) |
| `0x0a` | firmware version | 0, 0 → major at `0x08`, minor at `0x0c` (ack `0xaaaaaaaa`) |
| `0x02` | register preview buffers | ch, size, 4× (addr_hi, addr_lo) |
| `0x03` | register audio buffers | ch, 0x2000, 4× (addr_hi, addr_lo) — 8 channels, consecutive 8 KiB chunks |
| `0x04`/`0x05` | register H.264 main/sub buffers | ch, size, 4× address |
| `0x06` / `0x07` | start / stop | `0xffffffff` |
| `0x0b`–`0x0f`, `0x60`, `0x61`, `0x7b`, `0x7c` | firmware / logo / flash download | **never used** |
| `0x15` | GPIO set | 1<<pin, value<<pin |
| `0x17` | GPIO direction | 1<<pin, input<<pin |
| `0x1a` | I2C read byte | addr8, reg, 0 → value at `0x10` |
| `0x1b` | I2C write byte | addr8, reg, value |
| `0x1e` | I2C read block | len<<16 \| reg<<8 \| addr8 → data from `0x0c` |
| `0x29` | SET_VIC (video capture) | see below |
| `0x2a` | SET_AIC (audio capture) | see below |
| `0x2d` | H.264 encoder parameters | 10 dwords |
| `0x31` | preview parameters | mask `0x1f`, fps<<8, deinterlace, 0, 0 |
| `0x50` | OSD slot | (slot<<16) \| (0 or 0x80), 9× 0 |
| `0x51`, `0x62` | misc. set-up sent by Windows before start | |

### SET_VIC (`0x29`) parameter bytes (BAR0 offsets)

| Offset | Field |
|---|---|
| `0x08` | channel |
| `0x09` | fps |
| `0x0a` | `fw`: 6 = YUY2, 5/7 = YV12 via card encoder app tinyvenc5/7 |
| `0x0b` | input format: 6 = BT.1120 progressive, 7 = interlaced |
| `0x0c`/`0x0e` | width / height (u16) |
| `0x10`–`0x12` | m, flip, mirror |
| `0x14`–`0x17` | colour info (Windows: 2,1,1,1) |
| `0x18`/`0x1a` | x / y start |
| `0x1c`/`0x1e` | input width / height |
| `0x20`–`0x23` | bitstream count, OSD enable, OSD size, no-signal flag |
| `0x24`–`0x27` | VANC lines, fast kill, interrupt reduce, slave |
| `0x28`–`0x2b` | no-signal background colour, Y, U, V |

### SET_AIC (`0x2a`)

`0x08`: channel, channels(2), bits(16, u16 at `0x0a`); `0x0c`: sample rate; `0x10`: frames per period
(u16, 256) and periods (u16 at `0x12`, 4); `0x14`: enable, `0x15`: interrupt mode (1).

## Data formats

- **Video:** I420 (Y plane, then U, then V), stride = width, in 4 buffers of 0x466000 bytes.
- **Audio:** each 8 KiB audio buffer carries 4096 valid bytes = 1024 frames of 16-bit little-endian
  interleaved stereo (21.3 ms at 48 kHz). Status bits 16–19 say which buffer just filled.

## HDMI receiver (MST3367)

Mode detection, per the Windows `MST3367_HDMI_MODE_DETECT`:

- p0 `0x55`: signal stable when `(v & 0x3c) == 0x3c`
- p0 `0x6a` (low nibble), `0x6b`: H total
- p0 `0x57`/`0x58` (14 bit): H period → H freq = 1 600 000 / period (100 Hz units)
- p0 `0x59`/`0x5a` (14 bit): V period → V freq = 1 250 000 / period (0.1 Hz units)
- p0 `0x5b` (3 bit), `0x5c`: V total
- p0 `0x5f` bit 3: interlaced
- p2 `0x29`/`0x28`: H active
- p1 `0x01` bit 2 + p2 `0x47`: deep colour factor
- p2 `0x48`: AVI infoframe byte 1 (bits 6:5 = colour space)

The init sequence and colour-space-conversion values in the driver were produced by running the
Windows routines in the emulator in `re/` and recording their I2C traffic.
