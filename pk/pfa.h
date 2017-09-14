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
#define PFA_NEWPAGE        ((volatile uintptr_t*)(PFA_BASE + 32))
#define PFA_NEWSTAT        ((volatile uintptr_t*)(PFA_BASE + 40))

/* PFA Limits (implementation-specific) */
#define PFA_FREE_MAX 64
#define PFA_NEW_MAX  64
#define PFA_EVICT_MAX 1

/* PFA PTE Bits */
#define PFA_PAGEID_SHIFT 12
#define PFA_PROT_SHIFT   2
#define PFA_REMOTE       0x2

#define pte_is_remote(pte) (!(pte & PTE_V) && (pte & PFA_REMOTE))

/* Turn a regular pte into a remote pte with page_id */
pte_t pfa_mk_remote_pte(uint64_t page_id, pte_t orig_pte);

void pfa_init(void);
uint64_t pfa_check_freeframes(void);
void pfa_publish_freeframe(uintptr_t paddr);
void pfa_evict_page(void const *page);

/* Returns vaddr of last evicted page or 0 if page still being evicted. 
 * uintptr_t to discourage dereferencing. */
uintptr_t pfa_poll_evict(void);

/* returns vaddr of most recently fetched page, or NULL if no page was fetched
 * since the last call */
void* pfa_pop_newpage(void);

/* Returns the number of pending free pages */
uint64_t pfa_check_newpage(void);

#endif
