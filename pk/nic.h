#include <stdint.h>

#define NIC_BASE           0x10016000 
#define NIC_MACADDR        ((volatile uint64_t*)(NIC_BASE + 24)) 
