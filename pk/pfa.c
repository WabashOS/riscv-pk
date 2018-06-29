#include "pfa.h"
#include "nic.h"
#include "frontend.h"

void pfa_init()
{
  printk("Initializing PFA\n");
  // create virtual mapping for PFA I/O area
  __map_kernel_range(PFA_BASE, PFA_BASE, RISCV_PGSIZE, PROT_READ|PROT_WRITE|PROT_EXEC);

  printk("Getting MAC from NIC for loopback config\n");
  __map_kernel_range(NIC_BASE, NIC_BASE, RISCV_PGSIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
  uint64_t mac = *NIC_MACADDR;

  printk("setting mac in PFA\n");
  *PFA_DSTMAC = mac;
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
  static pgid_t pgid = PFA_INIT_PGID;
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

  return pgid++;
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
  return (pgid_t)(*PFA_NEWPGID);
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
