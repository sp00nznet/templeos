# Survey: TempleOS V5.03 as a statically recompiled program

This is the engineering survey behind templeos-recomp: what is on the disc,
how it boots, what the kernel touches, where code is made at run time, and the
approach chosen. Paths like `Kernel/KMain.HC` are files on the TempleOS disc
(`tools/extract.py` unpacks them to `work/src/` for reading).

## 1. The ISO

`TempleOS.ISO` (17,350,656 bytes, 8,472 blocks of 2 KB) is not an ISO 9660 file
system in any useful sense: the primary volume descriptor has an empty root and
exists so that firmware finds the El Torito boot record. The real file system is
**RedSea**, TempleOS's own, starting at 512-byte block 88:

| What | Where |
|---|---|
| El Torito boot catalog | 2 KB block 20; one entry, no emulation, 4 virtual sectors at block 21 |
| Boot image (`Adam/Opt/Boot/BootDVD.HC`) | block 21; carries the kernel's location, patched in by the installer: block 25, 94 blocks, shifted 2 × 512 B |
| RedSea boot record | 512-byte block 88 (`CRedSeaBoot`, signature 0x88, `0xAA55`) |
| RedSea root directory | block 98 (`drv_offset` 88, 0x8408 sectors, 9 bitmap sectors) |

A RedSea directory is an array of 64-byte `CDirEntry` (attr U16, name[38],
cluster I64, size I64, CDate); clusters are absolute 512-byte blocks and every
file is contiguous. 645 of the 652 files are compressed (`.Z`): TempleOS's own
LZW (`Kernel/Compress.HC`, `CArcCompress`: compressed size, expanded size, type;
7- or 8-bit base codes up to 12-bit codes, with entry reuse once the table is
full). `tools/redsea.py` and `tools/tosz.py` read both, from the ISO, in Python.

Top level: `0000Boot/0000Kernel.BIN.C` (the kernel, 189,760 bytes, stored
uncompressed and contiguous so the boot loader can read it), `Compiler/`
(`Compiler.BIN.Z`, the precompiled HolyC compiler, plus its source), `Kernel/`,
`Adam/` (the rest of the OS, as HolyC source), `Apps/`, `Demo/`, `Doc/`,
`Misc/` and the start-up scripts `StartOS.HC.Z`, `MakeHome.HC.Z`, `Home*.HC.Z`.

## 2. The boot path

```
 firmware ──El Torito──▶ BootDVD (16-bit)      reads 94 blocks to 0x7C00 via INT 13h
                          │
                          ▼
 KStart16.HC (16-bit)    INT 10h mode 0x12 (640x480x16), E801/E820 memory maps,
                          PCI BIOS probe, A20, GDT, -> protected mode
                          │
 KStart32.HC (32-bit)    patch absolute addresses (+0x7C20), find PCI BIOS32,
 Mem/PageTables.HC        identity page tables (2 MB pages, +4 GB uncached alias),
                          first block pool below 16 MB, EFER.LME, -> long mode
                          │
 KStart64.HC (64-bit)    FNINIT/FXSAVE, CPU struct, Adam heap and task, -> KMain
                          │
 KMain (Kernel/KMain.HC) block pools, globals, LoadKernel (exports into the hash
                          table, kernel self-imports patched), VGA raw text, IDT,
                          PIC, timers, block devices (finds the boot CD), keyboard
                          and mouse, then Load("Compiler") and ExeFile("StartOS")
                          │
 StartOS.HC (JIT)        #include the kernel/compiler headers, Adam (MakeAdam),
                          the window manager task, the registry, MakeHome
```

The kernel is a `CBinFile` module (`TOSB`), loaded with its header at 0x7C00, so
its code starts at 0x7C20 (`SYS_KERNEL`). Its patch table holds one
`IET_ABS_ADDR` list (8,000-odd 32-bit absolute addresses, patched by KStart32),
931 `IET_REL32_EXPORT` symbols (functions and asm labels), and 19 rel32
*imports*: two of them are the kernel's own symbols (patched by `LoadKernel`),
seventeen are compiler and Adam functions (`ExePrint`, `ExeFile`, `Ed`, `Lex`,
`DocSave`...) patched only when those modules load.

## 3. What the kernel touches

Everything runs in ring 0 with one identity-mapped address space; tasks are
`CTask` structures sharing it, found through `FS` (the current task) and `GS`
(the current CPU), whose bases are set with `WRMSR 0xC0000100/0xC0000101`.

