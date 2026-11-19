/* temple.h - the host side of the TempleOS recompilation.
 *
 * The guest is TempleOS's own machine code, lifted to C by pcrecomp's
 * lift64_sys. This layer is the machine it runs on: guest memory, the CPU's
 * system instructions, interrupts, the devices (PIT, PIC, i8042, CMOS, VGA,
 * an ATAPI drive holding the ISO, HPET), and the host contexts that guest
 * task switches land on. See docs/architecture.md.
 */
#ifndef TEMPLE_H
#define TEMPLE_H
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include "cpu64.h"

#define GUEST_RAM      (1ull << 30)          /* 1 GB of guest physical memory */
#define ALIAS_BASE     GUEST_RAM             /* dev.uncached_alias: the low 4 GB again */
#define ALIAS_END      (ALIAS_BASE + (4ull << 30))

extern CPU g_cpu;
extern int g_trace;
extern const char *g_work;                   /* the work/ directory */

/* guest memory, by guest address */
static inline uint8_t *gptr(uint64_t a) { return g_mem_base + a; }
uint64_t mem_init(void);

/* dispatch.c / generated tables */
typedef void (*lifted_fn)(CPU *);
lifted_fn lookup(uint64_t a);
const char *sym_near(uint64_t a, uint64_t *off);
void dispatch_miss(CPU *c, uint64_t a);
void guest_backtrace(CPU *c);

/* machine.c */
void machine_init(void);
void irq_raise(int irq);
uint64_t host_ns(void);
void out_text(const char *fmt, ...);

/* tasks.c: host contexts for guest task switches */
void tasks_start(CPU *c, uint64_t rip);       /* never returns: runs the guest */
void hook_yield(CPU *c);
void resume_point_irq(CPU *c, void (*deliver)(CPU *, uint64_t));
void tasks_iret(CPU *c, uint64_t rip);

/* devices.c */
uint64_t dev_in(uint32_t port, int size);
void dev_out(uint32_t port, uint64_t v, int size);
uintptr_t dev_mmio(uint64_t phys, int size, int write);
void dev_tick(void);
int  iso_open(const char *path);

/* present.c */
int  present_init(int hidden);
void present_frame(void);
int  present_png(const char *path);
void present_poll(void);
void vga_planes_latch(int new_mask);
extern uint8_t g_vga_pal[16][3];
extern int g_quit;
#endif
