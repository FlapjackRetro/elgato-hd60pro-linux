# hd60pro — Linux driver for the Elgato Game Capture HD60 Pro

An experimental, reverse-engineered Linux kernel driver for the **Elgato Game Capture HD60 Pro**
PCI Express capture card (YUAN MZ0380, PCI ID `12ab:0380` / `12ab:0381`).

It provides:

- a **V4L2 video capture device** (`/dev/videoN`, "Elgato HD60 Pro") delivering 1080p60 as planar YUV 4:2:0 (`YU12` / I420)
- an **ALSA capture device** ("Elgato HD60 Pro", card `HD60Pro`) delivering the HDMI audio as 48 kHz 16-bit stereo

Both work in OBS Studio (*Video Capture Device (V4L2)* and *Audio Input Capture (PipeWire/PulseAudio)*).

> **Status:** works on the developer's card (HD60 Pro Rev. 1, subsystem `1cfa:0006`) on Ubuntu 24.04 with kernel 7.0,
> capturing an Analogue Super Nt at 1920×1080 60 Hz. Other revisions, resolutions and sources are untested.
> Use at your own risk.

## Tested hardware

| Board | PCI ID | Subsystem | Status |
|---|---|---|---|
| HD60 Pro Rev. 1 (Ryzen fix) | `12ab:0380` | `1cfa:0006` | works |
| HD60 Pro Rev. 1 | `12ab:0380` | `1cfa:0003` | untested, should work |
| HD60 Pro Rev. 3 | `12ab:0381` | `1cfa:0010` | untested — uses a different HDMI receiver path, probably needs work |

Check yours with `lspci -nn -d 12ab:`.

## Install

Requirements: kernel headers, `build-essential`, `dkms`. Secure Boot must be off, or you must sign the module yourself.

```sh
git clone https://github.com/<you>/elgato-hd60pro-linux.git
cd elgato-hd60pro-linux
sudo ./install.sh            # builds with DKMS, loads the driver, loads it at boot
sudo ./install.sh --remove   # uninstall
```

DKMS rebuilds the module automatically when your kernel is updated.

To build without installing: `make`, then `sudo insmod hd60pro.ko`
(it needs `videobuf2-vmalloc`, `videobuf2-v4l2` and `snd-pcm` loaded).

## Using it with OBS

1. **Sources → + → Video Capture Device (V4L2)** → device *Elgato HD60 Pro*, format *Planar YUV 4:2:0*.
2. **Sources → + → Audio Input Capture (PipeWire)** (or PulseAudio) → *Elgato HD60 Pro*.
3. If audio and video are out of step, adjust **Sync Offset** in *Advanced Audio Properties*.

The input resolution is detected when capture starts. If you change the source's output mode while
capturing, deactivate and reactivate the OBS source.

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `bitstreams` | `0` | H.264 streams the card encodes alongside the preview. `1` matches the Windows driver but limits capture to ~55 fps on a Gen1 x1 link. |
| `pixmode` | `0` | Card output mode: `0` auto (YV12/I420 like Windows), `6` YUY2. |
| `force_width`, `force_height`, `force_fps` | `0` | Override the detected input mode. |
| `uv_swap` | `0` | Swap the chroma planes if colours are wrong on your board. |
| `use_msi` / `poll` | `1` / `0` | Interrupt mode; `poll=1` polls the status register instead. |
| `mst_init` | `1` | Initialise the HDMI receiver at load time. |
| `debug` | `0` | `1`–`3` for more kernel log output. |

## Safety

The card runs its own embedded Linux from on-board flash. **This driver never uploads or flashes firmware**;
the firmware/flash commands of the card's mailbox are blocked, including through the debug interface.
No Elgato firmware or Windows driver files are included in this repository.

## How it works

The card contains a VATICS "Mozart" ARM SoC that does the capture and DMAs frames and audio into host
memory, and an MStar MST3367 HDMI receiver that the **host** has to program over I2C (through the card).
The protocol was reconstructed from the Windows driver and the card's own endpoint module; see
[docs/PROTOCOL.md](docs/PROTOCOL.md). The `re/` directory contains the small x86 emulator that was used to
run the Windows driver's receiver set-up and mode-detection routines and capture their register sequences.

## Known limitations

- Resolution changes while streaming are not handled; restart capture.
- Only 48 kHz stereo audio is exposed.
- Only the uncompressed preview path is supported; the card's hardware H.264 encoder is not exposed.
- No brightness/contrast/saturation controls yet (the Windows defaults are used).

## Debugging

With the driver loaded, `/sys/kernel/debug/<pci-address>/` contains:

- `regs` — card status, mailbox registers and detected signal
- `cmd` — send a raw mailbox command (`echo "0x1a 0x9c 0x55 0" > cmd`), read back the response
- `frame`, `abuf` — raw contents of the last video buffer / the audio ring
- `atrace` — recent audio interrupt trace
- `detect` — write anything to re-run signal detection

`test/` contains the scripts used during bring-up (`test.sh`, `audiotest.sh`, `fpstest.sh`); run them from
the repository root with `sudo bash test/<script>`.

## License

GPL-2.0, see [LICENSE](LICENSE).

This project is not affiliated with or endorsed by Elgato, Corsair or YUAN High-Tech.
"Elgato" and "Game Capture HD60 Pro" are trademarks of their respective owners.
