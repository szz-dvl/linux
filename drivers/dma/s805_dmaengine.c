#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/printk.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
//#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/of.h>
//#include <linux/of_dma.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>

/* #include <linux/preempt.h> */
/* #include <linux/irqflags.h> */

#include "virt-dma.h"

#include <mach/am_regs.h>

#define WR(data, addr)  *(volatile unsigned long *)(addr)=data
#define RD(addr)        *(volatile unsigned long *)(addr)

#define S805_DMA_IRQ                     68
#define LOWER_32                         0x00000000FFFFFFFF

/* Registers & Bitmaps for the s805 DMAC */

#define S805_DMA_MAX_DESC                128
#define S805_DMA_MAX_THREAD              4
#define S805_DMA_NULL_COOKIE             0
#define S805_DMA_MAX_BURST               0xFFFF
#define S805_DMA_MAX_SKIP                0xFFFF
#define S805_MAX_TR_SIZE                 0x1FFFFFF

#define S805_DMA_CTRL                    P_NDMA_CNTL_REG0
#define S805_DMA_THREAD_CTRL             P_NDMA_THREAD_REG
#define S805_DMA_CLK                     P_HHI_GCLK_MPEG1
#define S805_DMA_CURR_THREAD             BIT(12) | BIT(13)
#define S805_DMA_ACTIVE_THREAD           ( ( RD(S805_DMA_THREAD_CTRL) & (S805_DMA_CURR_THREAD) ) >> 12 ) 

#define S805_DMA_DLST_STR0               P_NDMA_THREAD_TABLE_START0
#define S805_DMA_DLST_CURR0              P_NDMA_THREAD_TABLE_CURR0
#define S805_DMA_DLST_END0               P_NDMA_THREAD_TABLE_END0
#define S805_DMA_DLST_STR1               P_NDMA_THREAD_TABLE_START1
#define S805_DMA_DLST_CURR1              P_NDMA_THREAD_TABLE_CURR1
#define S805_DMA_DLST_END1               P_NDMA_THREAD_TABLE_END1
#define S805_DMA_DLST_STR2               P_NDMA_THREAD_TABLE_START2
#define S805_DMA_DLST_CURR2              P_NDMA_THREAD_TABLE_CURR2
#define S805_DMA_DLST_END2               P_NDMA_THREAD_TABLE_END2
#define S805_DMA_DLST_STR3               P_NDMA_THREAD_TABLE_START3
#define S805_DMA_DLST_CURR3              P_NDMA_THREAD_TABLE_CURR3
#define S805_DMA_DLST_END3               P_NDMA_THREAD_TABLE_END3

#define S805_DMA_ENABLE                  BIT(14)                 /* Both CTRL and CLK resides in the same bit */  

#define S805_DTBL_ADD_DESC               P_NDMA_TABLE_ADD_REG
#define S805_DMA_MAGIC_REG               0x2271
#define S805_DMA_MAGIC                   CBUS_REG_ADDR(S805_DMA_MAGIC_REG)
#define S805_DMA_MAGIC_CLEAR             0x00000001
#define S805_DMA_ADD_DESC(ch, cnt)       (((ch & 0x3) << 8) | (cnt & 0xff))
#define S805_DMA_DESC_CNT                0xFF

#define S805_DMA_THREAD_INIT(ch)         (1 << (24 + ch))
#define S805_DMA_THREAD_ENABLE(ch)       (1 << (8 + ch))
#define S805_DMA_THREAD_START(ch)        S805_DMA_THREAD_ENABLE(ch) | S805_DMA_THREAD_INIT(ch)

#define S805_DTBL_OWNER_ID(ch)           ((ch & 0x3) << 30)
#define S805_DTBL_PRE_ENDIAN             ((0x000 & 0x7) << 27)  /* Fixed Pre Endian type */
#define S805_DTBL_SRC_HOLD               BIT(26) 
#define S805_DTBL_DST_HOLD               BIT(25)
#define S805_DTBL_INLINE_TYPE(type)      ((type & 0x7) << 22)
#define S805_DTBL_IRQ                    BIT(21)
#define S805_DTBL_NO_BREAK               BIT(8)                
#define S805_DTBL_TSC(tsc)               (tsc & 0xFF)           /* Unused by now */

/* We will need this in "dmaengine.h" in order to support crypto transactions. Don't know exactly how I will arrange crypto into linux yet ... */
typedef enum inline_types {
	INLINE_NORMAL,
	INLINE_TDES,
	INLINE_DIVX,
	INLINE_CRC,
	INLINE_AES
} s805_dma_tr_type;

struct s805_dmadev 
{
	struct dma_device ddev;
	struct device_dma_parameters dma_parms;
	
	spinlock_t lock;                  /* General mgr lock. */
	int irq_number;                   /* IRQ number. */
    uint chan_available;              /* Amount of channels available. */

	struct list_head scheduled;       /* List of descriptors currently scheduled. */
	struct list_head in_progress;     /* List of descriptors in progress. */
	struct list_head completed;       /* List of descriptors completed. */

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

struct memset_info {

	long long * value;
	dma_addr_t paddr;
};

struct s805_desc {
	
	struct s805_chan *c;
	struct virt_dma_desc vd;
	struct list_head elem;
	
	/* List of table descriptors holding the information of the trasaction */
	struct list_head desc_list;
	
	/* Descriptors pending of process */
	unsigned int frames;
	
	/* Boolean to detect terminated transactions. */
	bool terminated;
	
	/* Struct to store the information for memset source value */
	struct memset_info * memset;

	/* Boolean for cyclic transfers. */
	bool cyclic;

	/* For transactions with more than S805_DMA_MAX_DESC data chunks. */
	dma_addr_t next;
	
};

typedef struct s805_chan {
	
	struct virt_dma_chan vc;

	 /* Channel configuration, needed for slave_sg and cyclic transfers. */
	struct dma_slave_config cfg;

	/* Status of the channel either DMA_PAUSE or DMA_IN_PROGRESS, DMA_SUCCESS if inactive channel. */
	enum dma_status status;        	

	/* DMA pool. */
	struct dma_pool *pool;        
	
} s805_chan;

/* Auxiliar structure for dma_dg */
struct sg_info {

    struct scatterlist * src_cursor;
	struct scatterlist * dst_cursor;
	
    struct scatterlist * next_src;
	struct scatterlist * next_dst;
	
	uint src_bytes;
	uint dst_bytes;
	
};

static struct s805_dmadev *mgr;	    /* DMA manager */
static unsigned int dma_channels = 0;

static const struct of_device_id s805_dma_of_match[] = 
	{
		{ .compatible = "aml,amls805-dma", },
		{},
	};
MODULE_DEVICE_TABLE(of, s805_dma_of_match);

static inline struct s805_chan *to_s805_dma_chan(struct dma_chan *c)
{
	return container_of(c, struct s805_chan, vc.chan);
}

static inline struct s805_desc *to_s805_dma_desc(struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct s805_desc, vd.tx);
}

/* 
   
   Adds a zeroed descriptor at the end of the current list, if possible. 
   To detect the end of the transaction. 
   
*/
static void add_zeroed_tdesc (struct s805_desc * d)
{
	
	s805_dtable * desc_tbl = kzalloc(sizeof(s805_dtable), GFP_NOWAIT);
	
	if (!desc_tbl) 
	    return;
	
	desc_tbl->table = dma_pool_alloc(d->c->pool, GFP_NOWAIT | __GFP_ZERO, &desc_tbl->paddr); /* __GFP_ZERO: Not Working. */
	
	if (!desc_tbl->table) {
		
		kfree(desc_tbl);
		return;
		
	} else
		*desc_tbl->table = (struct s805_table_desc) { 0 };

	list_add_tail(&desc_tbl->elem, &d->desc_list);
		
}

