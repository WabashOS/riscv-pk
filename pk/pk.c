#include "pk.h"
#include "mmap.h"
#include "boot.h"
#include "elf.h"
#include "mtrap.h"
#include "frontend.h"
#include "vm.h"
#include "atomic.h"
#include "bits.h"
#include "pfa.h"
#include <stdbool.h>
#include <stdlib.h>

elf_info current;
long disabled_hart_mask;

bool queues_empty() {
  bool ret = true;

  if (!pfa_is_newqueue_empty()) {
    printk("new queue is not empty\n");
    ret = false;
  }

  if (!pfa_is_evictqueue_empty()) {
    printk("evict queue is not empty\n");
    ret = false;
  }

  if (!pfa_is_freequeue_empty()) {
    printk("free queue is not empty\n");
    ret = false;
  }

  return ret;
}

/* Test one page. Evict, publish frame as free, then touch and check newpage */
bool test_one(bool flush)
{
  printk("test_one: %s\n", flush ? "Flush TLB" : "Don't Flush TLB");

  /* Volatile to force multiple queries to memory */
  volatile uint8_t *page = (volatile uint8_t*) page_alloc();
  uintptr_t paddr = va2pa((void*)page);
  /* we use index 10 to make sure the PFA works even with unaligned accesses */
  page[10] = 17;

  printk("Evicting\n");
  pgid_t pgid = pfa_evict_page((void*)page);
  if(flush) {
    flush_tlb();
  }

  printk("Polling for eviction\n");
  if(!pfa_poll_evict())
    return false;

  if(flush) {
    flush_tlb();
  }

  printk("Pushing free frame\n");
  pfa_publish_freeframe(paddr);
  if(flush) {
    flush_tlb();
  }

  /* Force rmem fault */
  printk("Bringing in page\n");
  page[10] = 42;
  if(flush) {
    flush_tlb();
  }

  printk("Checking newstat\n");
  uint64_t nnew = pfa_check_newpage();
  if(nnew != 1) {
    printk("New page queue reporting wrong number of new pages: %ld\n", nnew);
    return false;
  }

  printk("Draining newq\n");
  pgid_t newpage = pfa_pop_newpage();
  if(newpage != pgid) {
    printk("Newpage id (%d) doesn't match fetched page (%d)\n", newpage, pgid);
    return false;
  }
  if(flush) {
    flush_tlb();
  }

  printk("Double checking page tables and page values\n");
  pte_t newpte = *walk((uintptr_t)page);
  if(pte_is_remote(newpte)) {
    printk("PTE Still marked remote!\n");
    return false;
  }

  printk("page[10]=%d\n", page[10]);

  if(page[10] != 42) {
    if(page[10] == 17) {
      printk("Page didn't get faulting write\n");
      return false;
    } else {
      printk("Page has garbage\n");
      return false;
    }
  }

  if (!queues_empty()) {
    return false;
  }

  printk("test_one success\n\n");
  return true;
}

bool test_read_allbytes()
{
  printk("test_read_allbytes\n");

  char *p0 = (char *) page_alloc(); // evicted page
  char *p1 = (char *) page_alloc(); // free page
  uintptr_t p0_paddr = va2pa((void*)p0);
  uintptr_t p1_paddr = va2pa((void*)p1);
  pte_t p0_pte = *walk((uintptr_t)p0);
  pte_t p1_pte = *walk((uintptr_t)p1);

  printk("p0: vaddr=%p paddr=%p pte=%lx pteaddr=%p\n", p0, p0_paddr, p0_pte, &p0_pte);
  printk("p1: vaddr=%p paddr=%p pte=%lx pteaddr=%p\n", p1, p1_paddr, p1_pte, &p1_pte);

  for (int i = 0; i < 4096; ++i) {
    p0[i] = i % 255;
  }

  pgid_t pgid = pfa_evict_page((void*) p0);

  if(!pfa_poll_evict())
    return false;

  pfa_publish_freeframe(p1_paddr);

  /* fetch */
  for (int i = 0; i < 4096; i++) {
    if (p0[i] != i % 255) {
      printk("p0[%d]=%d didn't match expected =%d\n", i, p0[i], i % 255);
      return false;
    }
  }
  printk("\n");

  p0_pte = *walk((uintptr_t) p0);
  p0_paddr = va2pa((void*) p0);

  printk("after fetch p0: vaddr=%p paddr=%p pte=%lx pteaddr=%p\n", p0, p0_paddr, p0_pte, &p0_pte);

  if (p0_pte != p1_pte || p0_paddr != p1_paddr) {
    printk("ptes or paddrs were not updated properly\n");
    return false;
  }

  pfa_pop_newpage();

  if (!queues_empty()) {
    return false;
  }

  printk("test_read_allbytes success\n\n");
  return true;
}

