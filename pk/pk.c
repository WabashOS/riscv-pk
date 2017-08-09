#include "pk.h"
#include "mmap.h"
#include "boot.h"
#include "elf.h"
#include "mtrap.h"
#include "frontend.h"
#include "vm.h"
#include "atomic.h"
#include "rpfh.h"
#include <stdbool.h>
#include <stdlib.h>

elf_info current;

/* Max time to poll for completion for PFA stuff. Assume that the device is
 * broken if you have to poll this many times. Currently very conservative. */
#define MAX_POLL_ITER 1024*1024

/* Test one page. Evict, publish frame as free, then touch and check newpage */
bool test_one(bool flush)
{
  printk("test_one: %s\n", flush ? "Flush TLB" : "Don't Flush TLB");
  /* Volatile to force multiple queries to memory */
  volatile uint8_t *page = (volatile uint8_t*) page_alloc();
  uintptr_t paddr = va2pa((void*)page);
  *(uint8_t*)page = 17;

  rpfh_evict_page((void*)page);
  if(flush) {
    flush_tlb();
  }

  int poll_count = 0;
  while(rpfh_poll_evict() == 0) {
    if(poll_count++ == MAX_POLL_ITER) {
      printk("Polling for eviction completion took too long\n");
      return false;
    }
  }
  if(flush) {
    flush_tlb();
  }

  rpfh_publish_freeframe(paddr);
  if(flush) {
    flush_tlb();
  }

  /* Force rmem fault */
  *page = 42;
  if(flush) {
    flush_tlb();
  }

  void* newpage = rpfh_pop_newpage();
  if(flush) {
    flush_tlb();
  }

  if(newpage != page) {
    printk("Newpage (%p) doesn't match fetched page (%p)\n", newpage, page);
    return false;
  }
 
  pte_t newpte = *walk((uintptr_t)newpage);
  if((newpte & PTE_REM)) {
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

  printk("test_one success\n\n");
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

  /* Frames */
  uintptr_t f0 = va2pa((void*)p0);
  uintptr_t f1 = va2pa((void*)p1);

  /* Values */
  uint8_t v0 = 17;
  uint8_t v1 = 42;
  *(uint8_t*)p0 = v0;
  *(uint8_t*)p1 = v1;

  printk("Starting test:\n");
  printk("(V0,V1): (%d,%d)\n", *(uint8_t*)p0, *(uint8_t*)p1);
  printk("(P0,P1): (%p,%p)\n", p0, p1);
  printk("(F0,F1): (%lx,%lx)\n", f0, f1);

  rpfh_evict_page(p0);
  rpfh_evict_page(p1);
  rpfh_publish_freeframe(f0);
  rpfh_publish_freeframe(f1);

  /* Values */
  uint8_t v0_after, v1_after;
  
  /* Fetch in reverse order of publishing.
   * This should result in the vaddrs switching paddrs. */
  v1_after = *(uint8_t*)p1;
  v0_after = *(uint8_t*)p0;
  
  /* Get newpage info */
  void *n1 = rpfh_pop_newpage();
  void *n0 = rpfh_pop_newpage();

  /* Check which frame the page was fetched into. We expect the pages to have
   * swapped frames since we fetched in reverse order and freeq is a FIFO */
  uintptr_t f0_after = va2pa((void*)p0);
  uintptr_t f1_after = va2pa((void*)p1);
  
  if(f0_after != f1 || f1_after != f0 ||
     v1_after != v1 || v0_after != v0 ||
     p0 != n0       || p1 != n1)
  {
    printk("Test Failed:\n");
    printk("Expected:\n\n");
    printk("(V0,V1): (%d,%d)\n", v0, v1);
    printk("(P0,P1): (%p,%p)\n", p0, p1);
    printk("(F0,F1): (0x%lx,0x%lx)\n", f1, f0);
    printk("(N0,N1): (%p,%p)\n", p0, p1);

    printk("\nGot:\n");
    printk("(V0,V1): (%d,%d)\n", v0_after, v1_after);
    printk("(P0,P1): (%p,%p)\n", p0, p1);
    printk("(F0,F1): (0x%lx,0x%lx)\n", f0_after, f1_after);
    printk("(N0,N1): (%p,%p)\n", n0, n1);

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
  for(int i = 0; i < 4096; i++) {
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

  /* Allocate, evict, and publish frames */
  for(int i = 0; i < PFA_FREE_MAX; i++) {
    pages[i] = (void*)page_alloc();
    memset(pages[i], i, 4096);
    uintptr_t paddr = va2pa(pages[i]);
    rpfh_evict_page(pages[i]);
    rpfh_publish_freeframe(paddr);
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
    void* newpage = rpfh_pop_newpage();
    if(newpage != pages[i]) {
      printk("Newpage (%p) doesn't match fetched page (%p)\n", newpage, pages[i]);
      return false;
    }
  }

  printk("test_max Success!\n");
  return true;
}

/* Schenaningans due to PK's lack of a useful malloc */
void **__testn_pages;
int  __testn_n;
int  __testn_i;
#define test_n(N)  ({         \
  void *__testn_stat_pages[N];     \
  __testn_pages = __testn_stat_pages; \
  __testn_n = N;          \
  __testn_i = 0;          \
  __test_n(__testn_pages, N); \
    })
bool __test_n(void **pages, int n)
{
  printk("Test_%d\n", n);
  /* Allocate and evict batch of pages
   * Don't publish frames though, let page fault handler do that */
  for(int i = 0; i < n; i++) {
    pages[i] = (void*)page_alloc();
    memset(pages[i], i, 4096);
    rpfh_evict_page(pages[i]);
  }

  /* Start touching! */
  for(int i = n - 1; i >= 0; i--) {
    if(!page_cmp(pages[i], i)) {
      printk("Unexpected value in page %d: %d\n", i, *(uint8_t*)pages[i]);
      return false;
    }
  }

  /* Finish draining the new page queue in case the page fault handler didn't
   * grab all of them (so we leave the PFA in a good state for subsequent 
   * tests*/
  void *newpage;
  while( (newpage = rpfh_pop_newpage()) ) {}

  printk("Test_%d Success\n", n);
  return true;
}

int main()
{
  rpfh_init();

  if(!test_one(false)) {
    printk("Test Failure!\n");
    return EXIT_FAILURE;
  }

  if(!test_one(true)) {
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

  if(!test_n(256)) {
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