/* Auxiliar function to initialize descriptors. */
static s805_dtable * def_init_new_tdesc (struct s805_chan * c, unsigned int frames)
{
	
	s805_dtable * desc_tbl = kzalloc(sizeof(s805_dtable), GFP_NOWAIT);
	
	if (!desc_tbl) 
	    return NULL;
	
	desc_tbl->table = dma_pool_alloc(c->pool, GFP_NOWAIT | __GFP_ZERO, &desc_tbl->paddr); /* __GFP_ZERO: Not Working. */
	
	if (!desc_tbl->table) {
		
		kfree(desc_tbl);
		return NULL;
		
	} else
		*desc_tbl->table = (struct s805_table_desc) { 0 };
	
	/* Control common part */
	desc_tbl->table->control |= S805_DTBL_PRE_ENDIAN;
	desc_tbl->table->control |= S805_DTBL_INLINE_TYPE(INLINE_NORMAL); /* To Do: Add support for crypto types */
	desc_tbl->table->control |= S805_DTBL_NO_BREAK;                   /* 
																		 Process the whole descriptor at once, without thread switching 
																		 
																		 This needs to be tested with this approach, if this bit is set the threads will be processed at once, 
																		 without thread switching, what will make that the interrupts to be delivered more separated in time, however
																		 if this bit is not set the work of the active threads will be, in some manner, balanced so is we see the
																		 the 4 threads, with its possible active transactions, as a batch of transactions (what is actually what this
																		 implementation tries) to not set this bit may be a benefit, specially if "in_progress" transactions differ
																		 in size. In the other hand if "in_progress" transactions are similar in size interrupts may be delivered very
																		 close in time, hence "nestedly preempted", what may cause malfunction.
																		 
																		 As exposed above, to be tested. 
																		 
																	  */

	if (frames && !(frames % S805_DMA_MAX_DESC))
		desc_tbl->table->control |= S805_DTBL_IRQ;                    /* 
																		 This bit will be set for every S805_DMA_MAX_DESC = 128 data chunks. Since the pool will allocate, in first instance, a whole page to deliver our 
																		 32 Bytes blocks if more than PAGE_SIZE / 32 = 128 blocks are allocated for a transaction and the new page allocated by the pool is not contiguous 
																		 with the latter one descriptors won't be contiguous in memory, hence the hardware won't be able to fetch the descriptor 129 letting the transaction 
																		 unfinished. So we force an interrupt every S805_DMA_MAX_DESC to reallocate the adresses in the hardware registers every time the page where the 
																		 descriptors reside attempts to change.
																		 
																	  */
		
	return desc_tbl;
	
}

/* 
   Bypass function for dma_prep_slave_sg (dmaengine interface) 
   
   To - Do:
   
   * Add support for crypto inline types;
   
*/

static struct dma_async_tx_descriptor * 
s805_dma_prep_slave_sg( struct dma_chan *chan,
						struct scatterlist *sgl,
						unsigned int sg_len,
						enum dma_transfer_direction direction,
						unsigned long flags, 
						void *context)
{
	struct s805_chan *c = to_s805_dma_chan(chan);
	enum dma_slave_buswidth dev_width;
	struct s805_desc *d;
	dma_addr_t dev_addr;
	struct scatterlist *sgent;
	unsigned int i, j;
	s805_dtable * desc_tbl, * temp;
	
	/*
	  
	  Is needed to check here for src/dst device FIFO to be at least S805_MAX_TR_SIZE ??
	  to not overwrite unknown memory positions?? ... How to do this??
	  
	*/

	/* RapidIO not supported */
	if (context != NULL) {

		dev_err(chan->device->dev, "%s: RapidIO transactions not supported.\n", __func__);
		return NULL;

	}
	
	/* If user didn't issued dmaengine_slave_config return */
	if ((!c->cfg.src_addr && direction == DMA_DEV_TO_MEM) 
		|| (!c->cfg.dst_addr && direction == DMA_MEM_TO_DEV) 
		|| (!c->cfg.dst_addr && !c->cfg.src_addr)) {
		
		dev_err(chan->device->dev, "Slave configuration not provided, please run dmaengine_slave_config before performing this operation.\n");
		return NULL;
	}
	
	/* src address can't never match dst address - TO TRY -*/
	if (c->cfg.dst_addr == c->cfg.src_addr) {
		
		dev_err(chan->device->dev, "Same source and destination address provided: 0x%08x\n", c->cfg.src_addr);
		return NULL;
	} 

	switch (direction) {
	case DMA_DEV_TO_MEM:
		{
			dev_addr = c->cfg.src_addr;
			dev_width = c->cfg.src_addr_width;
		}
		break;;
	case DMA_MEM_TO_DEV:
		{
			dev_addr = c->cfg.dst_addr;
			dev_width = c->cfg.dst_addr_width;
		}
		break;;
	case DMA_DEV_TO_DEV:
		{
			/* Quick fix to treat all cases for buswidth errors */
			if (c->cfg.dst_addr_width != c->cfg.src_addr_width) {
				dev_err(chan->device->dev, "Bad buswidth for DMA_DEV_TO_DEV");
				return NULL;
			} else
				dev_width = c->cfg.dst_addr_width;
		}		
		break;;
	default:
	    dev_err(chan->device->dev, "Unsupported direction: %i\n", direction);
		return NULL;
	}
	

	/* Datasheet p.57 entry 1 & 2 */
	if (dev_width != DMA_SLAVE_BUSWIDTH_8_BYTES) {
		
		dev_err(chan->device->dev, "Unsupported buswidth: %i\n", dev_width);
		return NULL;

	}
	
	/* Allocate and setup the descriptor. GFP_NOWAIT */
	d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;

	d->c = c;
	d->frames = 0;
	INIT_LIST_HEAD(&d->desc_list);
	
	/*
	 * Iterate over all SG entries, create a table descriptor
	 * for each frame and add them to the descriptor list.
	 * 
	 * We need to care about the length of the blocks not to 
	 * overcome the maximum length supported by the hard, it is
	 * S805_MAX_TR_SIZE (~33.5MB) per block.
	 *
	 */
    
	for_each_sg(sgl, sgent, sg_len, i) {
		
		dma_addr_t addr = sg_dma_address(sgent); /* is the offset taken into account here? */
		unsigned int len = sg_dma_len(sgent);
		
		for (j = 0; j < len; j += S805_MAX_TR_SIZE) {
			
			s805_dtable *desc_tbl = def_init_new_tdesc(c, d->frames);

			if (!desc_tbl) 
				goto error_list;
			
			/* Setup addresses */
			switch(direction) {
			case DMA_DEV_TO_MEM:
				{
					desc_tbl->table->control |= S805_DTBL_SRC_HOLD;
					desc_tbl->table->src = dev_addr;
					desc_tbl->table->dst = addr + (dma_addr_t)j;
				}
				break;;
				
			case DMA_MEM_TO_DEV:
				{
					desc_tbl->table->control |= S805_DTBL_DST_HOLD;
					desc_tbl->table->src = addr + (dma_addr_t)j;
					desc_tbl->table->dst = dev_addr;
				}
				break;;    
				
			case DMA_DEV_TO_DEV:
				{
					/* Is that assumption fair? will the user have acces to devices memory? can I hold both dst and src addresses here? */
					desc_tbl->table->control |= (S805_DTBL_SRC_HOLD | S805_DTBL_DST_HOLD);
					desc_tbl->table->src = c->cfg.src_addr;
					desc_tbl->table->dst = c->cfg.dst_addr;
				}
				break;;

			default:
				
				dev_err(chan->device->dev, "This must never happen!");
				return NULL;
				
			}
			
			/* Fill byte count for the block move */
			desc_tbl->table->count = min((len - j) & S805_MAX_TR_SIZE, (uint) S805_MAX_TR_SIZE);
			
			list_add_tail(&desc_tbl->elem, &d->desc_list);

			d->frames ++;
		}
	}

	/* Ensure that the last descriptor will interrupt us. */
	list_entry(d->desc_list.prev, s805_dtable, elem)->table->control |= S805_DTBL_IRQ;

	add_zeroed_tdesc(d);
	
	/* Assert cyclic false for this descriptor */
	d->cyclic = false;
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);
	
 error_list:

	dev_err(chan->device->dev, "Error allocating descriptors.");
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}

	kfree(d);
	return NULL; 
}

/* Auxiliar function to initialise descriptors (interleaved) */
static s805_dtable * ileaved_def_init_new_tdesc (struct s805_chan *c,
												 struct dma_interleaved_template *xt,
												 int burst_and_skip,
												 int count,
												 uint frames)
{
	s805_dtable * desc_tbl = def_init_new_tdesc(c, frames);
	
	if (!desc_tbl) 
	    return NULL;
	
	if (!xt->src_inc) {
		
		desc_tbl->table->control |= S805_DTBL_SRC_HOLD;
		desc_tbl->table->src = xt->src_start;
		
	} else {
		
		if (xt->src_sgl)
			desc_tbl->table->src = xt->src_start + (dma_addr_t) burst_and_skip;
		else
			desc_tbl->table->src = xt->src_start + (dma_addr_t) count;
	}
	
	if (!xt->dst_inc) {
		
		desc_tbl->table->control |= S805_DTBL_DST_HOLD;
		desc_tbl->table->dst = xt->dst_start;
		
	} else {
		
		if (xt->dst_sgl)
			desc_tbl->table->dst = xt->dst_start + (dma_addr_t) burst_and_skip;
		else
			desc_tbl->table->dst = xt->dst_start + (dma_addr_t) count;
	}
	
	return desc_tbl;
}

