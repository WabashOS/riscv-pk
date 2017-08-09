#ifndef _RPFH_H
#define _RPFH_H

#include "mmap.h"
#include "pk.h"
#include "atomic.h"
#include <stdint.h>

#define PFA_BASE           0x2000
#define PFA_FREEFRAME      ((volatile uintptr_t*)(PFA_BASE))
#define PFA_FREESTAT       ((volatile uintptr_t*)(PFA_BASE + 8))
#define PFA_EVICTPAGE      ((volatile uintptr_t*)(PFA_BASE + 16))
#define PFA_EVICTSTAT      ((volatile uintptr_t*)(PFA_BASE + 24))
#define PFA_NEWPAGE        ((void** volatile)(PFA_BASE + 32))

/* PFA Limits (implementation-specific) */
#define PFA_FREE_MAX 64
#define PFA_NEW_MAX  64
#define PFA_EVICT_MAX 1

void rpfh_init();
uint64_t rpfh_check_freeframes(void);
void rpfh_publish_freeframe(uintptr_t paddr);
void rpfh_evict_page(void const *page);

/* Returns vaddr of last evicted page or 0 if page still being evicted. 
 * uintptr_t to discourage dereferencing. */
uintptr_t rpfh_poll_evict();

/* returns vaddr of most recently fetched page, or NULL if no page was fetched
 * since the last call */
void *rpfh_pop_newpage();


#endif