/* Test two pages. Evict both, publish both frames, touch in opposite order.
 * This tests two things:
 * 1. Can all the queues handle multiple things.
 * 2. Is the remote data really being fetched (that's why we touch in reverse
 *    order).
 */
bool test_two()
{
  printk("test_two:\n");
  /* Pages */
  void *p0 = (void*) page_alloc();
  void *p1 = (void*) page_alloc();

  pte_t pte0 = *walk((uintptr_t)p0);
  pte_t pte1 = *walk((uintptr_t)p1);
  printk("pte0=%lx pte1=%lx\n", pte0, pte1);

  /* Frames */
  uintptr_t f0 = va2pa(p0);
  uintptr_t f1 = va2pa(p1);

  /* Values */
  uint8_t v0 = 17;
  uint8_t v1 = 42;
  *(uint8_t*)p0 = v0;
  *(uint8_t*)p1 = v1;

  printk("Starting test:\n");
  printk("(V0,V1): (%d,%d)\n", *(uint8_t*)p0, *(uint8_t*)p1);
  printk("(P0,P1): (%p,%p)\n", p0, p1);
  printk("(F0,F1): (%lx,%lx)\n", f0, f1);

  /* pg IDs (i*) */
  pgid_t i0 = pfa_evict_page(p0);
  if(!pfa_poll_evict())
    return false;

  pgid_t i1 = pfa_evict_page(p1);
  if(!pfa_poll_evict())
    return false;

  pfa_publish_freeframe(f0);
  pfa_publish_freeframe(f1);

  if(!pfa_poll_evict())
    return false;

  /* Values */
  uint8_t v0_after, v1_after;

  /* Fetch in reverse order of publishing.
   * This should result in the vaddrs switching paddrs. */
  v1_after = *(uint8_t*)p1;
  v0_after = *(uint8_t*)p0;

  pte0 = *walk((uintptr_t)p0);
  pte1 = *walk((uintptr_t)p1);
  printk("pte0=%lx pte1=%lx\n", pte0, pte1);

  /* Get newpage info */
  pgid_t n1 = pfa_pop_newpage();
  pgid_t n0 = pfa_pop_newpage();

  /* Check which frame the page was fetched into. We expect the pages to have
   * swapped frames since we fetched in reverse order and freeq is a FIFO */
  uintptr_t f0_after = va2pa((void*)p0);
  uintptr_t f1_after = va2pa((void*)p1);

  if(f0_after != f1 || f1_after != f0 ||
     v1_after != v1 || v0_after != v0 ||
     i0 != n0       || i1 != n1)
  {
    printk("Test Failed:\n");
    printk("Expected:\n\n");
    printk("(V0,V1): (%d,%d)\n", v0, v1);
    printk("(P0,P1): (%p,%p)\n", p0, p1);
    printk("(F0,F1): (0x%lx,0x%lx)\n", f1, f0);
    printk("(N0,N1): (%p,%p)\n", i0, i1);

    printk("\nGot:\n");
    printk("(V0,V1): (%d,%d)\n", v0_after, v1_after);
    printk("(P0,P1): (%p,%p)\n", p0, p1);
    printk("(F0,F1): (0x%lx,0x%lx)\n", f0_after, f1_after);
    printk("(N0,N1): (%p,%p)\n", n0, n1);

    return false;
  }

  if (!queues_empty()) {
    return false;
  }

  printk("test_two success:\n");
  printk("(V0,V1): (%d,%d)\n", *(uint8_t*)p0, *(uint8_t*)p1);
  printk("(P0,P1): (%p,%p)\n", p0, p1);
  printk("(F0,F1): (0x%lx,0x%lx)\n", f0_after, f1_after);
  printk("(N0,N1): (%p,%p)\n", n0, n1);

  printk("\n");
  return true;
}