static struct dma_async_tx_descriptor * 
s805_dma_prep_interleaved (struct dma_chan *chan, 
						   struct dma_interleaved_template *xt, 
						   unsigned long flags) 
{
	
	struct s805_chan *c = to_s805_dma_chan(chan);
	struct s805_desc *d;
    struct data_chunk last;
	struct s805_table_desc *table;
	s805_dtable *desc_tbl, *temp;
	int i, j, count, tmp_size, byte_cnt = 0, act_size = 0, idx = 0;

    dev_dbg(c->vc.chan.device->dev, "DMA interleaved (xt): \n"	\
			"\tsrc_start: 0x%08x\n"								\
			"\tdst_start: 0x%08x\n"								\
			"\tdir: %d\n"										\
			"\tsrc_inc: %s\n"									\
			"\tdst_inc: %s\n"									\
			"\tsrc_sgl: %s\n"									\
			"\tdst_sgl: %s\n"									\
			"\tnumf: %d\n"										\
			"\tframe_size: %d\n\n"										\
			, xt->src_start, xt->dst_start, xt->dir, xt->src_inc ? "true" : "false", xt->dst_inc ? "true" : "false", 
			xt->src_sgl ? "true" : "false", xt->dst_sgl ? "true" : "false", xt->numf, xt->frame_size);
	
	if (xt->dir == DMA_DEV_TO_DEV || 
		(!xt->src_inc && (xt->src_sgl || xt->dir != DMA_DEV_TO_MEM)) || 
		(!xt->dst_inc && (xt->dst_sgl || xt->dir != DMA_MEM_TO_DEV))) {
		
		dev_err(chan->device->dev, "Bad Configuration provided.\n");
		return NULL;
		
	}
	
	/* Allocate and setup the descriptor. */
	d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;

	d->c = c;
	d->frames = 0;
	INIT_LIST_HEAD(&d->desc_list);
	
	/*
	 * At this point we need to find out the number of descriptors (physicall descriptors, it is the ones in DS p.57) we need to allocate here,
	 * 
	 * Assumptions:
	 *     
	 *    * frame_size = number of xt->sgl entries for each frame,
	 *    
	 *    * Each frame is supposed to be in order, it is, if frame size is 6, the sgl entries will be token 6 by 6, and it will make a frame, 
	 *      next frame will then start one position after the end of the later. Then ARRAY_SIZE(xt->sgl) / nframes == frame_size 
	 *      must evaluate allways to true here. 
	 *    
     *    * Data chunks sizes and skips values may vary inside the same frame.
	 *
	 * Questions:
	 *
	 *    * Must we raise a warning if any acces is not aligned with 8 Bytes buswidth, 64 bit accesses??
	 *    
	 *    
	 */
	
	count = 0;
	byte_cnt = 0;
	last.size = xt->sgl[d->frames].size;
	last.icg = xt->sgl[d->frames].icg;
	desc_tbl = ileaved_def_init_new_tdesc(c, xt, count, byte_cnt, d->frames);
	
	if (!desc_tbl)
		return NULL;
	else
		table = desc_tbl->table;
	
	d->frames ++;
		
	for (i = 0; i<xt->numf; i++) { 
		
		for (j = 0; j<xt->frame_size; j++) {
			
			idx = (i * xt->frame_size) + j;
			
			//dev_dbg(c->vc.chan.device->dev, "Adding data chunk %d: \n\tsize = %d\n\ticg = %d\n", idx, xt->sgl[idx].size, xt->sgl[idx].icg);
			
			/* 
			 * 2D move:
			 *
			 * It is unsupported for the current kernel (3.10.y) to distinguish between the lengths of the skip and burts for the src and dst.
			 *
			 */
			
			if (xt->dst_sgl || xt->src_sgl) {
				
				/* if ( xt->sgl[idx].icg == 0 ) */
				/* 	dev_warn(c->vc.chan.device->dev, "ICG size 0 received for data chunk %d while src_sgl / dst_sgl evaluates to true.\n", idx); */
				
					
				for (tmp_size = xt->sgl[idx].size; tmp_size > 0; tmp_size -= S805_DMA_MAX_BURST) {
					
					if (tmp_size <= S805_DMA_MAX_BURST)
						act_size = tmp_size;
					else
						act_size = S805_DMA_MAX_BURST;
					
					if ( ((table->count + act_size) >= S805_MAX_TR_SIZE || last.size != xt->sgl[idx].size || last.icg != xt->sgl[idx].icg) &&
						 ((table->src_skip != 0 && xt->src_sgl) || (table->dst_skip != 0 && xt->dst_sgl)) ) {
						
						list_add_tail(&desc_tbl->elem, &d->desc_list);
						
						desc_tbl = ileaved_def_init_new_tdesc(c, xt, count, byte_cnt, d->frames);
						
						if (!desc_tbl)
							goto error_allocation;
						else
							table = desc_tbl->table;
							
						d->frames++;
					}
													
					table->count += act_size;
					
					/* 
					   Notice that the following statement will take into account ICG sizes bigger than S805_DMA_MAX_SKIP, 
					   so if an ICG bigger than the supported by the hardware is demanded a new block will be allocated
					   with the addresses offsetted as demanded by ICG. 
					
					*/
					
					count += (act_size + xt->sgl[idx].icg);
					byte_cnt += act_size;
							
					if (xt->src_sgl) {
						table->src_burst = act_size;

						if (xt->sgl[idx].icg <= S805_DMA_MAX_SKIP)
							table->src_skip = xt->sgl[idx].icg;
					}
						
					if (xt->dst_sgl) {
						table->dst_burst = act_size;

						if (xt->sgl[idx].icg <= S805_DMA_MAX_SKIP)
							table->dst_skip = xt->sgl[idx].icg;
					}
						
					if (xt->sgl[idx].icg > S805_DMA_MAX_SKIP) {
						
						list_add_tail(&desc_tbl->elem, &d->desc_list);
							
						desc_tbl = ileaved_def_init_new_tdesc(c, xt, count, byte_cnt, d->frames);
							
						if (!desc_tbl)
							goto error_allocation;
						else
							table = desc_tbl->table;
							
						d->frames++;

					} 
				}

				last.icg  = xt->sgl[idx].icg;
				last.size = xt->sgl[idx].size;
					
			} else { /* 1D move */
				
				for (tmp_size = xt->sgl[idx].size; tmp_size > 0; tmp_size -= S805_MAX_TR_SIZE) {
					
					if (tmp_size <= S805_MAX_TR_SIZE) {
						
						if ((table->count + tmp_size) > S805_MAX_TR_SIZE) {
							
							list_add_tail(&desc_tbl->elem, &d->desc_list);
							
							desc_tbl = ileaved_def_init_new_tdesc(c, xt, count, byte_cnt, d->frames);
							
							if (!desc_tbl)
								goto error_allocation;
							else
								table = desc_tbl->table;
							
							d->frames++;
						}
						
						table->count += tmp_size;
						count += tmp_size;
						
					} else {
						
						/* We need a new descriptor here */
						if (table->count) {
							
							list_add_tail(&desc_tbl->elem, &d->desc_list);
							
							desc_tbl = ileaved_def_init_new_tdesc(c, xt, count, byte_cnt, d->frames);
							
							if (!desc_tbl)
								goto error_allocation;
							else
								table = desc_tbl->table;
							
							d->frames++;
						}
						
						table->count += S805_MAX_TR_SIZE;
						count += S805_MAX_TR_SIZE;
					}
				}
			}
			
		} //j
		
	} //i

	/* Ensure the last descriptor will interrupt us */
    table->control |= S805_DTBL_IRQ;
	list_add_tail(&desc_tbl->elem, &d->desc_list);

	add_zeroed_tdesc(d);
	
	/* Assert cyclic false for this descriptor */
	d->cyclic = false;
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);
	
 error_allocation:
	
	dev_err(chan->device->dev, "Error allocating descriptors.");
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}
	
	kfree(d);
	return NULL;
}

static struct dma_async_tx_descriptor *
s805_dma_prep_dma_cyclic (struct dma_chan *chan,
						  dma_addr_t buf_addr,
						  size_t buf_len,
						  size_t period_len,
						  enum dma_transfer_direction direction,
						  unsigned long flags,
						  void *context)

