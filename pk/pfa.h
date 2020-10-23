#ifndef _RPFH_H
#define _RPFH_H

#include "mmap.h"
#include "pk.h"
#include "atomic.h"
#include <stdint.h>

#define PFA_BASE           0x10017000
// #define PFA_BASE           0x2000
#define PFA_FREEFRAME      ((volatile uintptr_t*)(PFA_BASE))
#define PFA_FREESTAT       ((volatile uint64_t*)(PFA_BASE + 8))
#define PFA_EVICTPAGE      ((volatile uint64_t*)(PFA_BASE + 16))
#define PFA_EVICTSTAT      ((volatile uint64_t*)(PFA_BASE + 24))
#define PFA_NEWPGID        ((volatile uint64_t*)(PFA_BASE + 32))
#define PFA_NEWVADDR       ((volatile uint64_t*)(PFA_BASE + 40))
#define PFA_NEWSTAT        ((volatile uint64_t*)(PFA_BASE + 48))
#define PFA_DSTMAC         ((volatile uint64_t*)(PFA_BASE + 56))

/* PFA Limits (implementation-specific) */
#define PFA_QUEUES_SIZE 256
#define PFA_FREE_MAX (PFA_QUEUES_SIZE)
#define PFA_NEW_MAX  (PFA_QUEUES_SIZE)
#define PFA_EVICT_MAX (PFA_QUEUES_SIZE)

/* PFA PTE Bits */
#define PFA_PAGEID_SHIFT     12
#define PFA_PAGEID_RPN_BITS  28 /* size of remote page number part of pgid */
#define PFA_PAGEID_SW_BITS   24 /* size of SW reserved part of pgid */
#define PFA_PROT_SHIFT       2
#define PFA_REMOTE           0x2

#define pfa_pgid_rpn(PGID) (PGID & ((1 << PFA_PAGEID_RPN_BITS) - 1))
#define pfa_pgid_sw(PGID) (PGID >> PFA_PAGEID_RPN_BITS)

/* Magic number used in the software reserved bits of the pgid for testing */
// #define PFA_PAGEID_SW_MAGIC 0xCAFEl
#define PFA_PAGEID_SW_MAGIC 0x0l

#define pte_is_remote(pte) (!(pte & PTE_V) && (pte & PFA_REMOTE))


/* Max time to poll for completion for PFA stuff. Assume that the device is
 * broken if you have to poll this many times. Currently very conservative. */
#define MAX_POLL_ITER 1024*1024

typedef enum {
  PFA_EXP_NEWVADDR_FAULT, /* test_interleaved_newq_fault */
  PFA_EXP_NEWPGID_FAULT, /* test_interleaved_newq_fault */
  PFA_EXP_TESTN, /* test_n */
  PFA_EXP_EMPTYQ, /* test_emptyq */
  PFA_EXP_OTHER /* All other experiments that don't need special handling */
} pfa_exp_t;

/* Page ID */
typedef uint64_t pgid_t;
#define PFA_INIT_RPN 4 //Remote page numbers will start at this value and go up
#define PFA_MAX_RPN ((1 << 22) - 1)

/* Info for a page that will be made remote */
typedef struct rem_pg {
  /* The value stored in the page */
  uint64_t  val;
  /* Pointer to the page in local mem */
  uint64_t  *ptr;

  /* Virtual and physical addresses */
  uintptr_t vaddr;
  uintptr_t paddr;

  pgid_t pgid;
} rem_pg_t;

/* ==============
 * Globals (defined in pfa.c)
 * ==============
 */
extern pfa_exp_t current_exp;

/* Globals used by test_n */
extern int64_t test_n_nrem;

/* Globals used by test_interleaved_newq_fault and test_emptyq */
extern volatile pgid_t test_pgid;
extern volatile uint64_t test_vaddr;
extern uint64_t test_paddr;

/* Turn a regular pte into a remote pte with page_id */
pte_t pfa_mk_remote_pte(pgid_t page_id, pte_t orig_pte);

void pfa_init(void);
uint64_t pfa_check_freeframes(void);
void pfa_publish_freeframe(uintptr_t paddr);

/* Evict a page and return the pgid that was used for it.
 * Page ids increase monotonically */
void pfa_evict_page_pgid(void const *page, pgid_t pgid);
pgid_t pfa_evict_page(void const *page);

/* Blocks (spin) until all pages in evictq are successfully evicted */
bool pfa_poll_evict(void);

/* returns vaddr of most recently fetched page, or NULL if no page was fetched
 * since the last call */
pgid_t pfa_pop_newpage(void);

/* Returns the number of pending free pages */
uint64_t pfa_check_newpage(void);

/* Pop all pages off new page queue. Don't check the results */
void pfa_drain_newq(void);

bool pfa_is_newqueue_empty(void);

bool pfa_is_evictqueue_empty(void);

bool pfa_is_freequeue_empty(void);

void check_pfa_clean(void);

/* rem_pg_t based functions. These automate more and should be used whenever
 * possible. Lower-level functions should be used only when needed for the test */
void alloc_rem_pg(rem_pg_t *pg);
void evict_full_rem_pg(rem_pg_t *pg);
void fetch_rem_pg(rem_pg_t *pg);
void pop_new_rem_pg(rem_pg_t *pg);
#endif