bool page_cmp(uint8_t *page, uint8_t val)
{
  for(int i = 0; i < RISCV_PGSIZE; i++) {
    if(page[i] != val)
      return false;
  }
  return true;
}

/* Test as much as we can at once (fill all queues) */
bool test_max(void)
{
  printk("test_max\n");
  void *pages[PFA_FREE_MAX];
  pgid_t ids[PFA_FREE_MAX];

  /* Allocate, evict, and publish frames */
  for(int i = 0; i < PFA_FREE_MAX; i++) {
    pages[i] = (void*)page_alloc();
    memset(pages[i], i, RISCV_PGSIZE);
    uintptr_t paddr = va2pa(pages[i]);
    ids[i] = pfa_evict_page(pages[i]);
    if(!pfa_poll_evict())
      return false;
    pfa_publish_freeframe(paddr);
  }

  /* Access pages in reverse order to make sure each page ends up in a
   * different physical frame */
  for(int i = PFA_FREE_MAX - 1; i >= 0; i--) {
    if(!page_cmp(pages[i], i)) {
      printk("Unexpected value in page %d: %d\n", i, *(uint8_t*)pages[i]);
      return false;
    }
  }

  /* Drain newpage queue sepparately to stress it out a bit */
  for(int i = PFA_FREE_MAX - 1; i >= 0; i--) {
    pgid_t newpage = pfa_pop_newpage();
    if(newpage != ids[i]) {
      printk("Newpage (%p) doesn't match fetched page (%p)\n", newpage, pages[i]);
      return false;
    }
  }

  if (!queues_empty()) {
    return false;
  }

  printk("test_max Success!\n");
  return true;
}

/* Test unbounded number of pages.
 * Note: __handle_page_fault is dealing with newpage and freeframe management
 * WARNING: This function leaks like a sieve (probably 2n pages)
 * WARNING: Since we're in the kernel, total memory is capped at 2MB. test_n works up
 * to about 512 pages in practice. If you really want to test larger, you can
 * modify mmap.c:pk_vm_init and set free_pages = mem_pages. This is probably
 * fine but I can't be sure.*/
#define PTRS_PER_PAGE (RISCV_PGSIZE / sizeof(void*))
bool test_n(int n) {
  printk("Test_%d\n", n);
  current_exp = PFA_EXP_TESTN;
  void **pages = (void**)page_alloc();

  /* int nleft = n; */
  test_n_nrem = n;
  while(test_n_nrem) {
    /* How many to do this iteration */
    int local_n = MIN(test_n_nrem, PTRS_PER_PAGE);

    /* Allocate and evict a bunch of pages.
     * Note: We'll never get the paddr or vaddr back after this */
    for(int i = 0; i < local_n; i++) {
      pages[i] = (void*)page_alloc();
      memset(pages[i], i, RISCV_PGSIZE);
      pfa_evict_page(pages[i]);
      if(!pfa_poll_evict())
        return false;
    }

    /* Touch all the stuff we just evicted */
    for(int i = 0; i < local_n; i++) {
      printk("pages[%d] = %p\n", i, pages[i]);
      if(!page_cmp(pages[i], i)) {
        printk("Unexpected value in page %d: %d\n", i, *(uint8_t*)pages[i]);
        return false;
      }
      /* This global is used by the pf handler, it must be decremented here
       * so that it reflects the number of un-faulted pages */
      test_n_nrem--;
    }
  }

  /* Finish draining the new page queue in case the page fault handler didn't
   * grab all of them (so we leave the PFA in a good state for subsequent
   * tests*/
  pfa_drain_newq();

  check_pfa_clean();

  printk("Test_%d Success\n", n);
  current_exp = PFA_EXP_OTHER;
  return true;
}