{

	struct s805_chan *c = to_s805_dma_chan(chan);
	enum dma_slave_buswidth dev_width;
	struct s805_desc *d;
	dma_addr_t dst_addr, src_addr;
	s805_dtable * desc_tbl, * temp;
	unsigned int i, j, periods;

	/* If user didn't issued dmaengine_slave_config return */
	if ( (!c->cfg.src_addr && direction == DMA_DEV_TO_MEM) ||
		 (!c->cfg.dst_addr && direction == DMA_MEM_TO_DEV) ) {
		
		dev_err(chan->device->dev, "%s: Configuration not provided, please run dmaengine_device_control to set up a configuration before performing this operation.\n", __func__);
		return NULL;
	}
	
	switch (direction) {
	case DMA_DEV_TO_MEM:
		{
			dst_addr = buf_addr;
			src_addr = c->cfg.src_addr;
			dev_width = c->cfg.src_addr_width;
		}
		break;;
	case DMA_MEM_TO_DEV:
		{
			dst_addr = c->cfg.dst_addr;
			src_addr = buf_addr;
			dev_width = c->cfg.dst_addr_width;
		}
		break;;
	case DMA_MEM_TO_MEM:
	case DMA_DEV_TO_DEV:
		{
			/* Quick fix to treat all cases for buswidth errors */
			if (c->cfg.dst_addr_width != c->cfg.src_addr_width) {
				dev_err(chan->device->dev, "%s: Bad buswidth provided.", __func__);
				return NULL;
			} else
				dev_width = c->cfg.dst_addr_width;

			if (!c->cfg.dst_addr) {
				dst_addr = buf_addr;
				
				if (c->cfg.src_addr)
					src_addr = c->cfg.src_addr; 
				
				else {

					dev_err(chan->device->dev, "%s: Missing source address.", __func__);
					return NULL;
				}
			}

			if (!c->cfg.src_addr) {
				src_addr = buf_addr;
				
				if (c->cfg.dst_addr)
					dst_addr = c->cfg.dst_addr;
				
				else {

					dev_err(chan->device->dev, "%s: Missing destination address.", __func__);
					return NULL;
				}
			}
		}		
		break;;
	default:
	    dev_err(chan->device->dev, "%s: Unsupported direction: %i\n", __func__, direction);
		return NULL;
	}

	/* src address can't never match dst address - TO TRY -*/
	if (src_addr == dst_addr) {
			
		dev_err(chan->device->dev, "%s: Same source and destination address provided: 0x%08x\n", __func__, buf_addr);
		return NULL;
		
	}

	/* Datasheet p.57 entry 1 & 2 */
	if (dev_width != DMA_SLAVE_BUSWIDTH_8_BYTES) {
		
		dev_err(chan->device->dev, "%s: Unsupported buswidth: %i\n", __func__, dev_width);
		return NULL;
	}
	
	if (buf_len % period_len)
		dev_warn(chan->device->dev,
				 "%s: buffer_length (%zd) is not a multiple of period_len (%zd).\n",
				 __func__, buf_len, period_len);
	
	/* Is that warning fair? */
	if (buf_len > 1 && direction == DMA_DEV_TO_DEV)
		dev_warn(chan->device->dev,
				 "%s: buffer_length (%u) is >1 while direction is DMA_DEV_TO_DEV.\n",
				 __func__, buf_len);
	
	periods = DIV_ROUND_UP(buf_len, period_len);
	
	/* Allocate and setup the descriptor. */
	d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;

	d->c = c;
	d->frames = 0;
	INIT_LIST_HEAD(&d->desc_list);

	for (i = 0; i < periods; i++) {

		/* 
		   Optimize this to minimize the amount of descriptors used 
		   
		   Idea: Use period_len as the burst (for src and dst) for our transaction, so we can hold dst or src (or both) for a desired amount of bytes (buffer_len),  
		   depending on the direction found in cfg.
		   
		   * Is this supported by the hw?, If src_hold is set and a src_burst is provided will the src pointer advace till the end of the 
		   burst and then start at the beggining again? or the src addr will be held for all the burst?

		   To be tested!
		   
		*/
		
		for (j = 0; j < period_len; j += S805_MAX_TR_SIZE) {
			
			s805_dtable *desc_tbl = def_init_new_tdesc(c, d->frames);

			if (!desc_tbl) 
				goto error_list;
			
			switch (direction) {
			case DMA_DEV_TO_MEM:
				{
					desc_tbl->table->control |= S805_DTBL_SRC_HOLD;
					desc_tbl->table->src = src_addr;
					desc_tbl->table->dst = dst_addr + (dma_addr_t)j;
					
				}
				break;;
			case DMA_MEM_TO_DEV:
				{
					desc_tbl->table->control |= S805_DTBL_DST_HOLD;
					desc_tbl->table->src = src_addr + (dma_addr_t)j;
					desc_tbl->table->dst = dst_addr;
					
				}
				break;;
			case DMA_MEM_TO_MEM:
				{

					desc_tbl->table->src = src_addr + (dma_addr_t)j;
					desc_tbl->table->dst = dst_addr + (dma_addr_t)j;
					
				}
				break;;
			case DMA_DEV_TO_DEV:
				{
					/* Again, is that a fair assumption ? */
					desc_tbl->table->control |= (S805_DTBL_SRC_HOLD | S805_DTBL_DST_HOLD);
					desc_tbl->table->src = src_addr;
					desc_tbl->table->dst = dst_addr;
				}		
				break;;
			default:
				dev_err(chan->device->dev, "%s: Unsupported direction: %i\n", __func__, direction);
				return NULL;
			}
				
			/* Fill byte count for the block move */
			desc_tbl->table->count = min((period_len - j) & S805_MAX_TR_SIZE, (uint) S805_MAX_TR_SIZE);
			
			list_add_tail(&desc_tbl->elem, &d->desc_list);

			d->frames ++;
		}
	}

	/* Ensure that the last descriptor will interrupt us. */
	list_entry(d->desc_list.prev, s805_dtable, elem)->table->control |= S805_DTBL_IRQ;

	add_zeroed_tdesc(d);
	
	/* Assert cyclic true for this descriptor */
	d->cyclic = true;
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);

 error_list:

	dev_err(chan->device->dev, "%s: Error allocating descriptors.", __func__);
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}

	kfree(d);
	return NULL;
}

/* Auxiliar functions for DMA_SG: */

static inline void fwd_dst (struct sg_info * info) {

	if (info->dst_cursor) {
		
		info->dst_cursor = info->next_dst;
		info->next_dst = sg_next(info->dst_cursor);
		info->dst_bytes = 0;
		
	}
	
}

static inline void fwd_src (struct sg_info * info) {

	if (info->src_cursor) {

		info->src_cursor = info->next_src;
		info->next_src = sg_next(info->src_cursor);
		info->src_bytes = 0;
		
	}
}

static int get_src_icg (struct sg_info * info) {

	/* 
	   Notice the integer return type, since we don't know if information will be arranged sequentially in memory. 
	
	*/

	if (info->next_dst)
		return sg_dma_address(info->next_src) - sg_dma_address(info->src_cursor);
	else
		return -1;
	
}

static int get_dst_icg (struct sg_info * info) {

	/* 
	   Notice the integer return type, since we don't know if information will be arranged sequentially in memory. 
	
	*/

	if (info->next_dst)
		return sg_dma_address(info->next_dst) - sg_dma_address(info->dst_cursor);
	else
		return -1;
}

static uint get_src_remain (struct sg_info * info) {

	if (info->src_cursor)
		return sg_dma_len(info->src_cursor) - info->src_bytes;
	else
		return UINT_MAX;
}

static uint get_dst_remain (struct sg_info * info) {
	
	if (info->dst_cursor)
		return sg_dma_len(info->dst_cursor) - info->dst_bytes;
	else
		return UINT_MAX;
}

static bool got_cursor (struct sg_info * info) {

	return (info -> src_cursor || info -> dst_cursor);

}

/* Not needed for this approach, preserved until test. */
static s805_dtable * get_next_chunk (s805_dtable * curr, struct s805_desc *d) {

	if (list_is_last(&curr->elem, &d->desc_list))
		return NULL;
	else
		return list_next_entry(curr, elem);
}

static s805_dtable * move_along (s805_dtable * cursor, struct s805_desc *d) {

	s805_dtable * aux = get_next_chunk(cursor, d);
	
	if (!aux) {

		/* Log debug here. */
		
		list_add_tail(&cursor->elem, &d->desc_list);
		d->frames ++;
		
		return def_init_new_tdesc(d->c, d->frames);

	} else
		return aux;

}

static bool src_ent_complete (struct sg_info * info) {

	if (info->src_cursor)
		return sg_dma_len(info->src_cursor) == info->src_bytes;
	else
		return false;
}

static bool dst_ent_complete (struct sg_info * info) {

	if (info->dst_cursor)
		return sg_dma_len(info->dst_cursor) == info->dst_bytes;
	else
		return false;
}

/* END of Auxiliar functions for DMA_SG */