| Touch point | Kernel code | How it is met here |
|---|---|---|
| BIOS INT 10h/13h/15h/1Ah | KStart16, BootDVD | not run: `runtime/main.c` writes the results (E801, E820, run level, boot block) |
| Page tables, CR0/3/4, EFER, GDT | KStart32, PageTables | built in guest memory as the kernel would, not used for translation (identity) |
| `IN`/`OUT` | everywhere | `sys_in`/`sys_out` -> `runtime/devices.c` |
| PIT 0x40-0x43, PIC 0x20/0xA0 | `TimersInit`, `IntsInit` | 1 kHz IRQ0 from the host clock; 8259 pair with ICWs, masks, EOI |
| i8042 0x60/0x64 | `SerialDev/Keyboard.HC`, `Mouse.HC` | controller + keyboard + PS/2 mouse (IntelliMouse id) |
| CMOS 0x70/0x71 | `KDate.HC` | host local time, BCD |
| VGA 0x3C4/5 (map mask), 0x3C8/9 (DAC), 0x3DA | `Display.HC`, `Adam/Gr` | planes latched on map-mask changes, palette from the DAC |
| VGA memory 0xA0000 | through `dev.uncached_alias` | ordinary guest RAM; `runtime/present.c` composes the four planes |
| ATA/ATAPI 0x1F0-0x1F7, 0x3F6 | `BlkDev/DskATA*.HC` | an ATAPI CD-ROM (PACKET: READ(12), READ CAPACITY, START/STOP...) serving the ISO |
| PCI 0xCF8/0xCFC, PCI BIOS32 | `PCIBIOS.HC` | no PCI devices; BIOS32 directory absent |
| HPET 0xFED00000, LAPIC 0xFEE00000 | `TimersInit`, `MultiProc.HC` | register shadows: a 10 MHz HPET counter from the host clock, a quiet APIC |
| `LIDT`, IDT gates, `IRETQ` | `KInts.HC`, `Sched.HC` | interrupts delivered to the lifted handler the IDT names |
| `HLT` | idle task | wait for the next interrupt on the host |
| x87 FPU, `FXSAVE`/`FXRSTOR` | all `F64` code | modelled (HolyC does all floating point on the x87) |
| multicore (APIC IPIs, MP tables) | `MultiProc.HC` | one core: no MP tables, so `mp_cnt` stays 1 |
| PC speaker 0x61 | `Snd` | gate recorded; muted (tests are always muted) |

The *uncached alias* matters: `UncachedAliasAlloc` maps the low 4 GB a second
time just above physical memory, and the kernel reaches VGA memory, the HPET and
the APIC through it. Guest memory is one 1 GB host mapping (`g_mem_base`); any
access at or above 1 GB goes to `mmio_addr`, which folds the alias back onto RAM
or onto a device's register page.

## 4. Where code is made at run time

Only the kernel is machine code on the disc - and `Compiler.BIN`, a second
`CBinFile` module that `Load()` puts in the code heap and links against the
kernel's exports. Everything else (Adam, the window manager, the editor, every
app and demo) is HolyC source compiled by that compiler while the system runs:

- `ExeFile`/`ExePutS` (`Compiler/CMain.HC`) lex and compile statement by
  statement and run each as soon as it is compiled (`#include` is just more
  statements, so StartOS.HC compiles and runs all of Adam this way);
- `COCCompile` (`Compiler/PrsLib.HC`) is the one place machine code is
  emitted: `res=MAlloc(code_size,Fs->code_heap)`, then the last optimiser pass
  (`OptPass789A(cc,reg_offsets,res,...)`) writes the bytes into it;
- forward references (`extern`, or a call to a function defined later) are
  `CHashImport`s, and `LoadOneImport`/`SysSymImportsResolve` patch the call
  sites' rel32 when the symbol appears - code is modified after it is written.

The AOT path exists too (`Cmp()`, `CCF_AOT_COMPILE`, `CAOT`, producing `.BIN`
modules like the kernel), but the OS is not built that way: Adam relies on
`#exe{}` blocks and on running each statement as it is compiled.

## 5. The approach

The rule is strict: guest bytes are never executed, and guest instructions are
never interpreted or emulated. Everything that runs is C produced by pcrecomp's
lifter. That leaves two ways to deal with the JIT.

**(a) An ahead-of-time cache of the JIT (chosen).** Lift the kernel. Run the
boot under the lift; when lifted code reaches an address no lifted function
covers, stop, name it, and record it:

- in the kernel or a captured module: a function discovery missed
  (`work/seeds.txt`, lifted by the next `tools/gen.py`);