/* Test fetch of an invalid page (should cause page fault) */
uintptr_t test_inval_vaddr = -1;
bool test_inval_touched = false;
bool test_inval(void)
{
  printk("test_inval\n");

  uint8_t *page = (uint8_t*) page_alloc();
  uintptr_t paddr = va2pa((void*)page);
  pte_t *ptep = walk((uintptr_t)page);

  /* Mark page invalid */
  *ptep &= ~(PTE_V);
  flush_tlb();

  pgid_t pgid = pfa_evict_page((void*)page);
  if(!pfa_poll_evict())
    return false;
  pfa_publish_freeframe(paddr);

  if(!pfa_poll_evict())
    return false;

  /* Touch it, should cause page fault */
  test_inval_vaddr = (uintptr_t)page;
  *page = 17;

  if(!test_inval_touched) {
    printk("Didn't receive page fault on invalid page\n");
    return false;
  }

  /* Finish draining the new page queue in case the page fault handler didn't
   * grab all of them (so we leave the PFA in a good state for subsequent
   * tests*/
  pfa_drain_newq();

  if (!queues_empty()) {
    return false;
  }

  printk("test_inval success\n");
  return true;
}

/* Evicts the same vaddr multiple times */
bool test_repeat(void)
{
  printk("test_repeat\n");

  /* Volatile to force multiple queries to memory */
  volatile uint8_t *page = (volatile uint8_t*) page_alloc();
  uintptr_t paddr = va2pa((void*)page);
  *page = 17;

  pgid_t pgid = pfa_evict_page((void*)page);

  if(!pfa_poll_evict())
    return false;

  pfa_publish_freeframe(paddr);

  /* Force rmem fault */
  *page = 42;

  uint64_t nnew = pfa_check_newpage();
  if(nnew != 1) {
    printk("New page queue reporting wrong number of new pages: %ld\n", nnew);
    return false;
  }

  pgid_t newpage = pfa_pop_newpage();

  if(newpage != pgid) {
    printk("Newpage (%d) doesn't match fetched page (%d)\n", newpage, pgid);
    return false;
  }

  pte_t newpte = *walk((uintptr_t)page);
  if(pte_is_remote(newpte)) {
    printk("PTE Still marked remote!\n");
    return false;
  }

  if(*page != 42) {
    if(*page == 17) {
      printk("Page didn't get faulting write\n");
      return false;
    } else {
      printk("Page has garbage\n");
      return false;
    }
  }

  printk("evict same page again\n");
  /* Now evict again! */
  pgid = pfa_evict_page((void*)page);
  if(!pfa_poll_evict())
    return false;

  pfa_publish_freeframe(paddr);

  /* Fetch again */
  if(*page != 42) {
    if(*page == 17) {
      printk("Fetched old version of page\n");
      return false;
    } else {
      printk("Page has garbage\n");
      return false;
    }
  }

  /* Drain newq to leave state clean */
  newpage = pfa_pop_newpage();
  if(newpage != pgid) {
    printk("Second fetch returned wrong ID\n");
    return false;
  }

  if (!queues_empty()) {
    return false;
  }

  printk("test_repeat success\n\n");
  return true;
}

/*  allocate page X, Y
    evict page Y
    poll for evict completion
    evict page X
    immediately access Y (fetch triggers)
    page fault will occurr because an eviction is in progress
    page fault handler will notice evict queue is not empty, will poll
    page fault handler returns when eviction is done
    cpu retries access, succeds because pfa is now ready for fetch
    poll for eviction on X (not really needed)
*/
bool test_fetch_while_evicting()
{
  printk("test_fetch_while_evicting\n");
  rem_pg_t pgs[2];

  alloc_rem_pg(&pgs[0]); 
  alloc_rem_pg(&pgs[1]); 

  // evict 1 normally
  evict_full_rem_pg(&pgs[0]);

  // partially evict 1
  pgs[1].pgid = pfa_evict_page(pgs[1].ptr);

  // fetch 0 while evicting 1
  fetch_rem_pg(&pgs[0]);

  // finish evicting 1
  if(!pfa_poll_evict())
    return false;
  pfa_publish_freeframe(pgs[1].paddr); 

  fetch_rem_pg(&pgs[1]);
  
  pop_new_rem_pg(&pgs[0]);
  pop_new_rem_pg(&pgs[1]);

  check_pfa_clean();

  printk("test_fetch_while_evicting success\n");
  return true;
}