static struct dma_async_tx_descriptor *
s805_dma_prep_sg (struct dma_chan *chan,
				  struct scatterlist *dst_sg, unsigned int dst_nents,
				  struct scatterlist *src_sg, unsigned int src_nents,
				  unsigned long flags)
{
	struct s805_chan *c = to_s805_dma_chan(chan);
	struct s805_desc *d;
	struct sg_info pinfo;
	s805_dtable * desc_tbl, * temp;
	unsigned int j, src_len, dst_len, icg, next_icg, next_burst, bytes = 0;
	dma_addr_t src_addr, dst_addr;
	int min_size, act_size;
	bool new_block;
	
	//struct data_chunk last_dst, last_src;
	for_each_sg(src_sg, pinfo.src_cursor, src_nents, j) 
		bytes += sg_dma_len(pinfo.src_cursor);
	
	for_each_sg(dst_sg, pinfo.dst_cursor, dst_nents, j) 
		bytes -= sg_dma_len(pinfo.dst_cursor);
	
	if (bytes != 0) { 
		
		dev_err(chan->device->dev, "%s: Length for destination and source sg lists differ. \n", __func__);
		return NULL;
	}
	
	/* Allocate and setup the descriptor. */
	d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;
	
	d->c = c;
	d->frames = 0;
	INIT_LIST_HEAD(&d->desc_list);

	/* Auxiliar struct to iterate the lists. */
	pinfo.src_cursor = src_sg;
	pinfo.dst_cursor = dst_sg;
	
	pinfo.next_src = sg_next(pinfo.src_cursor);
	pinfo.next_dst = sg_next(pinfo.dst_cursor);
	
	pinfo.src_bytes = 0;
	pinfo.dst_bytes = 0;
	
	desc_tbl = def_init_new_tdesc(c, d->frames);
	
	src_addr = sg_dma_address(pinfo.src_cursor);
	dst_addr = sg_dma_address(pinfo.dst_cursor);
	
	/* "Fwd logic" not optimal, first approach, must do. */
	while (got_cursor(&pinfo)) {
		
		src_len = get_src_remain(&pinfo);
		dst_len = get_dst_remain(&pinfo);
	   
	    min_size = min(dst_len, src_len);
		
	    while (min_size > 0) {
			
			act_size = min_size <= S805_MAX_TR_SIZE ? min_size : S805_MAX_TR_SIZE;
			
			if ((desc_tbl->table->count + act_size) > S805_MAX_TR_SIZE) {

				desc_tbl = move_along(desc_tbl, d);
					
				if (!desc_tbl)
					goto error_allocation;
				
				desc_tbl->table->src = src_addr + (dma_addr_t) pinfo.src_bytes;
				desc_tbl->table->dst = dst_addr + (dma_addr_t) pinfo.dst_bytes;	
					
			}
			
		    desc_tbl->table->count += act_size;
			
			pinfo.src_bytes += act_size;
			pinfo.dst_bytes += act_size;
			
		    min_size -= act_size;
		}

		new_block = true;
	   
		if (src_ent_complete(&pinfo)) {

			icg = get_src_icg(&pinfo);
			next_burst = sg_dma_len(pinfo.next_src);
			fwd_src(&pinfo);
			next_icg = get_src_icg(&pinfo);
				
			if (!desc_tbl->table->src_burst) {

				/* This check will ensure that the next block will fit in the src_burst size we will allocate right after this lines. */
				if (next_burst == desc_tbl->table->count &&
					desc_tbl->table->count <= S805_DMA_MAX_BURST) {
					
				   
					if (icg > 0 && icg <= S805_DMA_MAX_SKIP && icg == next_icg) {
						
							
							desc_tbl->table->src_burst = desc_tbl->table->count; 
							desc_tbl->table->src_skip = icg;
							new_block = false;	
						
					}
				}
			}
			
			src_addr = sg_dma_address(pinfo.src_cursor);
		}
			
		if (dst_ent_complete(&pinfo)) {

			icg = get_dst_icg(&pinfo);
			next_burst = sg_dma_len(pinfo.next_dst);
			fwd_dst(&pinfo);
			next_icg = get_dst_icg(&pinfo);

			if (!desc_tbl->table->dst_burst) {

				/* This check will ensure that the next block will fit in the dst_burst size we will allocate right after this lines. */
				if (next_burst == desc_tbl->table->count &&
					desc_tbl->table->count <= S805_DMA_MAX_BURST) {
				
					if (icg > 0 && icg <= S805_DMA_MAX_SKIP && icg == next_icg) {
						
						desc_tbl->table->dst_burst = desc_tbl->table->count;
						desc_tbl->table->dst_skip = icg;
						new_block = false;
						
					}
				}
			}

			dst_addr = sg_dma_address(pinfo.dst_cursor);
		}
					  
		if (new_block) {
			
			desc_tbl = move_along(desc_tbl, d);
			
			if (!desc_tbl)
				goto error_allocation;
			
			desc_tbl->table->src = src_addr + (dma_addr_t)pinfo.src_bytes;
			desc_tbl->table->dst = dst_addr + (dma_addr_t)pinfo.dst_bytes;
		}
	}

	/* Ensure that the last descriptor will interrupt us. */
	list_entry(d->desc_list.prev, s805_dtable, elem)->table->control |= S805_DTBL_IRQ;

	add_zeroed_tdesc(d);
	
	/* Assert cyclic false for this descriptor */
	d->cyclic = false;
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);
	
 error_allocation:
	
	dev_err(chan->device->dev, "%s: Error allocating descriptors.", __func__);
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}

	kfree(d);
	return NULL;
}

struct dma_async_tx_descriptor *
s805_dma_prep_memcpy (struct dma_chan *chan,
					  dma_addr_t dest,
					  dma_addr_t src,
					  size_t len,
					  unsigned long flags)
{

	struct s805_chan *c = to_s805_dma_chan(chan);
	struct s805_desc *d;
	s805_dtable * desc_tbl, * temp;
	unsigned int tmp_size, act_size, bytes = 0;
	
	/* Allocate and setup the descriptor. */
	d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;
	
	d->c = c;
	d->frames = 0;
	INIT_LIST_HEAD(&d->desc_list);
	
	desc_tbl = def_init_new_tdesc(c, d->frames);
	d->frames ++;
	
	desc_tbl->table->src = src;
	desc_tbl->table->dst = dest;
	
	for (tmp_size = len; tmp_size > 0; tmp_size -= S805_MAX_TR_SIZE) {

		act_size = tmp_size > S805_MAX_TR_SIZE ? S805_MAX_TR_SIZE : tmp_size;
		
		if ((desc_tbl->table->count + act_size) > S805_MAX_TR_SIZE) {
			
			desc_tbl = def_init_new_tdesc(c, d->frames);
			
			if (!desc_tbl)
				goto error_allocation;
			
			desc_tbl->table->src = src + (dma_addr_t) bytes;
			desc_tbl->table->dst = dest + (dma_addr_t) bytes;	

			d->frames ++;
		}

		desc_tbl->table->count += act_size;
		bytes += act_size;
			
	}
	
	/* Ensure that the last descriptor will interrupt us. */
	list_entry(d->desc_list.prev, s805_dtable, elem)->table->control |= S805_DTBL_IRQ;

	add_zeroed_tdesc(d);
	
	/* Assert cyclic false for this descriptor */
	d->cyclic = false;
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);
	
 error_allocation:
	
	dev_err(chan->device->dev, "%s: Error allocating descriptors.", __func__);
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}

	kfree(d);
	return NULL;
}

struct dma_async_tx_descriptor *
s805_dma_prep_memset (struct dma_chan *chan,
					  dma_addr_t dest,
					  int value,
					  size_t len,
					  unsigned long flags)
{

	struct s805_chan *c = to_s805_dma_chan(chan);
	struct s805_desc *d;
	s805_dtable * desc_tbl, * temp;
	unsigned int tmp_size, act_size, bytes = 0;

	/* Allocate and setup the descriptor. */
	d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;
	
	d->c = c;
	d->frames = 0;
	INIT_LIST_HEAD(&d->desc_list);
	
	d->memset = kzalloc(sizeof(struct memset_info), GFP_NOWAIT);
	
	if (!d->memset) {
		
		kfree(d);
		return NULL;
		
	}
	
	d->memset->value = dma_alloc_coherent(chan->device->dev,
										  sizeof(long long),
										  &d->memset->paddr,
										  GFP_NOWAIT); 
	
	if (!d->memset->value) {

		kfree(d);
		kfree(d->memset);
		
		return NULL;
		
	} else
		*d->memset->value = value;

	/*

	  The following statements will concatenate the integer value (32 bits long) into a 
	  long long value (64 bits long), given that the hardware is only capable of moving
	  the data in chunks of 64 bits. This means that if "dest" address is pointing to an
	  array or a buffer of integers (positions of 32 bits long) the desired result will 
	  be achieved, however the buffer MUST be aligned to 8 Bytes to avoid writting 
	  undesired addresses. 

	  To be tested!
	  
	 */
	
	*d->memset->value <<= (sizeof(int) * 8);
    *d->memset->value |= (value & LOWER_32);
	
	desc_tbl = def_init_new_tdesc(c, d->frames);
	d->frames ++;
	
	desc_tbl->table->src = d->memset->paddr;
	desc_tbl->table->dst = dest;
	desc_tbl->table->control |= S805_DTBL_SRC_HOLD;

	for (tmp_size = len; tmp_size > 0; tmp_size -= S805_MAX_TR_SIZE) {

		act_size = tmp_size > S805_MAX_TR_SIZE ? S805_MAX_TR_SIZE : tmp_size;
		
		if ((desc_tbl->table->count + act_size) > S805_MAX_TR_SIZE) {
			
			desc_tbl = def_init_new_tdesc(c, d->frames);
			
			if (!desc_tbl)
				goto error_allocation;
			
			desc_tbl->table->src = d->memset->paddr;
			desc_tbl->table->dst = dest + (dma_addr_t) bytes;	
			desc_tbl->table->control |= S805_DTBL_SRC_HOLD;

			d->frames ++;
			
		}

		desc_tbl->table->count += act_size;
		bytes += act_size;
	}

	/* Ensure that the last descriptor will interrupt us. */
	list_entry(d->desc_list.prev, s805_dtable, elem)->table->control |= S805_DTBL_IRQ;

	add_zeroed_tdesc(d);
	
	/* Assert cyclic false for this descriptor */
	d->cyclic = false;
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);

 error_allocation:
	
	dev_err(chan->device->dev, "%s: Error allocating descriptors.", __func__);
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}

	dma_free_coherent(chan->device->dev, sizeof(long long), d->memset->value, d->memset->paddr);

	kfree(d->memset);
	kfree(d);
	return NULL;
	
} 

