/* temple_lift.h - what the lifted TempleOS code is compiled against. */
#ifndef TEMPLE_LIFT_H
#define TEMPLE_LIFT_H
#include <stdint.h>
void recomp_todo(uint64_t va, const char *what);
#define RECOMP_TODO(va, what) recomp_todo((uint64_t)(va), (what))
#include "cpu64.h"
#endif
