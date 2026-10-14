#!/usr/bin/env python3
"""hd60ctl - bring-up helper for the hd60pro driver.

Runs selected routines of the Windows driver (e60MZ0380.X64.SYS) inside a small
x86 emulator and forwards the I2C/GPIO commands they issue to the real card
through the hd60pro debugfs interface.  Used to compare our native code with
what Windows does and to configure the HDMI receiver exactly like Windows.

usage: sudo python3 hd60ctl.py detect|init|startseq|reg PAGE REG
"""
import gzip, glob, os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from emu import Emu, Sym, issym

SYS_CANDIDATES = [os.environ.get('HD60_SYS', ''),
                  os.path.join(HERE, '..', '..', 'HD60_extracted', 'e60MZ0380.X64.SYS'),
                  os.path.join(HERE, '..', 'HD60_extracted', 'e60MZ0380.X64.SYS'),
                  os.path.join(HERE, 'e60MZ0380.X64.SYS')]
DEV, EXT, HDR, BAR0, BAR5 = 0x10000000, 0x20000000, 0x30000000, 0x40000000, 0x50000000
LIVE_OK = {0x1a, 0x1b, 0x14, 0x15, 0x17, 0x1e}

def find_dbg():
    for d in glob.glob('/sys/kernel/debug/0000:*'):
        if os.path.exists(os.path.join(d, 'cmd')) and os.path.exists(os.path.join(d, 'regs')):
            return d
    sys.exit('hd60pro debugfs not found - is the driver loaded and are you root?')

class Card:
    def __init__(self, live=True):
        self.dbg = find_dbg() if live else None
        self.live = live
        self.log = []
    def cmd(self, words):
        cmd, p = words[0], words[1:]
        desc = 'CMD %02x %s' % (cmd, ' '.join('%08x' % x for x in p))
        if not self.live or cmd not in LIVE_OK:
            self.log.append(desc + ('  (not sent)' if self.live else ''))
            return True, [0] * 12
        with open(os.path.join(self.dbg, 'cmd'), 'w') as f:
            f.write(' '.join('0x%x' % w for w in words) + '\n')
        with open(os.path.join(self.dbg, 'cmd'), 'rb', buffering=0) as f:
            parts = f.read(4096).decode().split()
        ret = int(parts[0]); resp = [int(x, 16) for x in parts[2:]]
        if cmd == 0x1a:
            self.log.append('I2C R %02x r%02x -> %02x%s' % (p[0], p[1], resp[2] & 0xff, ' TIMEOUT' if ret else ''))
        elif cmd == 0x1b:
            self.log.append('I2C W %02x r%02x = %02x%s' % (p[0], p[1], p[2], ' TIMEOUT' if ret else ''))
        else:
            self.log.append(desc + (' TIMEOUT' if ret else ''))
        return ret == 0, resp

def load_sys():
    for c in SYS_CANDIDATES:
        if os.path.exists(c): return open(c, 'rb').read()
    sys.exit('e60MZ0380.X64.SYS not found next to this tool or in ../HD60_extracted')

