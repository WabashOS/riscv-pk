#include "rpfh.h"
#include "frontend.h"

static void rpfh_send_request(void const *page, rpfh_op op);

static void rpfh_write_reg(void const *page, rpfh_op op) {
  pte_t *page_pte = walk((uintptr_t) page);

  volatile uint64_t pte_paddr = va2pa(page_pte);
  volatile uint64_t *rpfh_addr = 0;
  const char *op_desc;

  switch (op) {
    case evict:
      rpfh_addr = (uint64_t *) PFA_EVICTPAGE;
      op_desc = "evictpage";
      break;
    case freepage:
      rpfh_addr = (uint64_t *) PFA_FREEPAGE;
      op_desc = "freepage";
      break;
    case workbuf:
      rpfh_addr = (uint64_t *) PFA_WORKBUF;
      op_desc = "workbuf";
      break;
    default:
      printk("op not implemented\n");
      shutdown(1);
  }

  printk("pk: op=%s pte_paddr=%lx, pte=%lx\n", op_desc, pte_paddr, *page_pte);
  *rpfh_addr = pte_paddr;
  flush_tlb();
}

void rpfh_init() {
  // create virtual mapping for RPFH I/O area
  // TODO: there should be a way of doing this without using vm
  __map_kernel_range(PFA_BASE, PFA_BASE, RISCV_PGSIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
  void *buf = (void *) page_alloc();
  rpfh_write_reg(buf, workbuf);
  __map_kernel_range((uintptr_t) buf, PFA_WORKBUF, RISCV_PGSIZE, PROT_READ|PROT_WRITE|PROT_EXEC);
}


void rpfh_publish_newpage(void const *page) {
  rpfh_write_reg(page, freepage);
}

void rpfh_evict_page(void const *page) {
  rpfh_write_reg(page, evict);
}

unsigned int rpfh_read_reg(rpfh_op op) {
  volatile uint32_t *rpfh_addr = 0;

  switch (op) {
    case evict:
      rpfh_addr = (uint32_t *) PFA_EVICTPAGE;
      break;
    case freepage:
      rpfh_addr = (uint32_t *) PFA_FREEPAGE;
      break;
    case newframe:
      rpfh_addr = (uint32_t *) PFA_NEWFRAME;
      break;
    default:
      printk("op not implemented\n");
      shutdown(1);
  }

  volatile unsigned int *ptr = (volatile unsigned int *) rpfh_addr;
  unsigned int val = *ptr;
  mb();
  return val;
}