struct dma_async_tx_descriptor *
s805_dma_prep_interrupt (struct dma_chan *chan,
						 unsigned long flags)
{


	/* 
	   I don't know exactly what is expected for this capability, with this implementation a 
	   new descriptor will be allocated and when the associated descriptor happens to be issued 
	   this cookie will be marked as completed, hence the associated callback (defined by the user)
	   will be called. So if the "dma_async_tx_descriptor" returned from this function belongs to a 
	   chain of descriptors, particullary its the last of a chain of descriptors, the associated 
	   callback will be triggered. As mentioned above I'm not sure if this is the expected behaviour 
	   for dma_prep_interrupt, if it is not, please correct it.  
 
	*/

	struct s805_chan *c = to_s805_dma_chan(chan);
	struct s805_desc *d;
	
	/* Allocate and setup the descriptor. */
	d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;
	
	d->c = to_s805_dma_chan(chan);
	INIT_LIST_HEAD(&d->desc_list);
	
	d->cyclic = false;
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);
	

}

/* Function passed to virtual channels to free resources */
static void s805_dma_desc_free(struct virt_dma_desc *vd)
{

	struct s805_desc * d = to_s805_dma_desc(&vd->tx);
	s805_dtable * desc_tbl, * temp;
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(d->c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}

	if (d->memset)
		dma_free_coherent(d->c->vc.chan.device->dev, sizeof(long long), d->memset->value, d->memset->paddr);

	dev_dbg(d->c->vc.chan.device->dev, "Descriptor 0x%p: Freed.", vd);
	
	kfree(d);
}

/*
  
  Write general CLK register to enable engine  
  
 */

static inline void s805_dma_enable_hw ( void ) { 
	
	u32 status = RD(S805_DMA_CLK);
	WR(status | S805_DMA_ENABLE, S805_DMA_CLK);
	
    status = RD(S805_DMA_CTRL);
	WR(status | S805_DMA_ENABLE, S805_DMA_CTRL);
	
}

static inline void s805_dma_thread_disable ( uint thread_id ) { 
   	
	u32 reg_val;
	
	reg_val = RD(S805_DMA_THREAD_CTRL);
	WR(reg_val & ~S805_DMA_THREAD_ENABLE(thread_id), S805_DMA_THREAD_CTRL);
	
}

/*
  
  Write s805 DMAC registers with start and end address of table descriptor list
  
  @addr: address of the descriptor to be processed.
  @frames: amount of descriptors to be processed.
  
*/

static dma_addr_t s805_dma_allocate_tr (uint thread_id, dma_addr_t addr, uint frames) 
{
	
    u32 status;
	u32 str_addr, end_addr;
	
	uint amount = min(frames, (uint) S805_DMA_MAX_DESC); 
	
	s805_dma_thread_disable(thread_id);

	str_addr = addr;
	end_addr = addr + (amount * sizeof(struct s805_table_desc_entry));

	switch(thread_id) {
	case 0:
		WR(str_addr, S805_DMA_DLST_STR0);
		WR(end_addr, S805_DMA_DLST_END0);
		break;
	case 1:
		WR(str_addr, S805_DMA_DLST_STR1);
		WR(end_addr, S805_DMA_DLST_END1);
		break;
	case 2:
		WR(str_addr, S805_DMA_DLST_STR2);
		WR(end_addr, S805_DMA_DLST_END2);
		break;
	case 3:
		WR(str_addr, S805_DMA_DLST_STR3);
		WR(end_addr, S805_DMA_DLST_END3);
		break;
	}
	
	/* Pulse thread init to register table positions (token from crypto module) */
	status = RD(S805_DMA_THREAD_CTRL);
	WR(status | S805_DMA_THREAD_INIT(thread_id), S805_DMA_THREAD_CTRL);
	
	/* Reset count register (for this thread) and write count value for the descriptor list */
	WR(S805_DMA_ADD_DESC(thread_id, 0x00), S805_DTBL_ADD_DESC);
	WR(S805_DMA_ADD_DESC(thread_id, amount), S805_DTBL_ADD_DESC);
	
	return amount < frames ? (end_addr + sizeof(struct s805_table_desc_entry)) : 0;
}

/* Protected by manager/general lock (serialized) */
static void s805_dma_schedule_tr ( struct s805_chan * c ) {

	struct virt_dma_desc *vd, *tmp;
	struct s805_desc * d;
#ifdef DEBUG
	s805_dtable *desc;
#endif

	list_for_each_entry_safe (vd, tmp, &c->vc.desc_issued, node) {

		d = to_s805_dma_desc(&vd->tx);
		
#ifdef DEBUG

		list_for_each_entry (desc, &d->desc_list, elem) {

			/* Last descriptors will be zeroed */
			if (!list_is_last(&desc->elem, &d->desc_list)) {

				dev_dbg(d->c->vc.chan.device->dev, "Descriptor (0x%08X) >> ctrl = 0x%08X, src = 0x%08X, dst = 0x%08X, byte_cnt = %u, src_burst = %u, src_skip = %u, dst_burst = %u, dst_skip = %u\n",
						desc->paddr,
						desc->table->control, 
						desc->table->src, 
						desc->table->dst, 
						desc->table->count,
						desc->table->src_burst,
						desc->table->src_skip,
						desc->table->dst_burst,
						desc->table->dst_skip);
			}
		}
#endif
		
		if (list_empty(&d->desc_list)) {
			
			/* 
			   This descriptor comes from device_prep_interrupt, so mark the cookie as completed to trigger 
			   the associated callback, and try to process any pending descriptors.
			*/
			
			vchan_cookie_complete(&d->vd);
			continue;
		}

		spin_lock(&mgr->lock);
		list_add_tail(&d->elem, &mgr->scheduled);
		spin_unlock(&mgr->lock);
		
		if (!d->cyclic)
			list_del(&vd->node);

	}
}

/* Start the given thread */
static inline void s805_dma_thread_enable ( uint thread_id ) { 
   	
	u32 reg_val;
	
    reg_val = RD(S805_DMA_THREAD_CTRL);
	WR(reg_val | S805_DMA_THREAD_ENABLE(thread_id), S805_DMA_THREAD_CTRL);
	
}

/* Fetch a new previously scheduled transaction. (Protected by mgr->lock)*/
static void s805_dma_fetch_tr ( uint ini_thread ) {

	uint thread;
	struct s805_desc * d;
	
	for (thread = ini_thread; thread < S805_DMA_MAX_THREAD; thread ++) {
		
		
		d = list_first_entry_or_null(&mgr->scheduled, struct s805_desc, elem);
	
		while (d) {

			if (d->c->status != DMA_PAUSED)
				break;
			else {
				
				if (list_is_last(&d->elem, &mgr->scheduled))
				    d = NULL;
				else
					d = list_next_entry(d, elem);
			}			
		}
		
		if (d) {
			
			d->next = s805_dma_allocate_tr (thread,
											d->next ? d->next : list_first_entry(&d->desc_list,
																				 s805_dtable,
																				 elem)->paddr,
											d->frames);
			
		    d->c->status = DMA_IN_PROGRESS;
			
			list_move_tail(&d->elem, &mgr->in_progress);
			
			s805_dma_thread_enable(thread);
			
		    mgr->busy = true;
		} 
	}
}

/* 
   Process, if existent, the next descriptor for a DMA channel
   
   @c: s805_chan to check for pending descriptors
   
 */
static enum dma_status s805_dma_process_next_desc ( struct s805_chan *c )
{

	if (c->status != DMA_PAUSED) {

		c->status = DMA_IN_PROGRESS;
		
		s805_dma_schedule_tr(c);
	}
	
