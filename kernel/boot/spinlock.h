/*
 * Zeos — Coarse spinlocks for SMP shared state.
 *
 * One primitive: zeos_spinlock_t. Two flavors:
 *   spin_lock / spin_unlock        — plain test-and-set + pause loop.
 *   spin_lock_irqsave / restore    — also save+disable IF for the
 *                                    duration of the critical section
 *                                    so an ISR running on the same CPU
 *                                    can't deadlock against us.
 *
 * These are the SMP audit primitive. We use coarse locks (one per
 * shared structure) rather than fine-grained per-field locks because
 * the shared structures (chain registry, masq journal, persistence,
 * VAULT, kprint serial) are touched at a rate where lock-grain
 * doesn't dominate. Fine-grained locking is a future pass.
 *
 * IMPORTANT: do NOT hold any of these spinlocks across a chain_resolve
 * call. The LAPIC-timer preempt path uses longjmp to abandon a hung
 * resolve; if a lock is held when the longjmp fires, the lock leaks.
 * Lock just long enough to mutate / snapshot the shared structure;
 * release before invoking subsystem code.
 */

#ifndef ZEOS_SPINLOCK_H
#define ZEOS_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t v;
} zeos_spinlock_t;

#define ZEOS_SPINLOCK_INIT { 0 }

static inline void spin_lock(zeos_spinlock_t *l)
{
    while (__sync_lock_test_and_set(&l->v, 1u)) {
        while (l->v) __asm__ volatile("pause");
    }
}

static inline int spin_trylock(zeos_spinlock_t *l)
{
    return __sync_lock_test_and_set(&l->v, 1u) == 0u;
}

static inline void spin_unlock(zeos_spinlock_t *l)
{
    __sync_lock_release(&l->v);
}

static inline uint64_t zeos_irq_save_disable(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void zeos_irq_restore(uint64_t flags)
{
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

/* IRQ-safe variants. Use these for locks that may be acquired from
 * both task context and an ISR on the same CPU (kprint, anything the
 * preempt-timer ISR may touch). */
static inline uint64_t spin_lock_irqsave(zeos_spinlock_t *l)
{
    uint64_t f = zeos_irq_save_disable();
    while (__sync_lock_test_and_set(&l->v, 1u)) {
        /* IF is already off; spin without re-enabling. */
        while (l->v) __asm__ volatile("pause");
    }
    return f;
}

static inline void spin_unlock_irqrestore(zeos_spinlock_t *l, uint64_t flags)
{
    __sync_lock_release(&l->v);
    zeos_irq_restore(flags);
}

#endif /* ZEOS_SPINLOCK_H */
