#!/usr/bin/env python3
"""Tiny symbolic x86-64 tracer for llvm-objdump (intel syntax) listings.

Runs a function of the Windows driver with a fake device object and records
calls to the I2C / GPIO / sleep helpers, so the MST3367 programming sequences
can be replayed from Linux.  Values read from I2C are symbolic.
"""
import re, sys

MASK = {1: 0xff, 2: 0xffff, 4: 0xffffffff, 8: 0xffffffffffffffff}

class Sym:
    def __init__(self, e): self.e = e
    def __repr__(self): return self.e

def issym(v): return isinstance(v, Sym)

R64 = ['rax','rbx','rcx','rdx','rsi','rdi','rbp','rsp','r8','r9','r10','r11','r12','r13','r14','r15']
SUB = {}
for r in ['rax','rbx','rcx','rdx']:
    c = r[1]
    SUB['e'+c+'x'] = (r,4,0); SUB[c+'x'] = (r,2,0); SUB[c+'l'] = (r,1,0); SUB[c+'h'] = (r,1,8)
for r,b in [('rsi','si'),('rdi','di'),('rbp','bp'),('rsp','sp')]:
    SUB['e'+b] = (r,4,0); SUB[b] = (r,2,0); SUB[b+'l'] = (r,1,0)
for i in range(8,16):
    SUB['r%dd'%i] = ('r%d'%i,4,0); SUB['r%dw'%i] = ('r%d'%i,2,0); SUB['r%db'%i] = ('r%d'%i,1,0)
for r in R64: SUB[r] = (r,8,0)

SIZE = {'byte':1,'word':2,'dword':4,'qword':8,'xmmword':16}