	/* 
	   We may face two different situations here: 
		   
	      *  Either the first descriptors in the thread queues are the once we just allocated ...
	      *  Or there are paused descriptors in the head of the queues.
				  
	   In both cases "s805_dma_fetch_tr" will start the proper transaction, it is, the first belonging
	   to a non paused channel.
	  			   
	*/

	spin_lock(&mgr->lock);
	if (!mgr->busy) 
		s805_dma_fetch_tr(0);
	
	spin_unlock(&mgr->lock);
	
	return c->status;
}

/* Process completed descriptors */
static void s805_dma_process_completed ( void )
{
	struct s805_desc * d, * temp;
	uint thread = 0;

	list_for_each_entry_safe (d, temp, &mgr->completed, elem) {
		
		/* All the transactions has been completed, process the finished descriptors.*/
		
		if (!d->next) {

			if (d->cyclic) {
				
				dev_dbg(d->c->vc.chan.device->dev, "Cookie %d completed, calling cyclic callback.\n", d->vd.tx.cookie);
				vchan_cyclic_callback(&d->vd);

				spin_lock(&mgr->lock);
				list_move_tail(&d->elem, &mgr->scheduled);
				spin_unlock(&mgr->lock);
				
			} else { 
				
				dev_dbg(d->c->vc.chan.device->dev, "Marking cookie %d completed.\n", d->vd.tx.cookie);

				list_del(&d->elem);
				
				spin_lock(&d->c->vc.lock);
				
				d->c->status = DMA_SUCCESS;
				vchan_cookie_complete(&d->vd);
				
				spin_unlock(&d->c->vc.lock);
				
			}
				
		} else {
				
			/* 
			   If we reach this code the scheduled descriptor have more than S805_DMA_MAX_DESC data chunks, 
			   so we need to restart the transaction from the last descriptor processed. 
			*/
			
			d->frames -= S805_DMA_MAX_DESC;
			
			dev_dbg(d->c->vc.chan.device->dev, "Re-scheduling cookie %d, frames left: %u.\n", d->vd.tx.cookie, d->frames);
			
			if (d->c->status != DMA_PAUSED) {
				
				d->next = s805_dma_allocate_tr (thread, d->next, d->frames);
				
				spin_lock(&mgr->lock);
				list_move_tail(&d->elem, &mgr->in_progress);
				spin_unlock(&mgr->lock);
				
				s805_dma_thread_enable(thread);
				thread ++;
				
			} else {
				
				spin_lock(&mgr->lock);
				list_move_tail(&d->elem, &mgr->scheduled);
				spin_unlock(&mgr->lock);
			}
		}
	}
	
	spin_lock(&mgr->lock);
	if (!mgr->busy) 
		s805_dma_fetch_tr(thread);
	
	spin_unlock(&mgr->lock);
}


/* 
   
   IRQ callback function: All transactions must submit one IRQ when a batch of at most four chunks is finished, 
   if needed a new chunk can be remaped for the same transaction, if the transaction is finished we will fetch 
   a new transaction, if existent.
   
*/

static irqreturn_t s805_dma_callback (int irq, void *data)
{

	struct s805_dmadev * m = (struct s805_dmadev *) data;
	
	list_move_tail (&list_first_entry(&m->in_progress,
									  s805_dtable,
									  elem)->elem,
					&m->completed);
	
	if (list_empty(&m->in_progress)) {
		
		spin_lock(&m->lock);
		m->busy = false;
		spin_unlock(&m->lock);
		
		s805_dma_process_completed();	
		
	}
	
	return IRQ_HANDLED;
}

/* Dismiss a previously scheduled descriptor */
static void s805_dma_dismiss_chann ( struct s805_chan * c ) {

	struct s805_desc * d, * tmp;
	
	list_for_each_entry_safe (d, tmp, &mgr->scheduled, elem) {

		if ( d->c == c )  {
			
			s805_dma_desc_free(&d->vd);
			list_del(&d->elem);
		}
	}

	/* Transactions in progress will finish, how is the proper way to treat this? */
}

/*
  Bypass function for device_control (dmaengine interface)
  
  @chan: channel to set up.
  @cmd: command to perform.
  @arg: pointer to a dma_slave_config struct, for DMA_SLAVE_CONFIG only.
   
*/
static int s805_dma_control (struct dma_chan *chan, 
							 enum dma_ctrl_cmd cmd,
							 unsigned long arg) 
{
	
    struct s805_chan * c = to_s805_dma_chan(chan);
	LIST_HEAD(head);
	
	switch (cmd) {
	case DMA_TERMINATE_ALL:
		{
			/* Pause the channel to stop any transaction in course */
			spin_lock(&c->vc.lock);

			c->status = DMA_PAUSED;
			
			vchan_dma_desc_free_list(&c->vc, &c->vc.desc_submitted);
			
			spin_unlock(&c->vc.lock);
		   	
			/* Dismiss all pending operations */
			spin_lock(&mgr->lock);
			s805_dma_dismiss_chann(c);
			spin_unlock(&mgr->lock);
			
			spin_lock(&c->vc.lock);
			c->status = DMA_SUCCESS;
			spin_lock(&c->vc.lock);
		}
		break;;
		
	case DMA_PAUSE:
		{
			
			/* 
			   If there is a transaction in progress we will let the current batch of desciptors finish, 
			   a new batch won't be scheduled however. 
			*/

			spin_lock(&c->vc.lock);
			
			c->status = DMA_PAUSED;

			spin_unlock(&c->vc.lock);
		}
		break;;
		
	case DMA_RESUME:
		{
			if (c->status == DMA_PAUSED) {
				
				spin_lock(&c->vc.lock);
				c->status = DMA_SUCCESS;
				
				if (vchan_issue_pending(&c->vc)) 
					s805_dma_process_next_desc(c);

				else {
					
					spin_lock(&mgr->lock);
					if (!mgr->busy) 
						s805_dma_fetch_tr(0);
					
					spin_unlock(&mgr->lock);
				}
				
				spin_unlock(&c->vc.lock);
			}
		}
	    break;;
	case DMA_SLAVE_CONFIG: /* We need to perform this before dma_prep_slave_sg */
		{
			struct dma_slave_config *cfg = (struct dma_slave_config *) arg;
			
			/* If device to memory (write) we need the src 32 bit address present*/
			if ((cfg->direction == DMA_DEV_TO_MEM) &&
				(cfg->src_addr_width != DMA_SLAVE_BUSWIDTH_8_BYTES || !cfg->src_addr))  
				{
					return -EINVAL;
				}
			
			/* If memory to device (read) we need the dst 32 bit address present */
			if ((cfg->direction == DMA_MEM_TO_DEV) &&
				(cfg->src_addr_width != DMA_SLAVE_BUSWIDTH_8_BYTES || !cfg->dst_addr)) 
				{
					return -EINVAL;
				}
				
			/* If device to device (What exactly this means?? does it goes here??) we need both 32 bit addresses present */
			if ((cfg->direction == DMA_DEV_TO_DEV) &&
				(((cfg->dst_addr_width != DMA_SLAVE_BUSWIDTH_8_BYTES) || (cfg->src_addr_width != DMA_SLAVE_BUSWIDTH_8_BYTES)) ||
				 (!cfg->dst_addr || !cfg->src_addr)))
				{
					
					return -EINVAL;
				}
			
			c->cfg = *cfg;
		}
		break;;
		
	default:
		dev_err(c->vc.chan.device->dev, "Unsupported cmd: %d\n", cmd);
		return -EINVAL;
	}
	
	return c->status;
}


/*
  Bypass function for dma_tx_status (dmaengine interface)
  
  @chan: channel holding the information for the transaction.
  @cookie: cookie identifier of the transaction
  @txstate: output parameter, to be filled by the rutine.

*/
enum dma_status s805_dma_tx_status(struct dma_chan *chan,
								   dma_cookie_t cookie,
								   struct dma_tx_state *txstate) 
{
	
	enum dma_status ret;
	struct s805_desc * d, * temp;
	s805_dtable *desc;
	u32 residue = 0;
	
	ret = dma_cookie_status(chan, cookie, txstate);
	
	if (ret == DMA_SUCCESS)
		return ret;

	/* Underprotected: to be tested! */
	list_for_each_entry_safe (d, temp, &mgr->scheduled, elem) {
		
		if (d->vd.tx.cookie == cookie) {
			
			list_for_each_entry (desc, &d->desc_list, elem)
				residue += desc->table->count;
		}
	}
	
	dma_set_residue(txstate, residue);
	
	return ret;
}

/*
  Bypass function for dma_issue_pending (dmaengine interface)
  
  @chan: channel to issue pending ops.

*/
static void s805_dma_issue_pending(struct dma_chan *chan)
{
	struct s805_chan *c = to_s805_dma_chan(chan);
	
	spin_lock(&c->vc.lock);

	if (vchan_issue_pending(&c->vc)) 
		s805_dma_process_next_desc(c);
	
	spin_unlock(&c->vc.lock);
}

