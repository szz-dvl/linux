#include "s805_dmac.h"

#define S805_DMA_TO_IRQ               INT_TIMER_A
#define S805_DMA_TIMER_CTRL           P_ISA_TIMER_MUX
#define S805_DMA_TIMER_CFG            P_ISA_TIMERA

#define S805_DMA_TIMER_ENABLE         BIT(16)
#define S805_DMA_TIMER_ONE_SHOT       0
#define S805_DMA_TIMER_RES_1ms        0x3

#define S805_DMA_TIMER_MODE(mode)     ((mode & 0x1) << 12)
#define S805_DMA_TIMER_RES(res)       (res & 0x3)

#define S805_DMA_TIMER_MAX            0xFFFF
#define S805_DMA_TIMER_VAL(val)       (val & S805_DMA_TIMER_MAX)

#define S805_DMA_RESET_CNT            CBUS_REG_ADDR(0x2271)
#define S805_DMA_RESET_REG            P_RESET1_REGISTER
#define S805_DMA_RESET                BIT(9)
	
static inline void s805_dma_hard_reset ( void ) {

	u32 status;
		
	WR(S805_DMA_RESET, S805_DMA_RESET_REG);
	WR(1, S805_DMA_RESET_CNT);
	
	status = RD(S805_DMA_CTRL);
	
	status &= ~S805_DMA_DMA_PM;
	status |= S805_DMA_ENABLE;
	
	WR(status, S805_DMA_CTRL);
	
}

static void s805_dma_reschedule_broken ( struct s805_dmadev *m ) {

	struct s805_desc * d, * temp;
	LIST_HEAD(head);

	list_splice_tail_init(&m->completed, &head);
	list_splice_tail_init(&m->in_progress, &head);
	
	list_for_each_entry_safe(d, temp, &head, elem) {
		
		if (d->vd.tx.next)
			
			list_move_tail(&d->elem, &m->completed); /* Cyclic transfer */
		
		else {
			
			spin_lock(&m->lock);
			list_move(&d->elem, &m->scheduled); /* Re-schedule transactions that where in the batch in the moment of the time-out, giving them preference (in the head of the queue) */
			spin_unlock(&m->lock);
		}
		
/* #ifndef CONFIG_S805_DMAC_SERIALIZE */
/* 		m->thread_reset ++; */
/* #endif */
		
	}
	
#ifndef CONFIG_S805_DMAC_SERIALIZE
	m->max_thread = 1; /* Force serialization for the same amount of descriptors that failed. */
	m->thread_reset ++;
#endif

	m->busy = false;

}

static irqreturn_t s805_dma_to_callback (int irq, void *data)
{
	struct s805_dmadev *m = data;
	
	if (m->busy) {
		
	    dev_warn(m->ddev.dev, "Transaction timed out, reseting device.\n");
		
		s805_dma_hard_reset();
		
		s805_dma_reschedule_broken(m);
		tasklet_hi_schedule(&m->tasklet_completed);
		
	} else
	    dev_info(m->ddev.dev,"Timeout interrupt: Bye Bye.\n");
	
	return IRQ_HANDLED;
}

int s805_dma_to_init ( void ) {

	u32 status = RD(S805_DMA_TIMER_CTRL);

	status |= S805_DMA_TIMER_MODE(S805_DMA_TIMER_ONE_SHOT);
	status |= S805_DMA_TIMER_RES(S805_DMA_TIMER_RES_1ms);
		
	WR(status, S805_DMA_TIMER_CTRL);

	return request_irq(S805_DMA_TO_IRQ, s805_dma_to_callback, IRQF_TIMER, "s805_dmaengine_to_irq", mgr);

}


void s805_dma_to_shutdown ( void ) {

	u32 status = RD(S805_DMA_TIMER_CTRL);
		
	WR(status & ~S805_DMA_TIMER_ENABLE, S805_DMA_TIMER_CTRL);

	free_irq(S805_DMA_TO_IRQ, mgr);

}

void s805_dma_to_start ( u16 ms ) {

	u32 status = RD(S805_DMA_TIMER_CTRL);
	
	WR(status & ~S805_DMA_TIMER_ENABLE, S805_DMA_TIMER_CTRL);
	
	WR(S805_DMA_TIMER_VAL(ms), S805_DMA_TIMER_CFG);

	WR(status | S805_DMA_TIMER_ENABLE, S805_DMA_TIMER_CTRL);

}

void s805_dma_to_stop ( void ) {

	u32 status = RD(S805_DMA_TIMER_CTRL);

	WR(status & ~S805_DMA_TIMER_ENABLE, S805_DMA_TIMER_CTRL);
	
	WR(S805_DMA_TIMER_VAL(S805_DMA_TIMER_MAX), S805_DMA_TIMER_CFG);

	WR(status | S805_DMA_TIMER_ENABLE, S805_DMA_TIMER_CTRL);

}
