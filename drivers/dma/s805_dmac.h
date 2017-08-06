#include <mach/irqs.h>
#include <mach/am_regs.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include "virt-dma.h"
#include "s805_shared.h"

#define S805_DMA_CTRL                    P_NDMA_CNTL_REG0
#define S805_DMA_ENABLE                  BIT(14)                 /* Both CTRL and CLK resides in the same bit */
#define S805_DMA_DMA_PM                  BIT(27)

#ifdef CONFIG_S805_DMAC_TO
#define S805_DMA_TIME_OUT                CONFIG_S805_DMAC_TO_VAL  /* ms */
#else
#define S805_DMA_TIME_OUT                150                      /* Used in channel release, for busy waiting a channel to free its pending transactions. */
#endif

#define S805_DMA_MAX_HW_THREAD           4

#define S805_DMA_FIRQ_SEL                P_MEDIA_CPU_INTR_FIRQ_SEL
#define S805_DMA_FIRQ_BIT                BIT(12)     

#if defined CONFIG_CRYPTO_DEV_S805_TDES && defined CONFIG_CRYPTO_DEV_S805_AES
#define S805_CRYPTO_CIPHER               1
#endif

#define S805_DMA_THREAD_CTRL             P_NDMA_THREAD_REG

#define S805_DMA_SET_SLICE(slice)        (slice & 0xFF)     
#define S805_DMA_DEF_SLICE               16

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
	
	struct tasklet_struct tasklet_completed;  /* Tasklet for bh processing of interrupts. */

#ifdef CONFIG_S805_DMAC_TO 
	bool timer_busy;
#endif
#ifdef S805_CRYPTO_CIPHER
	bool cipher_busy;
#endif
	bool cyclic_busy;
	bool busy;

	uint __pending;
};

#ifdef CONFIG_S805_DMAC_TO
extern struct s805_dmadev *mgr;

int s805_dma_to_init ( void );
void s805_dma_to_shutdown ( void );
void s805_dma_to_start ( u16 ms );
void s805_dma_to_stop ( void );
#endif
