/*
 * Zeos — MSI-X interrupt delivery
 *
 * Vector pool: 0x40-0x7F (64 vectors total).
 * Each vector has a void(*)(void) handler installed via the pool.
 */

#ifndef ZEOS_MSIX_H
#define ZEOS_MSIX_H

#include <stdint.h>

struct pci_device;

#define MSIX_VECTOR_BASE  0x40
#define MSIX_VECTOR_TOP   0x80
#define MSIX_VECTOR_COUNT (MSIX_VECTOR_TOP - MSIX_VECTOR_BASE)

void msix_init(void);
int  msix_alloc_vector(void (*handler)(void));
void msix_free_vector(int vec);
int  msix_enable(struct pci_device *dev, int num_vectors);
int  msix_set_handler(struct pci_device *dev, int entry, void (*handler)(void));
void msix_disable(struct pci_device *dev);
int  msix_free_count(void);
void msix_dispatch(int vec);

#endif /* ZEOS_MSIX_H */
