# Reverse-engineering helpers

These are the tools used to work out the MST3367 set-up. They are not needed to use the driver.

- `emu.py` — a small symbolic x86-64 tracer that executes routines from an `llvm-objdump` listing.
- `hd60ctl.py` — runs selected routines of the Windows driver in the emulator and forwards the
  I2C/GPIO commands they issue to a real card through the driver's debugfs `cmd` file.

No Elgato code is included. To use `hd60ctl.py` you need your own copy of `e60MZ0380.X64.SYS`
(from Elgato's Windows installer, e.g. extracted with `7z x`) and a disassembly of it:

```sh
llvm-objdump -d --no-show-raw-insn --x86-asm-syntax=intel e60MZ0380.X64.SYS | gzip > re/sys.asm.gz
HD60_SYS=/path/to/e60MZ0380.X64.SYS sudo -E python3 re/hd60ctl.py detect   # or: init, dry, dump, reg PAGE REG
```

The function addresses in `hd60ctl.py` refer to driver version 1.1.0.194.