bool test_evict_largepgid()
{
  rem_pg_t pg;

  printk("test_evict_largepgid\n");

  alloc_rem_pg(&pg);
  
  // manually evict page because we need to set pageid
  // Note that we don't set a SWRES for this test
  pg.pgid = PFA_MAX_RPN;
  pfa_evict_page_pgid(pg.ptr, pg.pgid);
  if(!pfa_poll_evict())
    return false;
  pfa_publish_freeframe(pg.paddr);

  fetch_rem_pg(&pg);
  pop_new_rem_pg(&pg);
  check_pfa_clean();

  printk("test_evict_largepgid success\n");

  return true;
}

bool test_interleaved_newq()
{
  printk("test_interleaved_newq\n");
  rem_pg_t pgs[2];
  volatile pgid_t pgid;
  volatile uint64_t vaddr;

  alloc_rem_pg(&pgs[0]);
  alloc_rem_pg(&pgs[1]);

  printk("test_interleaved_newq vaddr then pgid\n");
  evict_full_rem_pg(&pgs[0]);
  evict_full_rem_pg(&pgs[1]);

  fetch_rem_pg(&pgs[0]);

  /* Partially drain newq (vaddr first)*/
  vaddr = *PFA_NEWVADDR;
  assert(vaddr == pgs[0].vaddr);

  fetch_rem_pg(&pgs[1]);

  /* Finish draining newq, this should be pgs[0] pgid */
  pgid = (pgid_t)(*PFA_NEWPGID);
  assert(pgid == pgs[0].pgid);

  pop_new_rem_pg(&pgs[1]);

  check_pfa_clean();


  printk("test_interleaved_newq pgid then vaddr\n");
  evict_full_rem_pg(&pgs[0]);
  evict_full_rem_pg(&pgs[1]);

  fetch_rem_pg(&pgs[0]);

  /* Partially drain newq */
  pgid = (pgid_t)(*PFA_NEWPGID);
  assert(pgid == pgs[0].pgid);

  fetch_rem_pg(&pgs[1]);

  /* Finish draining newq, this should be pgs[0] pgid */
  vaddr = *PFA_NEWVADDR;
  assert(vaddr == pgs[0].vaddr);

  pop_new_rem_pg(&pgs[1]);

  check_pfa_clean();

  return true;
}

/* bool test_n(int n) */
/* { */
/*   printk("test_%d\n", n); */
/*    */
/*   test_n_nrem = n; */
/*   while(test_n_nrem > 0) */
/*   { */
/*     for(int i = 0; i < PFA_NEW_MAX; i++) */
/*     { */
/*       evict_full_rem_pg(&exp_pgs[i]); */
/*     } */
/*  */
/*     for(int i = 0 */
/*  */
/*  */
/* } */
bool test_interleaved_newq_fault(void)
{
  printk("test_interleaved_newq_fault\n");
  rem_pg_t pgs[PFA_NEW_MAX];
  rem_pg_t faulting_pg;

  printk("Testing new_vaddr popped before fault:\n");

  alloc_rem_pg(&faulting_pg);

  current_exp = PFA_EXP_NEWVADDR_FAULT;
  test_vaddr = 0;
  test_pgid = 0;
  for(int i = 0; i < PFA_NEW_MAX; i++)
  {
    alloc_rem_pg(&pgs[i]);
    evict_full_rem_pg(&pgs[i]);
    fetch_rem_pg(&pgs[i]);
  }

  /* The newqs are now full, next access will fault */
  evict_full_rem_pg(&faulting_pg);

  test_vaddr = *PFA_NEWVADDR;
  assert(test_vaddr == pgs[0].vaddr);
  
  /* Page fault hander should be triggered by this fetch and drain the new_pgid */
  fetch_rem_pg(&faulting_pg);
  assert(test_pgid == pgs[0].pgid);

  test_pgid = 0;
  test_vaddr = 0;

  /* Everything should be back to normal with pgs[1:MAX] and faulting_pg in the newqs */
  for(int i = 1; i < PFA_NEW_MAX; i++) {
    pop_new_rem_pg(&pgs[i]);
  }
  pop_new_rem_pg(&faulting_pg);
  
  check_pfa_clean();
  
  printk("new_vaddr popped first success\n");
  printk("Testing new_pgid popped before fault:\n");

  test_pgid = 0;
  test_vaddr = 0;
  current_exp = PFA_EXP_NEWPGID_FAULT;

  for(int i = 0; i < PFA_NEW_MAX; i++)
  {
    evict_full_rem_pg(&pgs[i]);
    fetch_rem_pg(&pgs[i]);
  }
  
  /* The newqs are now full, next access will fault */
  evict_full_rem_pg(&faulting_pg);

  test_pgid = *PFA_NEWPGID;
  assert(test_pgid == pgs[0].pgid);
  
  /* Page fault hander should be triggered by this fetch and drain the new_pgid */
  fetch_rem_pg(&faulting_pg);
  assert(test_vaddr == pgs[0].vaddr);

  test_pgid = 0;
  test_vaddr = 0;

  /* Everything should be back to normal with pgs[1:MAX] and faulting_pg in the newqs */
  for(int i = 1; i < PFA_NEW_MAX; i++) {
    pop_new_rem_pg(&pgs[i]);
  }
  pop_new_rem_pg(&faulting_pg);
  
  check_pfa_clean();
 
  current_exp = PFA_EXP_OTHER;
  printk("new_pgid popped first success\n");
  return true;
}
  