- inside a `.BIN` module TempleOS loaded itself (a `TOSB` header around the
  address - `Compiler.BIN`): the module's bytes as they are in memory,
  relocated and linked (`work/jit/mod_<addr>.bin`). It is verified byte for
  byte (FNV-1a) the first time it is entered on later runs;
- inside a code-heap block (found from TempleOS's own heap header,
  `CMemUsed {hc, size}` with the heap control's `HcSV` signature): compiler
  output. Lifted blocks are keyed by (address, content hash); the first time
  lifted code reaches a block its hash picks the variant and activates its
  entry points. A hooked `_FREE` deactivates a block's entries, so different
  code later at the same address never reaches stale lifted code. A hooked
  `COCCompile` remembers every block compiled, so one stop captures all the
  functions compiled so far, not just the one that was called;
- anything else is new code: the run stops and says so.

Call sites a loader patches later (imports of symbols defined after a module
loads; in compiler output, every rel32 call) are lifted to read their target
from guest memory at run time, so code lifted before its imports resolved
still calls the right function afterwards.

The cost is rounds: each new top-level statement StartOS compiles and runs is
a new block, so building the cache takes many lift-build-run rounds
(`tools/capture.py` automates them). It also depends on the boot laying out
the heap the same way each time. Tasks switch only cooperatively during boot
(`IRQ_TIMER` re-enters the scheduler but resumes the same task while it is
ready), which keeps the layout stable in practice; a virtual clock (PIT, HPET
and TSC driven by a count of safe points) is the fallback if it is not.
Matching blocks by their instruction bytes with relocated fields masked, and
lifting them to read those fields from guest memory, would remove the address
dependence altogether (planned; see the handoff).

**(b) TempleOS's own AOT path (rejected as the main route).** `Cmp()` can turn
HolyC into `.BIN` modules, which could be lifted like the kernel. But producing
them needs a running compiler (so (a)'s machinery anyway, for the bootstrap),
Adam is written for JIT and does not build as modules, and the result would no
longer be the OS as shipped. It stays useful for single programs later.

So (a): one mechanism covers `Compiler.BIN` (loaded at a heap address decided at
run time - it is captured exactly like JIT output) and every JIT block.

## 6. How the lifted kernel runs

- **Lifting**: `tools/gen.py` reads the kernel from the ISO the way BootDVD
  does, relocates it for 0x7C00, resolves its self-imports, finds its 64-bit
  functions (exports with HolyC's `PUSH RBP; MOV RBP,RSP` prologue, the KStart64
  entry, function addresses taken as immediates, and recursive descent through
  calls), and lifts them with `lift64_sys.py` (pcrecomp branch `temple`).
- **lift64_sys** extends pcrecomp's lift64_cpu for ring 0: port I/O, CLI/STI/
  HLT, MSRs, control registers, LIDT/LGDT, IRETQ, INT, FXSAVE/FXRSTOR, x87
  (lift32_cpu's translations, reused), guest FS/GS bases, DF for string ops,
  bit strings for `BT`/`BTS [mem],reg`, interrupt safe points, and a check at
  every call site that the callee returned where it was called from.
- **cpu64.h** gained a based memory model (`RECOMP64_BASED`: guest address +
  `g_mem_base`) with an MMIO hook (`RECOMP64_MMIO`), so the 64-bit guest runs as
  an ordinary process on any 64-bit host.
- **Task switches**: a guest task switch is an IRETQ to another task's saved
  RIP/RSP. `runtime/tasks.c` keeps a host context (thread, one baton) per
  suspended call chain and resume points at Yield and at each delivered
  interrupt; see `docs/architecture.md`.

## 7. Handoff

State, newest first. Keep this current.

- **2026-11-19** - Repo created. ISO layout, RedSea and the LZW expander done in
  Python. Kernel relocation, function discovery (703+ functions), lift64_sys and
  the based/system cpu64.h on pcrecomp branch `temple`
  (`G:/recomp/pc/pcrecomp-temple`). Runtime: memory, devices, interrupts, task
  contexts, SDL2 presenter with its own PNG writer, boot HLE of KStart16/32.
  The lifted kernel boots through KMain to interrupt delivery; the seed loop
  (`tools/iterate.sh`) is filling in functions reached only through pointers.
- Next: first light (KMain's raw "TempleOS V5.03" text in the window), then the
  CD found by the ATAPI model, `Load("Compiler")`, and the JIT cache (virtual
  clock, COCCompile hook, block capture and lifting).
