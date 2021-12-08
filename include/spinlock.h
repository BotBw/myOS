#ifndef SPINLOCK_H
#define SPINLOCH_H

#include "types.h"
#include "proc.h"
#include "panic.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

struct spinlock {
  int locked;
  // For debugging:
  const char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
  uint pcs[10];      // The call stack (an array of program counters)
                     // that locked the lock.
};

void acquire(struct spinlock *);
void release(struct spinlock *);
void initlock(struct spinlock *, const char*);

#endif