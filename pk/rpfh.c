#include "rpfh.h"
#include "frontend.h"

void rpfh_init()
{
  // create virtual mapping for RPFH I/O area
  __map_kernel_range(PFA_BASE, PFA_BASE, RISCV_PGSIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
}

void rpfh_publish_freeframe(uintptr_t paddr)
{
  *PFA_FREEFRAME = paddr;
}

void rpfh_evict_page(void const *page)
{
  pte_t *page_pte = walk((uintptr_t) page);
  
  *PFA_EVICTPAGE = (uintptr_t)page;
  *PFA_EVICTPAGE = va2pa(page_pte);
}

uintptr_t rpfh_poll_evict()
{
  uintptr_t evicted = (uintptr_t)*PFA_EVICTPAGE;
  return evicted;
}

void *rpfh_pop_newpage()
{
  void *newpage = *PFA_NEWPAGE;
  return newpage;
}
