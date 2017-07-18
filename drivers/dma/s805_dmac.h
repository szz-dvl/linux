#include <mach/irqs.h>
#include <mach/am_regs.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include "virt-dma.h"

#define WR(data, addr)  *(volatile unsigned long *)(addr)=data
#define RD(addr)        *(volatile unsigned long *)(addr)

#define S805_DMA_CTRL                    P_NDMA_CNTL_REG0
#define S805_DMA_ENABLE                  BIT(14)                 /* Both CTRL and CLK resides in the same bit */
#define S805_DMA_DMA_PM                  BIT(27)

#ifdef CONFIG_S805_DMAC_TO
#define S805_DMA_TIME_OUT                CONFIG_S805_DMAC_TO_VAL  /* ms */
#else
#define S805_DMA_TIME_OUT                150  /* Used in terminate channel and channel release, for busy waiting a channel to free its pending transactions. */
#endif

#define __S805_DMAC

struct s805_dmadev 
{
	struct dma_device ddev;
	struct device_dma_parameters dma_parms;
	
	spinlock_t lock;                          /* General mgr lock. */
	int irq_number;                           /* IRQ number. */
    uint chan_available;                      /* Amount of channels available. */

	uint max_thread;                          /* Max number of threads to be run in parallel */
	
#ifndef CONFIG_S805_DMAC_SERIALIZE
	uint thread_reset;                        /* Amount of transactions to be serialized before thread reset */
#endif
	
	struct list_head scheduled;               /* List of descriptors currently scheduled. */
	struct list_head in_progress;             /* List of descriptors in progress. */
	struct list_head completed;               /* List of descriptors completed. */
	
	struct tasklet_struct tasklet_completed;  /* Tasklet for bh processing of interrupts. */

	bool busy_2d;  
	bool cyclic_busy;
	bool busy;
};

/* S805 Datasheet p.57 */
struct s805_table_desc 
{
	u32 control;        /* entry 0 */
	u32 src;            /* entry 1 */
	u32 dst;            /* entry 2 */
	u32 count;          /* entry 3 */
	
	/* 2D Move */
	u16 src_burst;      /* entry 4 [15:0]  */
	u16 src_skip;       /* entry 4 [31:16] */
	
	u16 dst_burst;      /* entry 5 [15:0]  */
	u16 dst_skip;       /* entry 5 [31:16] */
	
	
	/* Crypto engine */
	u32 crypto;         /* entry 6 */
	
} __attribute__ ((aligned (32)));

typedef struct s805_table_desc_entry {

	struct list_head elem;
	struct s805_table_desc * table;
	dma_addr_t paddr;

} s805_dtable;

struct s805_desc {
	
	struct s805_chan *c;
	struct virt_dma_desc vd;
	struct list_head elem;
	
	/* List of table descriptors holding the information of the trasaction */
	struct list_head desc_list;
	
	/* Descriptors pending of process */
	unsigned int frames;

	/* Struct to store the information for memset source value */
	struct memset_info * memset;

	/* For transactions with more than S805_DMA_MAX_DESC data chunks. */
	s805_dtable * next;

	/* 2D mode, meant to detect 2D transactions to serialize them */
	bool xfer_2d;
	
};

#ifdef CONFIG_S805_DMAC_TO
extern struct s805_dmadev *mgr;

int s805_dma_to_init ( void );
void s805_dma_to_shutdown ( void );
void s805_dma_to_start ( u16 ms );
void s805_dma_to_stop ( void );
#endif