/*
  Bypass function for free_chan_resources (dmaengine interface)
  
  @chan: channel to free resources for.

*/
static void s805_dma_free_chan_resources(struct dma_chan *chan)
{
	struct s805_chan *c = to_s805_dma_chan(chan);
	
	vchan_free_chan_resources(&c->vc);
	dma_pool_destroy(c->pool);
	
	dev_dbg(c->vc.chan.device->dev, "Freeing DMA channel.\n");
}

/*
  Bypass function for alloc_chan_resources (dmaengine interface)
  
  @chan: channel to allocate resources for.

*/
static int s805_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct s805_chan *c = to_s805_dma_chan(chan);
	struct device *dev = c->vc.chan.device->dev;

	dev_dbg(dev, "Allocating DMA channel.\n");

	c->pool = dma_pool_create(dev_name(dev),
							  dev,
							  sizeof(struct s805_table_desc),
							  sizeof(struct s805_table_desc),
							  0);
	if (!c->pool) {
		
		dev_err(dev, "Unable to allocate descriptor pool.\n");
		return -ENOMEM;
		
	}
	
	return 0;  
}

/* Allocate s805 channel structures */
static int s805_dma_chan_init (struct s805_dmadev *d)
{
	struct s805_chan *c;
	
	c = devm_kzalloc(d->ddev.dev, sizeof(struct s805_chan), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	
	c->vc.desc_free = s805_dma_desc_free;
	vchan_init(&c->vc, &d->ddev);
	
	c->status = DMA_SUCCESS;
	
	return 0;
}

/* Allocation of the global structures that will hold DMA manager information */
static int s805_dmamgr_probe(struct platform_device *pdev)
{	
	mgr = devm_kzalloc(&pdev->dev, sizeof(struct s805_dmadev), GFP_KERNEL);
	if (!mgr)
		return -ENOMEM;
	
    spin_lock_init(&mgr->lock);
	
	INIT_LIST_HEAD(&mgr->scheduled);
	INIT_LIST_HEAD(&mgr->in_progress);
	INIT_LIST_HEAD(&mgr->completed);
	
	dev_info(&pdev->dev, "DMA legacy API manager at 0x%p\n", mgr);
	
	mgr->irq_number = S805_DMA_IRQ;
	
	return request_irq(mgr->irq_number, s805_dma_callback, 0, "s805_dmaengine_irq", mgr);
}


/* Free DMA s805 device REVISAR */
static void s805_dma_free(struct s805_dmadev *sd)
{
	/* Check for active descriptors. */
	struct s805_chan *c, *next;

	list_for_each_entry_safe(c, next, &sd->ddev.channels,
							 vc.chan.device_node) {
		
		list_del(&c->vc.chan.device_node);
		tasklet_kill(&c->vc.task);
	}

	free_irq(mgr->irq_number, mgr);
	
}

static int get_chan_num_cmdline (char *str)
{
	
	get_option(&str, &dma_channels);
    return 1;
	
}

__setup("dma_channels=", get_chan_num_cmdline);


/* Probe subsystem */
static int s805_dma_probe (struct platform_device *pdev)
{
	
	struct s805_dmadev *sd;
	int ret, i;
	
	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;
	
	ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(32)); //DMA_TX_TYPE_END
	
	if (ret)
		return ret;
	
	dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	
	/* S805 DMAC device */
	sd = devm_kzalloc(&pdev->dev, sizeof(struct s805_dmadev), GFP_KERNEL);
	
	if (!sd)
		return -ENOMEM;

	ret = s805_dmamgr_probe(pdev);
	
	if (ret) 	
	    goto err_no_dma;
	else
		mgr->chan_available = dma_channels;
	
	if (!mgr->chan_available) {
		
		/* If no cmd line param present request DMA channel number from device tree */ 
		if (of_property_read_u32(pdev->dev.of_node,
								 "aml,dma-channel-num",
								 &mgr->chan_available)) {
			
			dev_err(&pdev->dev, "Failed to get channel number\n");
			ret = -EINVAL;
			
			goto err_no_dma;
		}	
	}
	
	pdev->dev.dma_parms = &sd->dma_parms;
	
	/* Datasheet p.57, entry 3 */
	dma_set_max_seg_size(&pdev->dev, S805_MAX_TR_SIZE); // is this correct?
	
	/* 
	   DMA_CAPABILITIES: All channels needs to be either private or public, we don't set the DMA_PRIVATE capabilitie 
	   to make them all public so we can give support to the async_tx api and network or audi drivers. 
	*/

	//dma_cap_set(DMA_PRIVATE, sd->ddev.cap_mask);
	dma_cap_set(DMA_SLAVE, sd->ddev.cap_mask);
	dma_cap_set(DMA_INTERRUPT, sd->ddev.cap_mask);
	
	dma_cap_set(DMA_ASYNC_TX, sd->ddev.cap_mask);
	dma_cap_set(DMA_INTERLEAVE, sd->ddev.cap_mask);

	/* Those needs to be exposed in linux/dmaengine.h, maybe backported from 4.x */
	dma_cap_set(DMA_CYCLIC, sd->ddev.cap_mask);
	dma_cap_set(DMA_SG, sd->ddev.cap_mask);
	dma_cap_set(DMA_MEMCPY,  sd->ddev.cap_mask);
	dma_cap_set(DMA_MEMSET,  sd->ddev.cap_mask);
	
	
	/* Demanded by dmaengine interface: */ 
	sd->ddev.device_tx_status = s805_dma_tx_status;
	sd->ddev.device_issue_pending = s805_dma_issue_pending;
	sd->ddev.device_control = s805_dma_control;
	sd->ddev.device_alloc_chan_resources = s805_dma_alloc_chan_resources;
	sd->ddev.device_free_chan_resources = s805_dma_free_chan_resources;
	
	/* Capabilities: */
	sd->ddev.device_prep_slave_sg = s805_dma_prep_slave_sg;
	sd->ddev.device_prep_interleaved_dma = s805_dma_prep_interleaved;
	sd->ddev.device_prep_dma_cyclic = s805_dma_prep_dma_cyclic;
	sd->ddev.device_prep_dma_sg = s805_dma_prep_sg;
	sd->ddev.device_prep_dma_memcpy = s805_dma_prep_memcpy;
	sd->ddev.device_prep_dma_memset = s805_dma_prep_memset;
	sd->ddev.device_prep_dma_interrupt = s805_dma_prep_interrupt;
	
	/* Init internal structs */
	sd->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&sd->ddev.channels);
	
	platform_set_drvdata(pdev, sd);
	
    dev_info(&pdev->dev, "Entering s805 DMA engine probe, chan available: %u, IRQ: %u\n", mgr->chan_available, S805_DMA_IRQ);
	
	for (i = 0; i < mgr->chan_available; i++) {
		
	    if (s805_dma_chan_init(sd))
			goto err_no_dma;
		
	}
	
	dev_dbg(&pdev->dev, "Initialized %i DMA channels\n", i);
	
	ret = dma_async_device_register(&sd->ddev);
	if (ret) {
		dev_err(&pdev->dev,
				"Failed to register slave DMA engine device: %d\n", ret);
		
		goto err_no_dma;
	}
	
	s805_dma_enable_hw();
	dev_info(&pdev->dev, "Loaded S805 DMAC driver\n");
	
	return 0;
	
 err_no_dma:
	
	dev_err(&pdev->dev, "No DMA available.\n");
	s805_dma_free(sd);
	
	return ret;
}

static int s805_dma_remove(struct platform_device *pdev)
{
   struct s805_dmadev *sd = platform_get_drvdata(pdev);

   dma_async_device_unregister(&sd->ddev);
   s805_dma_free(sd);

   return 0;
}

static struct platform_driver s805_dma_driver = {
   .probe	= s805_dma_probe,
   .remove	= s805_dma_remove,
   .driver = {
	   .name = "s805-dmaengine",
	   .owner = THIS_MODULE,
	   .of_match_table = of_match_ptr(s805_dma_of_match),
   },
};

static int s805_init(void)
{
   return platform_driver_register(&s805_dma_driver);
}

static void s805_exit(void)
{
   platform_driver_unregister(&s805_dma_driver);
}

/*
* Load after serial driver (arch_initcall) so we see the messages if it fails,
* but before drivers (module_init) that need a DMA channel.
*/

subsys_initcall(s805_init);
module_exit(s805_exit);

MODULE_ALIAS("platform:s805-dmaengine");
MODULE_DESCRIPTION("Amlogic S805 dmaengine driver");
MODULE_AUTHOR("szz-dvl <such-a-mistake@gmail.com>");
MODULE_LICENSE("GPL v2");
