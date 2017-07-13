#include <mach/irqs.h>
#include <mach/am_regs.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>

#define WR(data, addr)  *(volatile unsigned long *)(addr)=data
#define RD(addr)        *(volatile unsigned long *)(addr)

#define S805_DMA_CTRL                    P_NDMA_CNTL_REG0
#define S805_DMA_ENABLE                  BIT(14)                 /* Both CTRL and CLK resides in the same bit */
#define S805_DMA_DMA_PM                  BIT(27)                 
#define S805_DMA_RESET_REG               P_RESET1_REGISTER
#define S805_DMA_RESET                   BIT(9)

#define __S805_DMAC

struct s805_dmadev 
{
	struct dma_device ddev;
	struct device_dma_parameters dma_parms;
	
	spinlock_t lock;                          /* General mgr lock. */
	int irq_number;                           /* IRQ number. */
    uint chan_available;                      /* Amount of channels available. */

	struct list_head scheduled;               /* List of descriptors currently scheduled. */
	struct list_head in_progress;             /* List of descriptors in progress. */
	struct list_head completed;               /* List of descriptors completed. */

	struct list_head cyclics;                 /* List of cyclic descriptors in progress. */
	
	struct tasklet_struct tasklet_cyclic;     /* Tasklet for processing cyclic callbacks. */
	struct tasklet_struct tasklet_completed;  /* Tasklet for bh processing of interrupts. */

	rwlock_t rwlock;
	uint pending_irqs;
	bool cyclic_busy;
	bool busy;
};

extern struct s805_dmadev *mgr;

int s805_dma_to_init ( void );
void s805_dma_to_shutdown ( void );
void s805_dma_to_start ( u16 ms );
void s805_dma_to_stop ( void );