def make_emu(card, sysdata):
    def cstr(va):
        o = va - 0x140001000 + 0x400
        return sysdata[o:sysdata.index(b'\0', o)].decode('latin1')
    def send_cmd(e):
        buf = e.getr('rdx'); n = e.getr('r8d')
        words = [e.load(buf + 4 * i, 4) for i in range(1, n)]
        words = [0 if issym(w) else w for w in words]
        ok, resp = card.cmd(words)
        for i, v in enumerate(resp[:10]): e.store(BAR0 + 8 + 4 * i, 4, v)
        return 1 if ok else 0
    def sleep(e):
        ms = e.getr('ecx')
        if card.live and not issym(ms): time.sleep(min(ms, 2000) / 1000.0)
        return None
    def dprint(e):
        import re
        try:
            fmt = cstr(e.getr('rcx'))
            args = [e.getr('rdx'), e.getr('r8'), e.getr('r9')] + [e.load(e.reg['rsp'] + 0x20 + 8 * i, 8) for i in range(24)]
            ai = [0]
            def rep(mm):
                spec = mm.group(0)
                if spec == '%%': return '%'
                v = args[ai[0]] if ai[0] < len(args) else 0; ai[0] += 1
                if issym(v): return '?'
                if spec[-1] == 's':
                    try: return cstr(v)
                    except Exception: return '<str>'
                if spec[-1] == 'S': return '<wstr>'
                v &= 0xffffffff
                if spec[-1] == 'd' and v & 0x80000000: v -= 1 << 32
                try: return (spec.replace('l', '').replace('I64', '')) % v
                except Exception: return str(v)
            card.log.append('WIN: ' + re.sub(r'%[-0-9.]*(?:I64|l)?[dxXuics%Sp]', rep, fmt).strip())
        except Exception:
            pass
        return None
    def ign(e): return None
    def memset(e):
        d = e.getr('rcx'); v = e.getr('edx') & 0xff; n = e.getr('r8')
        for i in range(n): e.store(d + i, 1, v)
        return d
    def memcpy(e):
        d = e.getr('rcx'); s = e.getr('rdx'); n = e.getr('r8')
        for i in range(n):
            b = e.load(s + i, 1); e.store(d + i, 1, 0 if issym(b) else b)
        return d
    hooks = {0x140285074: send_cmd, 0x1400012e0: sleep, 0x140296f7b: dprint, 0x140295750: ign,
             0x1402977c0: memset, 0x140297500: memcpy}
    f = {(DEV + 0x28, 8): HDR, (DEV + 0x1d110, 8): EXT, (DEV + 0x108, 8): BAR0, (DEV + 0x110, 8): BAR5,
         (HDR + 8, 2): 0x12ab, (HDR + 0xa, 2): 0x0380, (HDR + 0xc, 2): 0x0006,
         (HDR + 0xe, 1): 0xfa, (HDR + 0xf, 1): 0x1c, (EXT + 0x1d110, 8): EXT,
         (EXT + 0x2090, 1): 0xff, (EXT + 0x2044, 4): 0xf1, (EXT + 0x202c, 4): 0xffff}
    asm = os.path.join(HERE, 'sys.asm.gz')
    with gzip.open(asm, 'rt') as fh:
        tmp = os.path.join('/tmp', 'hd60_sys.asm')
        if not os.path.exists(tmp):
            with open(tmp, 'w') as out: out.write(fh.read())
    e = Emu(tmp, hooks, f)
    # Windows registry defaults for brightness/contrast/saturation/hue/sharpness
    for off in (0x6ff8, 0x7038, 0x7078, 0x70b8, 0x70f8, 0x7138, 0x7178):
        e.store(DEV + off, 4, 128)
    e.store(DEV + 0x71b8, 4, 32)
    e.symdefault = {}
    e.imgdata = sysdata
    e.image = [(0x140001000, 0x2a2f37, 0x400), (0x1402a4000, 0x815, 0x2a3400),
               (0x1402a5000, 0x49ce8, 0x2a3e00), (0x1402ef000, 0xace00, 0x2edc00)]
    return e

def call(e, fn):
    e.reg['rcx'] = DEV
    e.reg['rsp'] = 0x7f000000
    e.call(fn)

def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    what = sys.argv[1]
    sysdata = load_sys()
    if what == 'reg':
        card = Card(True)
        page, reg = int(sys.argv[2], 0), int(sys.argv[3], 0)
        card.cmd([0x1b, 0x9c, 0x00, page]); ok, r = card.cmd([0x1a, 0x9c, reg, 0])
        print('page %d reg 0x%02x = 0x%02x' % (page, reg, r[2] & 0xff)); return
    if what == 'dump':
        card = Card(True)
        for page in (0, 1, 2):
            card.cmd([0x1b, 0x9c, 0x00, page]); row = []
            for reg in range(256):
                ok, r = card.cmd([0x1a, 0x9c, reg, 0]); row.append(r[2] & 0xff)
            for i in range(0, 256, 16):
                print('p%d %02x: %s' % (page, i, ' '.join('%02x' % x for x in row[i:i + 16])))
        return
    card = Card(live=(what != 'dry'))
    e = make_emu(card, sysdata)
    if what == 'init':
        card.log.append('==== Windows MST3367 init')
        call(e, 0x14024dc28)
    if what in ('detect', 'init', 'dry'):
        for i in range(3):
            card.log.append('==== Windows signal check #%d' % (i + 1))
            call(e, 0x14024efc8)
            if card.live: time.sleep(0.5)
        w, h, fps, il = [e.load(EXT + o, 4) for o in (0x30c, 0x310, 0x314, 0x318)]
        card.log.append('DETECTED (Windows logic): %sx%s @ %s %s' % (w, h, fps, 'interlaced' if il == 1 else ''))
        # what Windows would send to start capture (not sent)
        card.live_saved = card.live; card.live = False
        card.log.append('==== Windows start sequence (dry run, not sent)')
        call(e, 0x140288208)
        card.live = card.live_saved
    print('\n'.join(card.log))

if __name__ == '__main__':
    main()
