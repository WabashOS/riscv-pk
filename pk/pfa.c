#include "pfa.h"
#include "nic.h"
#include "frontend.h"

void pfa_init()
{
  printk("Initializing PFA\n");
  // create virtual mapping for PFA I/O area
  __map_kernel_range(PFA_BASE, PFA_BASE, RISCV_PGSIZE, PROT_READ|PROT_WRITE|PROT_EXEC);

  printk("Getting MAC from NIC\n");
  __map_kernel_range(NIC_BASE, NIC_BASE, RISCV_PGSIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
  uint64_t mac = *NIC_MACADDR;

  printk("setting mac in PFA\n");
  *PFA_DSTMAC = mac + (1L << 40);
  return;
}

uint64_t pfa_check_freeframes(void) {
  return *PFA_FREESTAT;
}

void pfa_publish_freeframe(uintptr_t paddr)
{
  assert(*PFA_FREESTAT > 0);
  *PFA_FREEFRAME = paddr;
}

pgid_t pfa_evict_page(void const *page)
{
  static pgid_t pgid = PFA_INIT_RPN;
  
  pfa_evict_page_pgid(page, pgid);

  return pgid++;
}

void pfa_evict_page_pgid(void const *page, pgid_t pgid)
{
  uintptr_t paddr = va2pa(page);

  /* pfn goes in first 36bits, pgid goes in upper 28
   * See pfa spec for details. */
  uint64_t evict_val = paddr >> RISCV_PGSHIFT;
  assert(evict_val >> 36 == 0);
  assert(pgid >> 28 == 0);
  evict_val |= (uint64_t)pgid << 36;
  *PFA_EVICTPAGE = evict_val;

  pte_t *page_pte = walk((uintptr_t) page);
  *page_pte = pfa_mk_remote_pte(pgid, *page_pte);
  flush_tlb();
}

bool pfa_poll_evict(void)
{
  int poll_count = 0;
  volatile uint64_t *evictstat = PFA_EVICTSTAT;
  while(*evictstat < PFA_EVICT_MAX) {
    if(poll_count++ == MAX_POLL_ITER) {
      printk("Polling for eviction completion took too long\n");
      return false;
    }
  }

  return true;
}

pgid_t pfa_pop_newpage()
{
  /*XXX Discard the vaddr for now */
  volatile uint64_t vaddr = *PFA_NEWVADDR;
  pgid_t pgid = (pgid_t)(*PFA_NEWPGID);

  assert(pfa_pgid_sw(pgid) == PFA_PAGEID_SW_MAGIC);
  return pfa_pgid_rpn(pgid);
}

uint64_t pfa_check_newpage()
{
  return *PFA_NEWSTAT;
}

/* Drain the new page queue without checking return values */
void pfa_drain_newq(void)
{
  uint64_t nnew = pfa_check_newpage();
  while(nnew) {
    pfa_pop_newpage();
    nnew--;
  }
  return;
}

pte_t pfa_mk_remote_pte(uint64_t rpn, pte_t orig_pte)
{
  pte_t rem_pte;

  /* rpn needs must fit in upper bits of PTE */
  assert(rpn >> (PFA_PAGEID_RPN_BITS) == 0);


  /* Page ID */
  rem_pte = rpn << PFA_PAGEID_SHIFT;
  rem_pte |= PFA_PAGEID_SW_MAGIC << (PFA_PAGEID_SHIFT + PFA_PAGEID_RPN_BITS);

  /* Protection Bits */
  rem_pte |= (orig_pte & ~(-1 << PTE_PPN_SHIFT)) << PFA_PROT_SHIFT;
  /* Valid and Remote Flags */
  rem_pte |= PFA_REMOTE;

  return rem_pte;
}

inline bool pfa_is_newqueue_empty(void)
{
  return *PFA_NEWSTAT == 0;
}

inline bool pfa_is_evictqueue_empty(void)
{
  return *PFA_EVICTSTAT == PFA_EVICT_MAX;
}

inline bool pfa_is_freequeue_empty(void)
{
  return *PFA_FREESTAT == PFA_FREE_MAX;
}