class Emu:
    def __init__(self, asm_path, hooks, dev_fields):
        self.ins = {}
        self.order = []
        for l in open(asm_path):
            m = re.match(r'^([0-9a-f]{9}):\s+\t?(\S+)\s*(.*)$', l.rstrip('\n'))
            if not m: continue
            a = int(m.group(1), 16)
            raw = m.group(3)
            ops = raw.split('#')[0].strip()
            cm = re.search(r'#\s*(0x[0-9a-f]+)', raw)
            if cm and 'rip' in ops:
                ops = re.sub(r'\[rip [+-] 0x[0-9a-f]+\]', '[' + cm.group(1) + ']', ops)
            elif 'rip' in ops:
                rm = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', ops)
                if rm:
                    nxt_guess = None
                    ops = ops  # resolved below once next address known
                    self._ripfix = getattr(self, '_ripfix', []) + [int(m.group(1), 16)]
            ops = re.sub(r'\s*<[^>]*>', '', ops)
            self.ins[a] = (m.group(2), ops)
            self.order.append(a)
        self.next = {}
        for i in range(len(self.order)-1):
            self.next[self.order[i]] = self.order[i+1]
        for a in getattr(self, '_ripfix', []):
            mn, ops = self.ins[a]
            rm = re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]', ops)
            tgt = self.next[a] + (int(rm.group(2), 16) * (1 if rm.group(1) == '+' else -1))
            self.ins[a] = (mn, ops.replace(rm.group(0), '[0x%x]' % tgt))
        self.image = None
        self.hooks = hooks
        self.mem = {}          # byte-addressed concrete memory
        self.symmem = {}       # (addr,size) -> Sym
        self.reg = {r: 0 for r in R64}
        self.reg['rsp'] = 0x7f000000
        self.flags = None
        self.log = []
        self.symbranches = []
        for (addr, size), v in dev_fields.items():
            self.store(addr, size, v)
        self.nread = 0

    # ---- memory
    def store(self, addr, size, v):
        if issym(v):
            self.symmem[(addr,size)] = v
            for i in range(size): self.mem.pop(addr+i, None)
            return
        for k in [k for k in self.symmem if k[0] < addr+size and k[0]+k[1] > addr]:
            del self.symmem[k]
        for i in range(size):
            self.mem[addr+i] = (v >> (8*i)) & 0xff
    def imgbyte(self, a):
        if self.image is None: return 0
        for va, sz, raw in self.image:
            if va <= a < va + sz:
                return self.imgdata[raw + a - va] if raw + a - va < len(self.imgdata) else 0
        return 0
    def load(self, addr, size):
        if (addr,size) in self.symmem: return self.symmem[(addr,size)]
        if self.image is not None and 0x140000000 <= addr < 0x1403c1000:
            v = 0
            for i in range(size):
                bb = self.mem.get(addr+i)
                if bb is None: bb = self.imgbyte(addr+i)
                v |= bb << (8*i)
            return v
        for k,v in self.symmem.items():
            if k[0] <= addr < k[0]+k[1]: return Sym('(%r)[%d:%d]'%(v,addr-k[0],size))
        v = 0
        for i in range(size): v |= self.mem.get(addr+i, 0) << (8*i)
        return v

    # ---- registers
    def getr(self, name):
        r,sz,sh = SUB[name]
        v = self.reg[r]
        if issym(v):
            return v if sz == 8 or sh == 0 else Sym('(%r>>%d)'%(v,sh))
        return (v >> sh) & MASK[sz]
    def setr(self, name, v):
        r,sz,sh = SUB[name]
        if issym(v):
            self.reg[r] = v; return
        v &= MASK[sz]
        if sz == 4: self.reg[r] = v
        elif sz == 8: self.reg[r] = v
        else:
            old = self.reg[r]
            if issym(old): old = 0
            m = MASK[sz] << sh
            self.reg[r] = (old & ~m) | (v << sh)

    # ---- operands
    def ea(self, s):
        m = re.match(r'\[(.*)\]', s)
        inner = m.group(1)
        if inner.startswith('rip'):
            return None
        total = 0
        for term in re.split(r'\s*\+\s*', inner.replace(' - ', ' + -')):
            term = term.strip()
            if re.match(r'^-?0x[0-9a-f]+$|^-?\d+$', term):
                total += int(term, 0)
            elif '*' in term:
                sc, rg = term.split('*')
                v = self.getr(rg)
                if issym(v): return None
                total += int(sc) * v
            else:
                v = self.getr(term)
                if issym(v): return None
                total += v
        return total & MASK[8]
    def opsize(self, s, other=None):
        m = re.match(r'(byte|word|dword|qword|xmmword) ptr', s)
        if m: return SIZE[m.group(1)]
        if s in SUB: return SUB[s][1]
        if other is not None and other in SUB: return SUB[other][1]
        return 8
    def read(self, s, size=None):
        s = s.strip()
        if s in SUB: return self.getr(s)
        if re.match(r'^-?0x[0-9a-f]+$|^-?\d+$', s):
            v = int(s, 0)
            return v & MASK[size or 8]
        m = re.match(r'(?:(byte|word|dword|qword|xmmword) ptr )?(.*)', s)
        sz = SIZE[m.group(1)] if m.group(1) else size
        a = self.ea(m.group(2))
        if a is None: return 0
        if sz == 16: return 0
        return self.load(a, sz)
    def write(self, s, v, size=None):
        s = s.strip()
        if s in SUB: self.setr(s, v); return
        m = re.match(r'(?:(byte|word|dword|qword|xmmword) ptr )?(.*)', s)
        sz = SIZE[m.group(1)] if m.group(1) else size
        a = self.ea(m.group(2))
        if a is None: return
        if sz == 16:
            self.store(a, 8, 0); self.store(a+8, 8, 0); return
        if not issym(v): v &= MASK[sz]
        self.store(a, sz, v)

    # ---- flags
    def setflags(self, kind, a, b, res, size):
        self.flags = (kind, a, b, res, size)
    def cond(self, cc):
        if self.flags is None: return None
        kind, a, b, res, size = self.flags
        if any(issym(x) for x in (a, b, res)):
            return Sym('flags')
        m = MASK[size]; sb = 1 << (size*8-1)
        res &= m
        zf = res == 0
        sf = bool(res & sb)
        if kind == 'sub':
            cf = (a & m) < (b & m)
            of = bool(((a ^ b) & (a ^ res)) & sb)
        elif kind == 'add':
            cf = (a & m) + (b & m) > m
            of = bool((~(a ^ b) & (a ^ res)) & sb)
        else:
            cf = False; of = False
        if kind == 'neg': cf = (a & m) != 0
        t = {'e':zf,'z':zf,'ne':not zf,'nz':not zf,'b':cf,'c':cf,'ae':not cf,'nc':not cf,
             'a':(not cf and not zf),'be':(cf or zf),'s':sf,'ns':not sf,
             'l':sf!=of,'ge':sf==of,'g':(not zf and sf==of),'le':(zf or sf!=of)}
        return t[cc]

    def push(self, v):
        self.reg['rsp'] -= 8; self.store(self.reg['rsp'], 8, v)
    def pop(self):
        v = self.load(self.reg['rsp'], 8); self.reg['rsp'] += 8; return v

    def arg_stack(self, off, size):
        return self.load(self.reg['rsp'] + off, size)

    # ---- run
    def call(self, target, depth=0, maxsteps=200000):
        ret_marker = 0xdead0000 + depth
        self.push(ret_marker)
        pc = target
        steps = 0
        while True:
            steps += 1
            if steps > maxsteps: raise RuntimeError('too many steps')
            if pc == ret_marker: return
            if pc not in self.ins: raise RuntimeError('no instruction at %x' % pc)
            mn, ops = self.ins[pc]
            nxt = self.next.get(pc)
            parts = [p.strip() for p in re.split(r',(?![^\[]*\])', ops)] if ops else []
            jumped = None
            if mn in ('mov','movabs'):
                sz = self.opsize(parts[0], parts[1])
                self.write(parts[0], self.read(parts[1], sz), sz)
            elif mn in ('movzx',):
                v = self.read(parts[1]); self.write(parts[0], v)
            elif mn in ('movsxd','movsx','cdqe'):
                if mn == 'cdqe':
                    v = self.getr('eax')
                    if not issym(v) and v & 0x80000000: v |= 0xffffffff00000000
                    self.setr('rax', v)
                else:
                    ssz = self.opsize(parts[1]); v = self.read(parts[1])
                    if not issym(v) and v & (1 << (ssz*8-1)): v -= 1 << (ssz*8)
                    self.write(parts[0], v)
            elif mn == 'lea':
                a = self.ea(re.sub(r'^\w+ ptr ', '', parts[1]))
                self.write(parts[0], a if a is not None else 0)
            elif mn in ('xor','and','or','add','sub','cmp','test','adc','sbb'):
                sz = self.opsize(parts[0], parts[1])
                a = self.read(parts[0], sz); b = self.read(parts[1], sz)
                if mn == 'xor' and parts[0] == parts[1]:
                    res = 0
                elif issym(a) or issym(b):
                    res = Sym('(%r %s %r)' % (a, mn, b))
                    if mn in ('or','and') and not issym(b):
                        res = Sym('(%r %s 0x%x)' % (a, {'or':'|','and':'&'}[mn], b))
                else:
                    if mn == 'xor': res = a ^ b
                    elif mn in ('and','test'): res = a & b
                    elif mn == 'or': res = a | b
                    elif mn == 'add': res = a + b
                    elif mn in ('sub','cmp'): res = a - b
                    elif mn == 'sbb':
                        cf = self.cond('b'); res = a - b - (1 if cf else 0)
                    elif mn == 'adc':
                        cf = self.cond('b'); res = a + b + (1 if cf else 0)
                kind = 'sub' if mn in ('sub','cmp','sbb') else 'add' if mn in ('add','adc') else 'logic'
                self.setflags(kind, a, b, res, sz)
                if mn not in ('cmp','test'): self.write(parts[0], res, sz)
            elif mn in ('inc','dec','neg','not'):
                sz = self.opsize(parts[0]); a = self.read(parts[0], sz)
                if issym(a): res = Sym('%s(%r)' % (mn, a))
                else: res = {'inc':a+1,'dec':a-1,'neg':-a,'not':~a}[mn]
                if mn != 'not':
                    self.setflags({'inc':'add','dec':'sub','neg':'neg'}[mn], a if mn != 'neg' else a, 1 if mn != 'neg' else 0, res, sz)
                self.write(parts[0], res, sz)
            elif mn in ('shl','shr','sar','rol','ror'):
                sz = self.opsize(parts[0])
                a = self.read(parts[0], sz)
                n = self.read(parts[1]) if len(parts) > 1 else 1
                if issym(a) or issym(n): res = Sym('(%r %s %r)' % (a, mn, n))
                else:
                    n &= 0x3f
                    if mn == 'shl': res = a << n
                    elif mn == 'shr': res = a >> n
                    elif mn == 'sar':
                        if a & (1 << (sz*8-1)): a -= 1 << (sz*8)
                        res = a >> n
                    else: res = a
                self.setflags('logic', a, 0, res, sz)
                self.write(parts[0], res, sz)
            elif mn in ('imul','mul','div','idiv'):
                if mn == 'imul' and len(parts) == 3:
                    a = self.read(parts[1]); b = self.read(parts[2])
                    res = Sym('mul') if issym(a) or issym(b) else a*b
                    self.write(parts[0], res)
                elif mn == 'imul' and len(parts) == 2:
                    a = self.read(parts[0]); b = self.read(parts[1])
                    res = Sym('mul') if issym(a) or issym(b) else a*b
                    self.write(parts[0], res)
                elif mn == 'imul' and len(parts) == 1:
                    sz = self.opsize(parts[0]); a = self.read(parts[0]); b = self.getr({4:'eax',8:'rax',2:'ax',1:'al'}[sz])
                    if issym(a) or issym(b): self.setr('rax', Sym('mul')); self.setr('rdx', Sym('mulhi'))
                    else:
                        sb = 1 << (sz*8-1)
                        if a & sb: a -= 1 << (sz*8)
                        if b & sb: b -= 1 << (sz*8)
                        p_ = (a*b) & ((1 << (2*sz*8)) - 1)
                        self.setr({4:'eax',8:'rax'}[sz], p_ & MASK[sz]); self.setr({4:'edx',8:'rdx'}[sz], p_ >> (sz*8))
                elif mn == 'mul':
                    sz = self.opsize(parts[0]); a = self.read(parts[0]); b = self.getr({4:'eax',8:'rax'}[sz])
                    if issym(a) or issym(b): self.setr('rax', Sym('mul')); self.setr('rdx', Sym('mulhi'))
                    else:
                        p = a*b; self.setr({4:'eax',8:'rax'}[sz], p & MASK[sz]); self.setr({4:'edx',8:'rdx'}[sz], p >> (sz*8))
                else:
                    sz = self.opsize(parts[0]); d = self.read(parts[0])
                    hi = self.getr({4:'edx',8:'rdx'}[sz]); lo = self.getr({4:'eax',8:'rax'}[sz])
                    if issym(d) or issym(hi) or issym(lo) or d == 0:
                        self.setr('rax', Sym('div')); self.setr('rdx', Sym('mod'))
                    elif mn == 'idiv':
                        n = (hi << (sz*8)) | lo
                        if n & (1 << (2*sz*8-1)): n -= 1 << (2*sz*8)
                        if d & (1 << (sz*8-1)): d -= 1 << (sz*8)
                        q = abs(n) // abs(d); q = q if (n >= 0) == (d >= 0) else -q
                        r = n - q*d
                        self.setr({4:'eax',8:'rax'}[sz], q & MASK[sz]); self.setr({4:'edx',8:'rdx'}[sz], r & MASK[sz])
                    else:
                        n = (hi << (sz*8)) | lo
                        self.setr({4:'eax',8:'rax'}[sz], n // d); self.setr({4:'edx',8:'rdx'}[sz], n % d)
            elif mn.startswith('set'):
                c = self.cond(mn[3:]); self.write(parts[0], c if issym(c) else int(bool(c)), 1)
            elif mn.startswith('cmov'):
                c = self.cond(mn[4:])
                if issym(c): self.write(parts[0], Sym('cmov'))
                elif c: self.write(parts[0], self.read(parts[1]))
            elif mn == 'bt':
                a = self.read(parts[0]); b = self.read(parts[1])
                if issym(a) or issym(b): self.flags = ('sym', Sym('bt'), 0, 0, 4)
                else:
                    cf = (a >> (b & 63)) & 1
                    self.flags = ('bt', 0, 0, 0, 4); self._btcf = cf
            elif mn in ('bts','btr'):
                a = self.read(parts[0]); b = self.read(parts[1])
                if not issym(a) and not issym(b):
                    self.write(parts[0], a | (1 << b) if mn == 'bts' else a & ~(1 << b))
            elif mn in ('push',):
                self.push(self.read(parts[0]))
            elif mn in ('pop',):
                self.write(parts[0], self.pop())
            elif mn == 'ret':
                jumped = self.pop()
            elif mn == 'jmp':
                if parts[0].startswith('0x'): jumped = int(parts[0], 16)
                else: raise RuntimeError('indirect jmp at %x' % pc)
            elif mn.startswith('j'):
                cc = mn[1:]
                if self.flags and self.flags[0] == 'bt':
                    c = {'b':self._btcf,'c':self._btcf,'ae':not self._btcf,'nc':not self._btcf}[cc]
                else:
                    c = self.cond(cc)
                if issym(c) or c is None:
                    self.symbranches.append(pc)
                    self.log.append(('SYMBRANCH', hex(pc), mn))
                    c = self.symdefault.get(pc, False) if hasattr(self, 'symdefault') else False
                if c: jumped = int(parts[0], 16)
            elif mn == 'call':
                if parts[0].startswith('qword ptr'):
                    self.log.append(('IMPORT', hex(pc))); self.setr('rax', 0)
                else:
                    t = int(parts[0], 16)
                    h = self.hooks.get(t)
                    if h is not None:
                        r = h(self)
                        if r is not None: self.setr('rax', r)
                    else:
                        self.call(t, depth+1)
            elif mn in ('cdq','cqo','cwde','cbw','cwd'):
                if mn == 'cdq':
                    v = self.getr('eax'); self.setr('edx', Sym('sx') if issym(v) else (0xffffffff if v & 0x80000000 else 0))
                elif mn == 'cqo':
                    v = self.getr('rax'); self.setr('rdx', Sym('sx') if issym(v) else (MASK[8] if v >> 63 else 0))
                elif mn == 'cwde':
                    v = self.getr('ax'); self.setr('eax', v if issym(v) else (v | 0xffff0000 if v & 0x8000 else v))
                elif mn == 'cwd':
                    v = self.getr('ax'); self.setr('dx', Sym('sx') if issym(v) else (0xffff if v & 0x8000 else 0))
                else:
                    v = self.getr('al'); self.setr('ax', v if issym(v) else (v | 0xff00 if v & 0x80 else v))
            elif mn in ('movups','movdqu','movaps','movdqa','movq','movd') or mn in ('xorps','pxor','xorpd'):
                if not hasattr(self, 'xmm'): self.xmm = {}
                def xr(op):
                    op = op.strip()
                    if re.match(r'^xmm\d+$', op): return self.xmm.get(op, 0)
                    if op in SUB: return self.getr(op)
                    m2 = re.match(r'(?:(\w+) ptr )?(.*)', op)
                    a = self.ea(m2.group(2))
                    if a is None: return 0
                    n = 8 if mn in ('movq',) else 4 if mn == 'movd' else 16
                    v = 0
                    for i in range(n):
                        b_ = self.load(a + i, 1); v |= (0 if issym(b_) else b_) << (8*i)
                    return v
                def xw(op, v):
                    op = op.strip()
                    if re.match(r'^xmm\d+$', op): self.xmm[op] = v; return
                    if op in SUB: self.setr(op, v & MASK[8]); return
                    m2 = re.match(r'(?:(\w+) ptr )?(.*)', op)
                    a = self.ea(m2.group(2))
                    if a is None: return
                    n = 8 if mn in ('movq',) else 4 if mn == 'movd' else 16
                    for i in range(n): self.store(a + i, 1, (v >> (8*i)) & 0xff)
                if mn in ('xorps','pxor','xorpd'):
                    if parts[0] == parts[1]: self.xmm[parts[0]] = 0
                    else: self.xmm[parts[0]] = self.xmm.get(parts[0], 0) ^ xr(parts[1])
                else:
                    v = xr(parts[1])
                    if issym(v): v = 0
                    if mn == 'movq': v &= MASK[8]
                    if mn == 'movd': v &= MASK[4]
                    xw(parts[0], v)
            elif mn in ('nop','int3','lock','prefetchw'):
                pass
            elif mn == 'xchg':
                a = self.read(parts[0]); b = self.read(parts[1]); self.write(parts[0], b); self.write(parts[1], a)
            else:
                raise RuntimeError('unhandled %s %s at %x' % (mn, ops, pc))
            pc = jumped if jumped is not None else nxt
