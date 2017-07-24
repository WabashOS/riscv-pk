#ifndef _RPFH_H
#define _RPFH_H

#include "mmap.h"
#include "pk.h"
#include "atomic.h"
#include <stdint.h>

#define PFA_BASE           0x2000
#define PFA_FREEFRAME      ((volatile uintptr_t*)(PFA_BASE))
#define PFA_EVICTPAGE      ((volatile uintptr_t*)(PFA_BASE + 8))
#define PFA_NEWPAGE        ((void** volatile)(PFA_BASE + 16))

void rpfh_init();
void rpfh_publish_freeframe(uintptr_t paddr);
void rpfh_evict_page(void const *page);
void *rpfh_pop_newpage();


#endif
