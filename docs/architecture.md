# How templeos-recomp works

TempleOS is a complete 64-bit operating system written by one person: a
kernel, the HolyC language and its compiler, a document format and editor,
2D/3D graphics, sound, games and its own documentation. On a PC it boots from
a CD and owns the whole machine. This project runs it as an ordinary program
on Windows, Linux and macOS, without an emulator: TempleOS's machine code is
translated to C ahead of time, compiled for the host, and given a small model
of the PC it expects.

    ┌───────────────────────────────────────────────────────────────────────┐
    │                       one host process (x86-64 or arm64)               │
    │                                                                        │
    │   TempleOS's own code, lifted to C by pcrecomp (lift64_sys)            │
    │  ┌──────────────────────────────────────────────────────────────────┐  │
    │  │  Kernel.BIN    scheduler, memory, interrupts, ATA/ATAPI, RedSea, │  │
    │  │                keyboard/mouse drivers, raw text, KMain           │  │
    │  ├──────────────────────────────────────────────────────────────────┤  │
    │  │  Compiler.BIN  HolyC: lexer, parser, optimiser, x86-64 back end  │  │
    │  ├──────────────────────────────────────────────────────────────────┤  │
    │  │  JIT output    Adam, window manager, editor, apps, demos         │  │
    │  │                (captured, lifted, checked by content - §4)       │  │
    │  └──────────────────────────────────────────────────────────────────┘  │
    │       │ IN/OUT     │ IRETQ, HLT      │ MMIO        │ dispatch          │
    │       ▼            ▼                 ▼             ▼                   │
    │  ┌──────────┐ ┌────────────────┐ ┌──────────┐ ┌───────────────────┐    │
    │  │devices.c │ │ machine.c      │ │ HPET,    │ │ dispatch.c        │    │
    │  │PIT PIC   │ │ interrupts,    │ │ LAPIC    │ │ guest address ->  │    │
    │  │i8042 CMOS│ │ MSRs, FXSAVE   │ │ shadows  │ │ lifted function;  │    │
    │  │VGA ATAPI │ │ tasks.c:       │ │          │ │ capture.c for     │    │
    │  │          │ │ task switches  │ │          │ │ code made at run  │    │
    │  └────┬─────┘ └───────┬────────┘ └──────────┘ └───────────────────┘    │
    └───────┼───────────────┼────────────────────────────────────────────────┘
            ▼               ▼
      the user's ISO   host threads and clock      an SDL2 window (present.c)
      (read-only)      (one runs at a time)        640x480, 16 colours

## 1. Static recompilation

