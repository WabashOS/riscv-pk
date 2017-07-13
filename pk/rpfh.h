#ifndef _RPFH_H
#define _RPFH_H

#include "mmap.h"
#include "pk.h"
#include "atomic.h"
#include <stdint.h>

#define PFA_BASE           0x2000
#define PFA_FREEPAGE       (PFA_BASE)
#define PFA_EVICTPAGE      (PFA_BASE + 8)
#define PFA_NEWFRAME       (PFA_BASE + 16)
#define PFA_WORKBUF        (PFA_BASE + 24)

typedef enum { evict, freepage, newframe, workbuf} rpfh_op;

void rpfh_init();
void rpfh_evict_page(void const *page);
void rpfh_publish_newpage(void const *page);
unsigned int rpfh_read_reg(rpfh_op op);

#endif
