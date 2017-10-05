#include "pfa.h"
#include "frontend.h"

void pfa_init()
{
  // create virtual mapping for RPFH I/O area
  __map_kernel_range(PFA_BASE, PFA_BASE, RISCV_PGSIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
}

uint64_t pfa_check_freeframes(void) {
  return *PFA_FREESTAT;
}

void pfa_publish_freeframe(uintptr_t paddr)
{
  assert(*PFA_FREESTAT > 0);
  *PFA_FREEFRAME = paddr;
}

void pfa_evict_page(void const *page)
{
  printk("Evicting va:%lx  pa:%lx\n", page, va2pa(page));
  *PFA_EVICTPAGE = (uintptr_t)page;
  *PFA_EVICTPAGE = va2pa(page);
  
  pte_t *page_pte = walk((uintptr_t) page);
  /* At the moment, the page_id is just the page-aligned vaddr */
  *page_pte = pfa_mk_remote_pte((uintptr_t)page >> PFA_PAGEID_SHIFT, *page_pte);
  flush_tlb();
}

bool pfa_poll_evict(void)
{ 
  int poll_count = 0;
  while(*PFA_EVICTSTAT < PFA_EVICT_MAX) {
    if(poll_count++ == MAX_POLL_ITER) {
      printk("Polling for eviction completion took too long\n");
      return false;
    }
  }

  return true;
}

void *pfa_pop_newpage()
{
  return (void*)(*PFA_NEWPAGE << PFA_PAGEID_SHIFT);
}

uint64_t pfa_check_newpage()
{
  return *PFA_NEWSTAT;
}

pte_t pfa_mk_remote_pte(uint64_t page_id, pte_t orig_pte)
{
  pte_t rem_pte;

  /* page_id needs must fit in upper bits of PTE */
  assert(page_id >> (64 - PFA_PAGEID_SHIFT) == 0);


  /* Page ID */
  rem_pte = page_id << PFA_PAGEID_SHIFT;
  /* Protection Bits */
  rem_pte |= (orig_pte & ~(-1 << PTE_PPN_SHIFT)) << PFA_PROT_SHIFT;
  /* Valid and Remote Flags */
  rem_pte |= PFA_REMOTE;

  return rem_pte;
}