`tools/gen.py` reads the kernel from the ISO the way the boot loader does,
applies its relocations for the address it boots at (0x7C20), and finds its
functions: the exports in its patch table, recursive descent through calls and
jumps, HolyC switch tables (`MOVSX r,[idx+table]; ADD r,base; JMP r`), and
function addresses the code takes as constants. pcrecomp's `lift64_sys.py`
writes each function as a C function over a CPU structure:

        kernel bytes                                generated C
     ┌──────────────────────────┐   lift     ┌──────────────────────────────┐
     │ 55          PUSH RBP     │ ─────────▶ │ void L_00000000A7C1(CPU *c) {│
     │ 48 8B EC    MOV RBP,RSP  │            │   push64(c, c->rbp);         │
     │ 48 8B 45 10 MOV RAX,[..] │            │   SET64(c->rbp, c->rsp);     │
     │ FF D0       CALL RAX     │            │   SET64(c->rax, rd64(...));  │
     └──────────────────────────┘            │   dispatch(c, c->rax); ...   │
                                             └──────────────────────────────┘

Guest memory holds the original bytes as data only. It is one host mapping
(1 GB) at `g_mem_base`; a guest address is an offset into it, so the 64-bit
guest runs on any 64-bit host. Nothing is ever executed from it, and nothing is
interpreted: an address with no lifted function stops the run and is named.

## 2. Booting without a BIOS

    BootDVD ──▶ KStart16 ──▶ KStart32 ──▶ │ KStart64 ──▶ KMain ──▶ StartOS
    (16-bit)    (16-bit)     (32-bit)     │ (64-bit, lifted from here on)
       └─── runtime/main.c does their work ┘

The 16- and 32-bit start-up talks to the BIOS (video mode, memory maps, PCI)
and prepares long mode. `runtime/main.c` performs the same writes in C: it
loads the kernel at 0x7C00, patches its absolute addresses, writes the E801
and E820 memory maps, the run level, the boot block, the page tables and the
first block pool, and starts the lifted code at KStart64 with the stack
KStart32 left.

## 3. Interrupts and task switches

TempleOS runs everything in ring 0 and switches tasks by loading another
task's registers and executing IRETQ to its saved RIP and RSP. A lifted call is
a C call, so each task's call chain lives on a host stack, and IRETQ cannot
simply jump. Instead (`runtime/tasks.c`):

      task A                          task B
      ──────                          ──────
      ... call Yield                  (suspended in Yield on its own
      hook_yield: resume point          host stack, resume point
        (rip=_RET, rsp=A's)             (rip=_RET, rsp=B's))
      lifted _YIELD saves A,
      picks B, IRETQ ──────────────▶  tasks_iret finds B's point:
      A's host stack waits             wake B's context, longjmp to it;
                                       B returns from Yield

- A *resume point* is a guest (rip, rsp) and a `jmp_buf`. Yield registers one
  (its saved rip is the kernel's `_RET`), and so does every interrupt this
  layer delivers (its frame's rip is a token unique to that delivery, which the
  timer handler saves as the task's rip if it preempts).
- IRETQ looks the (rip, rsp) up. On this host stack: `longjmp`. On another:
  hand that context the baton and wait. Nowhere: the task has never run, and a
  new context starts it with `dispatch(rip)`.
- Contexts are host threads, but exactly one runs at a time, so the CPU state
  is one shared structure. A context with no resume points left can never be
  resumed and exits.

Interrupts are delivered at safe points the lifter marks (every call and every
backward branch): if one is pending and IF is set, the runtime pushes the
frame the CPU would, clears IF and calls the lifted handler the IDT names.
`HLT` waits on the host for the next one.

## 4. Code made at run time

Only the kernel is on the disc as machine code. `Load("Compiler")` puts
`Compiler.BIN` at a heap address and links it, and from then on HolyC source
becomes machine code in the code heap as the system runs (`COCCompile` in
`Compiler/PrsLib.HC`).

     run ──▶ lifted code reaches an address with no lifted function
                 │
                 ├─ in the kernel or a captured module: a function discovery
                 │  missed ─▶ work/seeds.txt
                 ├─ inside a module TempleOS loaded (a CBinFile in memory):
                 │  its bytes, relocated and linked ─▶ work/jit/mod_<addr>.bin
                 └─ anything else: new code ─▶ stop, and say so
             tools/gen.py lifts what was recorded; the next run gets further.

A captured module is checked byte for byte (FNV-1a over its image) the first
time it is entered, so lifted code is only ever used for the exact bytes it
was made from. Call sites that a loader patches later (imports of symbols
defined after the module loads) are lifted to read their target from guest
memory at run time.

## 5. The machine

| Device | Model |
|---|---|
| PIT + PIC | 1 kHz IRQ0 from the host clock; two 8259s with init words, masks and EOI |
| i8042 | controller commands, keyboard (set 1 via translation), PS/2 mouse with the IntelliMouse id |
| CMOS | the host's local time, in BCD |
| VGA | sequencer map mask (the four planes are latched when it changes), DAC palette; memory at 0xA0000 is guest RAM |
| ATAPI CD-ROM | primary master: IDENTIFY's ATAPI signature, PACKET with READ(10/12), READ CAPACITY, START/STOP and friends, serving the ISO read-only |
| HPET | a 10 MHz counter from the host clock |
| local APIC | a quiet register page: one CPU |
| PCI | empty configuration space |
| PC speaker | the gate bit is recorded; sound is muted for now |

## 6. Building

    tools/gen.py TempleOS.ISO   ->  src/gen/*.c   (lifted kernel, modules, tables)
    cmake + a C compiler        ->  templeos      (runtime + generated C + SDL2)

See the README for the commands, and `docs/survey.md` for the reasoning behind
each choice.
