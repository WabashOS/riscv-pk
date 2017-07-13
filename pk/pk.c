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

elf_info current;

// check everything works with one free page, one evicted page, one fetched page
void test_full_onepage() {
  // publish freepage available for rpfh
  void *freepage1 = (void *) page_alloc();
  uint64_t freepage1pte = *(walk((uint64_t) freepage1));
  rpfh_publish_newpage(freepage1);

  kassert(rpfh_read_reg(freepage) == 1);

  // evict a page
  void *page = (void *) page_alloc();
  *((int *)page) = 100;
  rpfh_evict_page(page);

  while (rpfh_read_reg(evict) != 0) { }

  // page is remote
  kassert(((uint64_t) *(walk((uint64_t )page)) & (uint64_t) 1 << 48) >> 48 == 1);

  // access the evicted page
  int val = *((int *)page);
  // contents are correct?
  kassert(val == 100);
  // freepage queue empty
  kassert(rpfh_read_reg(freepage) == 0);

  uint64_t *fetchedpteaddr = walk((uint64_t) page);
  uint64_t fetchedpte = *fetchedpteaddr;
  // make sure we are using freepage1's ppn to backup the fetched page
  kassert(freepage1pte >> 10 == fetchedpte >> 10);

  // check newpage entries, valid entry should be the fetched page's pte
  kassert((rpfh_read_reg(newframe) & 0xFFFFFFFF) == (uint64_t) fetchedpteaddr);
  kassert(rpfh_read_reg(newframe) == 0);
}

// register a few freepages, check that the count is correct
void test_full_onepage_freepages() {
  void *freepage1 = (void *) page_alloc();
  void *freepage2 = (void *) page_alloc();
  void *freepage3 = (void *) page_alloc();
  rpfh_publish_newpage(freepage1);
  rpfh_publish_newpage(freepage2);
  rpfh_publish_newpage(freepage3);

  kassert(rpfh_read_reg(freepage) == 3);

  // evict a page
  void *page = (void *) page_alloc();
  rpfh_evict_page(page);
  while (rpfh_read_reg(evict) != 0) { }
  volatile int val = *((int *)page); // cause the remote fault

  kassert(rpfh_read_reg(freepage) == 2);
  kassert((rpfh_read_reg(newframe) & 0xFFFFFFFF) == (uint64_t) walk((uint64_t) page));
  kassert(rpfh_read_reg(newframe) == 0);
}

void test_full_onepage_evictpages() {
  void *freepage1 = (void *) page_alloc();
  void *freepage2 = (void *) page_alloc();
  void *freepage3 = (void *) page_alloc();
  rpfh_publish_newpage(freepage1);
  rpfh_publish_newpage(freepage2);
  rpfh_publish_newpage(freepage3);

  kassert(rpfh_read_reg(freepage) == 3);

  // evict 3 pages
  void *page = (void *) page_alloc();
  void *page1 = (void *) page_alloc();
  void *page2 = (void *) page_alloc();
  rpfh_evict_page(page);
  rpfh_evict_page(page1);
  rpfh_evict_page(page2);

  // wait for evictions to be processed
  while (rpfh_read_reg(evict) != 0) { }

  // cause the remote faults
  volatile int val = *((int *)page);
  val = *((int *)page1);
  val = *((int *)page2);

  // freepage queue empty
  kassert(rpfh_read_reg(freepage) == 0);
  kassert((rpfh_read_reg(newframe) & 0xFFFFFFFF) == (uint64_t) walk((uint64_t) page));
  kassert((rpfh_read_reg(newframe) & 0xFFFFFFFF) == (uint64_t) walk((uint64_t) page1));
  kassert((rpfh_read_reg(newframe) & 0xFFFFFFFF) == (uint64_t) walk((uint64_t) page2));
  kassert(rpfh_read_reg(newframe) == 0);
}

int main() {
  printk("I'M ALIVE!\n");
  rpfh_init();

  //test_full_onepage();
  //test_full_onepage_freepages();
  test_full_onepage_evictpages();
  printk("tests passed\n");
  return 0;
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