/* This test behaves similarly to emulation mode in linux, we mark a page remote
 * without actually evicting anything, and leave all the queues empty */
bool test_emptyq(void)
{
  rem_pg_t pg;

  printk("test_empty_q\n");

  /* Paranoia, this test requires that the queues all be empty */
  check_pfa_clean();
  test_paddr = 0;
  test_vaddr = 0;

  current_exp = PFA_EXP_EMPTYQ;

  alloc_rem_pg(&pg);

  /* This is an illegal pgid, it definitely doesn't exist on the memblade */
  pgid_t pgid = PFA_INIT_RPN - 1;

  /* Make a remote pte without actually evicting */
  pte_t *page_pte = walk(pg.vaddr);
  *page_pte = pfa_mk_remote_pte(pgid, *page_pte);
  flush_tlb();

  /* page fault handler uses these */
  test_paddr = pg.paddr;
  test_vaddr = pg.vaddr;

  /* trigger a fault on this fake remote page. The handler has special code
   * here to fixup the pte. */
  fetch_rem_pg(&pg);
  test_paddr = 0;
  test_vaddr = 0;

  check_pfa_clean();

  printk("test_empty_q success\n");
  return true;
}

int main()
{
  pfa_init();

  if(!test_one(false)) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_one(true)) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_read_allbytes()) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_two()) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_max()) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_inval()) {
    printk("Test Failure\n");
    return EXIT_FAILURE;
  }

  if(!test_repeat()) {
    printk("Test Failure\n");
    return EXIT_FAILURE;
  }

  if(!test_fetch_while_evicting()) {
    printk("Test Failure\n");
    return EXIT_FAILURE;
  }

  if(!test_evict_largepgid()) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_interleaved_newq()) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_interleaved_newq_fault()) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_emptyq()) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }


  if(!test_n(32)) { // takes about 2m cycles
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_n(512)) { // takes about 2m cycles
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  printk("Test Success!\n");
  return EXIT_SUCCESS;
}

static void rest_of_boot_loader(uintptr_t kstack_top)
{
  current.time0 = rdtime();
  current.cycle0 = rdcycle();
  current.instret0 = rdinstret();

  printk("rest_of_boot_loader\n");

  int ret = main();

  size_t dt = rdtime() - current.time0;
  size_t dc = rdcycle() - current.cycle0;
  size_t di = rdinstret() - current.instret0;

  printk("%ld ticks\n", dt);
  printk("%ld cycles\n", dc);
  printk("%ld instructions\n", di);
  printk("%d.%d%d CPI\n", dc/di, 10ULL*dc/di % 10, (100ULL*dc + di/2)/di % 10);
  shutdown(ret);
}

void boot_loader(uintptr_t dtb)
{
  extern char trap_entry;
  write_csr(stvec, &trap_entry);
  write_csr(sscratch, 0);
  write_csr(sie, 0);
  set_csr(sstatus, SSTATUS_SUM);

  file_init();
  enter_supervisor_mode(rest_of_boot_loader, pk_vm_init(), 0);
}

void boot_other_hart(uintptr_t dtb)
{
  // stall all harts besides hart 0
  while (1)
    wfi();
}
