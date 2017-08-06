//#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/preempt.h>
#include <asm/mach/irq.h>

#include "s805_dmac.h"

#define S805_DMA_IRQ                     INT_NDMA
#define S805_DMA_ALIGN_SIZE              sizeof(unsigned long long)

#ifdef CONFIG_S805_DMAC_SERIALIZE
#define S805_DMA_MAX_THREAD              1
#else
#define S805_DMA_MAX_THREAD              S805_DMA_MAX_HW_THREAD
#endif

/* Registers & Bitmaps for the s805 DMAC */

#define S805_DMA_MAX_BURST               (0xFFFF - (S805_DMA_ALIGN_SIZE - 1))
#define S805_DMA_MAX_SKIP                (0xFFFF - (S805_DMA_ALIGN_SIZE - 1))
#define S805_MAX_TR_SIZE                 (0x1FFFFFF - (S805_DMA_ALIGN_SIZE - 1))

#define S805_DMA_DLST_STR0               P_NDMA_THREAD_TABLE_START0
#define S805_DMA_DLST_END0               P_NDMA_THREAD_TABLE_END0
#define S805_DMA_DLST_STR1               P_NDMA_THREAD_TABLE_START1
#define S805_DMA_DLST_END1               P_NDMA_THREAD_TABLE_END1
#define S805_DMA_DLST_STR2               P_NDMA_THREAD_TABLE_START2
#define S805_DMA_DLST_END2               P_NDMA_THREAD_TABLE_END2
#define S805_DMA_DLST_STR3               P_NDMA_THREAD_TABLE_START3
#define S805_DMA_DLST_END3               P_NDMA_THREAD_TABLE_END3

#define S805_DMA_BUSY                    BIT(26)

#define S805_DTBL_ADD_DESC               P_NDMA_TABLE_ADD_REG
#define S805_DMA_ADD_DESC(th, cnt)       (((th & 0x3) << 8) | (cnt & 0xff))

#define S805_DMA_THREAD_INIT(th)         (1 << (24 + th))
#define S805_DMA_THREAD_ENABLE(th)       (1 << (8 + th))

#define S805_DTBL_NO_BREAK               BIT(8) /* To be tested */

struct memset_val {

	unsigned long long val;

} __attribute__ ((aligned (32)));

struct memset_info {

    struct memset_val * value;
	dma_addr_t paddr;

};

/**
 * struct sg_info - Auxiliar structure to iterate sg lists.
 * 
 * @cursor: Current entry, will point to the sg entry beeing treated or %NULL 
 * if all the list has been treated.
 * 
 * @next: Next entry, will point the entry immediately consecutive to the one being treated, 
 * or %NULL if @cursor points to the last entry in the list.
 *
 * @bytes: Amount of bytes already consumed from the current entry being treated.
 *
 */
struct sg_info {

    struct scatterlist * cursor;
    struct scatterlist * next;
	
	uint bytes;
};

static inline struct s805_desc *to_s805_dma_desc(struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct s805_desc, vd.tx);
}

/**
 * struct s805_dmadev - General manager for the driver, will hold the lists of descriptors
 * among other fields nedded for the driver. More detailed information can be found in the 
 * file "s805_dmac.h" in the same directory that this file resides. 
 *
 */
#ifdef CONFIG_S805_DMAC_TO
struct s805_dmadev *mgr;
#else
static struct s805_dmadev *mgr;
#endif

static unsigned int dma_channels = 0;

static const struct of_device_id s805_dma_of_match[] = 
	{
		{ .compatible = "aml,amls805-dma", },
		{},
	};
MODULE_DEVICE_TABLE(of, s805_dma_of_match);

extern char __fiq_handler_str[], __fiq_handler_end[];

/**
 * add_zeroed_tdesc - This will set the control bit S805_DTBL_IRQ for the last 
 * chunk in the list, to ensure that the last descriptor will interrupt us, 
 * so we can properly handle the end of the transaction. After this a zeroed 
 * chunk will be added at the end of the current list, by way of padding. 
 *
 * @d: The descriptor to be closed.
 *  
 */
static s805_dtable * add_zeroed_tdesc (struct s805_desc * d)
{
	
	s805_dtable * desc_tbl = kzalloc(sizeof(s805_dtable), GFP_NOWAIT);

	/* Ensure that the last descriptor will interrupt us. */
	list_last_entry (&d->desc_list, s805_dtable, elem)->table->control |= S805_DTBL_IRQ;
	
	if (!desc_tbl) 
	    return NULL;
	
	desc_tbl->table = dma_pool_alloc(d->c->pool, GFP_NOWAIT, &desc_tbl->paddr); /* __GFP_ZERO: Not Working. */
	
	if (!desc_tbl->table) {
		
		kfree(desc_tbl);
		return NULL;
		
	} else
		*desc_tbl->table = (struct s805_table_desc) { 0 };

	list_add_tail(&desc_tbl->elem, &d->desc_list);

	return desc_tbl;
}

/**
 * def_init_new_tdesc - Auxiliar function to initialize data chunks. 
 * 
 * @c: Channel holding the pool that will store the data chunk.
 * @frames: Amount of frames already stored in the list.
 *
 */
static s805_dtable * def_init_new_tdesc (struct s805_chan * c, unsigned int frames)
{
	
	s805_dtable * desc_tbl = kzalloc(sizeof(s805_dtable), GFP_NOWAIT);
	
	if (!desc_tbl) 
	    return NULL;

	desc_tbl->table = dma_pool_alloc(c->pool, GFP_NOWAIT, &desc_tbl->paddr); /* __GFP_ZERO: Not Working. */
	
	if (!desc_tbl->table) {
		
		kfree(desc_tbl);
		return NULL;
		
	} else
		*desc_tbl->table = (struct s805_table_desc) { 0 };
	
	/* Control common part */
	desc_tbl->table->control |= S805_DTBL_PRE_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->control |= S805_DTBL_INLINE_TYPE(INLINE_NORMAL);
	
	/* desc_tbl->table->control |= S805_DTBL_NO_BREAK;                   
	   
	   Process the whole descriptor at once, without thread switching. 
	   
	   This needs to be carefully tested with this approach, if this bit is set the threads will be processed at once, 
	   without thread switching, what will make that the interrupts to be delivered more separated in time, however
	   if this bit is not set the work of the active threads will be, in some manner, balanced so if we see the
	   the 4 threads, with its possible active transactions, as a batch of transactions (what is actually what this
	   implementation tries) to not set this bit may be a benefit, specially if "in_progress" transactions differ
	   in size. In the other hand if "in_progress" transactions are similar in size interrupts may be delivered very
	   close in time, leading to hardware failures.
	   
	   As exposed above, to be tested. 
																		 
	*/

	if (!((frames + 1) % S805_DMA_MAX_DESC))
		desc_tbl->table->control |= S805_DTBL_IRQ;
		
	return desc_tbl;
	
}

/* Auxiliar functions for DMA_SG: */

/**
 * fwd_sg - Auxiliar fucntion meant to iterate sg lists, this will forward 
 * the cursor for the current entry.
 * 
 * @info: sg_info struct holding the pointers to the involved sg entries.
 *
 */
static inline void fwd_sg (struct sg_info * info) {

	if (info->cursor) {
		
		if (info->next) {

			info->cursor = info->next;
			info->next = sg_next(info->cursor);
		
		} else
			info->cursor = NULL;
	}
	
	info->bytes = 0;
			
}

/**
 * get_sg_icg - Will get the ICG (inter-chunk-gap) for the given sg_info struct.
 * 
 * @info: sg_info struct holding the pointers to the involved sg entries.
 *
 */
static uint get_sg_icg (struct sg_info * info) {

	/* 
	   Notice the integer return type, since we don't know if information will be arranged sequentially in memory. 
	   
	*/

	if (info->next)
		return sg_dma_address(info->next) - (sg_dma_address(info->cursor) + sg_dma_len(info->cursor));
	else
		return 0;
}

/**
 * get_sg_remain - Will get the remaining bytes for the current entry being treated,
 * the one pointed by @cursor.
 * 
 * @info: sg_info struct holding the pointers to the involved sg entries.
 *
 */
static uint get_sg_remain (struct sg_info * info) {
	
	if (info->cursor)
		return sg_dma_len(info->cursor) - info->bytes;
	else
		return UINT_MAX; /* For convenience in s805_scatterwalk */
}

/**
 * sg_ent_complete - Will return %true if the current entry, the one pointed 
 * by @cursor is complete, it is, if all the bytes in the current sg_entry are 
 * consumed, otherwise %false will be returned.
 * 
 * @info: sg_info struct holding the pointers to the involved sg entries.
 *
 */

static bool sg_ent_complete (struct sg_info * info) {

	if (info->cursor)
		return sg_dma_len(info->cursor) == info->bytes;
	else
		return false;
}

/**
 * sg_move_along - This function will be used by those iterating sg lists, however,
 * no struct sg_info is involved, so what this function will do is store the current
 * data chunk, which will hold information from sg_lists, in the given descriptor and
 * return a new empty chunk.
 * 
 * @chunk: The data chunk to be stored in the descriptor list.
 * @d: The descriptor holding the target list.
 *
 */
static s805_dtable * sg_move_along (struct s805_desc *d, s805_dtable * chunk) {

	if (chunk) {
		
		list_add_tail(&chunk->elem, &d->desc_list);
		d->frames ++;

	}
	
	return def_init_new_tdesc(d->c, d->frames);
	
}

/**
 * sg_init_desc - This function is meant to abstract the process of chunk initialisation 
 * for s805_scatterwalk(), depending on the type stored in @init_info a different new empty
 * chunk will be returned. Needed by crypto drivers to initialise its particular data chunks.
 * Notice that for each possible type, those defined in "linux/s805_dmac.h", it will be a
 * public function available, prototypes for those will be found in the same file.
 *
 * @d: Descriptor holding the list of chunks.
 * @chunk: The data chunk to be stored in the descriptor list, may be %NULL.
 * @init_nfo: Struct holding the needed information for a particular chunck initialisation,
 * may be %NULL.
 *
 */
static s805_dtable * sg_init_desc (struct s805_desc *d, s805_dtable * chunk) {

	switch(s805_desc_get_type(d)) {
#ifdef CONFIG_CRYPTO_DEV_S805_AES
	case AES_DESC:
		return sg_aes_move_along (d, chunk);
#endif
#ifdef CONFIG_CRYPTO_DEV_S805_TDES
	case TDES_DESC:
		return sg_tdes_move_along (d, chunk);
#endif
#ifdef CONFIG_CRYPTO_DEV_S805_CRC
	case CRC_DESC:
		return sg_crc_move_along (d, chunk);
#endif
#ifdef CONFIG_CRYPTO_DEV_S805_DIVX
	case DIVX_DESC:
		return sg_divx_move_along (d, chunk);
#endif
	default:
		return sg_move_along (d, chunk);
	}
}

/* Public functions, for crypto modules */

/* static void s805_dma_desc_free(struct virt_dma_desc *vd); */

/* /\** */
/*  * s805_desc_early_free - Public funtion offered to crypto modules through "linux/s805_dmac.h",  */
/*  * meant to "early" free a failed descriptor. */
/*  * */
/*  * @tx_desc: The descriptor to be closed. */
/*  * */
/*  *\/ */
/* void s805_desc_early_free (struct dma_async_tx_descriptor * tx_desc) { */

/*     s805_dma_desc_free(&to_s805_dma_desc(tx_desc)->vd); */
/* } */


/**
 * s805_crypto_set_req - Public funtion offered to crypto modules through "linux/s805_dmac.h", 
 * for crypto decriptors to hold all the information needed.
 *
 * @tx_desc: The descriptor to be closed.
 *
 */
void s805_crypto_set_req (struct dma_async_tx_descriptor * tx_desc, void * req) {

	to_s805_dma_desc(tx_desc)->req = req;
}

/**
 * s805_close_desc - Public funtion offered to crypto modules through "linux/s805_dmac.h", 
 * meant to close an already settled up descriptor. Will return NULL if the operation failed,
 * a valid pointer to the last padding block otherwise.
 *
 * @tx_desc: The descriptor to be closed.
 *
 */
bool s805_close_desc (struct dma_async_tx_descriptor * tx_desc) {

	return add_zeroed_tdesc(to_s805_dma_desc(tx_desc));
}

/**
 * s805_scatterwalk - This function will "translate" sg lists into a list of data chunks suitable 
 * for the hardware and return the associated descriptor. This function is made available to crypto
 * modules in the file "linux/s805_dmac.h".
 *
 * @src_sg: Source sg list.
 * @dst_sg: Destination sg list.
 * @init_nfo: Struct holding the needed information for a particular chunck initialisation.
 * @tx_desc: Already initialised descriptor, usefull to add information to a descriptor as many 
 * times as needed.
 * @last: Boolean value indicating if it will be the last time the descriptor pointed by @tx_desc
 * will be passed to this function, it is, if we must close the descriptor.
 *
 */
struct dma_async_tx_descriptor * s805_scatterwalk (struct scatterlist * src_sg,
												   struct scatterlist * dst_sg,
												   struct dma_async_tx_descriptor * tx_desc,
												   uint limit,
												   bool last)
{
	struct s805_desc *d;
	struct sg_info src_info, dst_info;
	s805_dtable * temp, * desc_tbl;
	unsigned int src_len, dst_len;
	dma_addr_t src_addr, dst_addr;
	int next_burst;
	bool new_block, src_completed;
	uint act_size, icg, burst, min_size;
		
	d = to_s805_dma_desc(tx_desc);
	
	if (s805_desc_is_crypto(d))
		limit -= d->byte_count;
	
	spin_lock(&d->c->vc.lock);
	
	desc_tbl = sg_init_desc (d, NULL);
	
	/* Auxiliar struct to iterate the lists. */
	src_info.cursor = src_sg;
	dst_info.cursor = dst_sg;
	
	src_info.next = src_info.cursor ? sg_next(src_info.cursor) : NULL;
	dst_info.next = dst_info.cursor ? sg_next(dst_info.cursor) : NULL;
	
	src_info.bytes = 0;
	dst_info.bytes = 0;
	
	desc_tbl->table->src = src_addr = src_info.cursor ? sg_dma_address(src_info.cursor) : 0;
	dst_addr = dst_info.cursor ? sg_dma_address(dst_info.cursor) : 0;

	if (!desc_tbl->table->dst)
		desc_tbl->table->dst = dst_addr;
	
	/* "Fwd logic" ¿not optimal?, must do. */
	while ((src_info.cursor || dst_info.cursor) && limit) {
		
		src_len = get_sg_remain(&src_info);
		dst_len = get_sg_remain(&dst_info);
		
	    min_size = min(min(dst_len, src_len), limit);
		
	    while (min_size) {
			
			act_size = min(min_size, (uint) S805_MAX_TR_SIZE);
			
			if ((desc_tbl->table->count + act_size) > S805_MAX_TR_SIZE) {
				
				/* Be careful!! may break multiplicity of blocks, to be tested. (MAX values protecting us?) */
				desc_tbl = sg_init_desc (d, desc_tbl);
				
				if (!desc_tbl)
					goto error_allocation;
				
				desc_tbl->table->src = src_addr + (dma_addr_t) src_info.bytes;
				
				/* DivX will have RK_FIFO address already set. */
				if (!desc_tbl->table->dst)
					desc_tbl->table->dst = dst_addr + (dma_addr_t) dst_info.bytes;		
			}
			
		    desc_tbl->table->count += act_size;
			
			src_info.bytes += act_size;
			dst_info.bytes += act_size;
			min_size -= act_size;

			if (s805_desc_is_crypto(d)) {
				
				d->byte_count += act_size;
				limit -= act_size;
			}
		}
		
		/* Either src entry or dst entry or both are complete here.  */
		
		new_block = true;
	    src_completed = false;
		
		if (sg_ent_complete(&src_info)) {
			
			src_completed = true;
			icg = get_sg_icg(&src_info);
			burst = src_info.bytes;
			next_burst = src_info.next ? sg_dma_len(src_info.next) : -1;
			fwd_sg(&src_info);
			
			/* ICG will be cleared if no burst present.*/
			if (!desc_tbl->table->src_burst) {
				
				if (burst <= S805_DMA_MAX_BURST && burst == desc_tbl->table->count
					&& burst == next_burst) {
					
					if (icg <= S805_DMA_MAX_SKIP) {
						
						desc_tbl->table->src_burst = burst; 
						desc_tbl->table->src_skip = icg;
						new_block = false;
						
					} 
					
				} else if (desc_tbl->table->dst_burst == 0 && icg == 0) /* Contiguous in memory, 1D case. */
					new_block = false;
				
			} else if ((desc_tbl->table->src_burst == next_burst || next_burst < 0) && desc_tbl->table->src_skip == icg) 
				new_block = false;
			
			src_addr = src_info.cursor ? sg_dma_address(src_info.cursor) : 0;
		}
		
		if ( sg_ent_complete(&dst_info) && (!src_completed || (src_completed && !new_block)) ) {
			
			icg = get_sg_icg(&dst_info);
			burst = dst_info.bytes;
			next_burst = dst_info.next ? sg_dma_len(dst_info.next) : -1;
			fwd_sg(&dst_info);
			
			if (!desc_tbl->table->dst_burst) {
				
				if (burst <= S805_DMA_MAX_BURST && burst == desc_tbl->table->count
					&& burst == next_burst) {
					
					if (icg <= S805_DMA_MAX_SKIP) {
						
						desc_tbl->table->dst_burst = burst;
						desc_tbl->table->dst_skip = icg;
						new_block = false;
						
					} else 
						new_block = true;
				    
				} else if (desc_tbl->table->src_burst == 0 && icg == 0) /* Contiguous in memory, 1D case. */
					new_block = false;
				else
					new_block = true;
				
			} else if ((desc_tbl->table->dst_burst == next_burst || next_burst < 0) && desc_tbl->table->dst_skip == icg) 
				new_block = false;
			else
				new_block = true;
			
			dst_addr = dst_info.cursor ? sg_dma_address(dst_info.cursor) : 0;
			
		} else if (sg_ent_complete(&dst_info)) {
			
			/* Both entries complete, src demands a new block. */
			
			fwd_sg(&dst_info); 
			dst_addr = dst_info.cursor ? sg_dma_address(dst_info.cursor) : 0;
			
	    }
		
		new_block = new_block && ((dst_info.cursor || src_info.cursor) && limit);
		
		if (new_block) {
			
			desc_tbl = sg_init_desc (d, desc_tbl);
			
			if (!desc_tbl)
				goto error_allocation;
			
			desc_tbl->table->src = src_addr + (dma_addr_t)src_info.bytes;

			/* DivX will have RK_FIFO address already set. */
			if (!desc_tbl->table->dst)
				desc_tbl->table->dst = dst_addr + (dma_addr_t)dst_info.bytes;
			
		} else if ((!dst_info.cursor && !src_info.cursor) || !limit) {
			
			list_add_tail(&desc_tbl->elem, &d->desc_list);
		    d->frames ++;	
		}
	}
	
	if (last) {
		
		if(!add_zeroed_tdesc(d))
			goto error_allocation;
		
	}

	spin_unlock(&d->c->vc.lock);
	
	return tx_desc;
	
 error_allocation:
	
	dev_err(d->c->vc.chan.device->dev, "%s: Error allocating descriptors.", __func__);
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(d->c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
	}

	spin_unlock(&d->c->vc.lock);
	
	kfree(d);
	
	return NULL;
}

/* END of Public functions, for crypto modules */
/* END of Auxiliar functions for DMA_SG */


/**
 * s805_dma_prep_slave_sg - Endpoint function for device_prep_slave_sg() (dmaengine interface).
 * Provides DMA_SLAVE capability. 
 *
 * @args: Argument documentation can be found in "linux/dmaengine.h".
 *
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
	struct sg_info info;
	s805_dtable * desc_tbl, * temp;
	uint size, act_size, icg, next_icg;
	int next_burst;
	dma_addr_t addr;
	u16 * my_burst, * my_skip;
	bool new_block;

	
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
	if (dev_width != DMA_SLAVE_BUSWIDTH_8_BYTES) 
		dev_warn(chan->device->dev, "%s: Unsupported buswidth: %i, only 8 bytes buswidth supported.\n", __func__, dev_width);
	
	/* Allocate and setup the descriptor. */
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
	
	info.cursor = sgl;
	info.next = sg_next(info.cursor);
	info.bytes = 0;

	spin_lock(&d->c->vc.lock);
	
	desc_tbl = def_init_new_tdesc(c, d->frames);

	addr = sg_dma_address(info.cursor);

	switch(direction) {
	case DMA_DEV_TO_MEM:
		{
			desc_tbl->table->control |= S805_DTBL_SRC_HOLD;
			desc_tbl->table->src = dev_addr;
			desc_tbl->table->dst = addr;
		}
		break;;
		
	case DMA_MEM_TO_DEV:
		{
			desc_tbl->table->control |= S805_DTBL_DST_HOLD;
			desc_tbl->table->src = addr;
			desc_tbl->table->dst = dev_addr;
		}
		break;;    
		
	case DMA_DEV_TO_DEV:
		{
			desc_tbl->table->control |= (S805_DTBL_SRC_HOLD | S805_DTBL_DST_HOLD);
			desc_tbl->table->src = c->cfg.src_addr;
			desc_tbl->table->dst = c->cfg.dst_addr;
		}
		break;;
					
	default:
		goto error_list;
		
	}
	
	while (info.cursor) {

	    size = get_sg_remain(&info);

		if (!IS_ALIGNED(size, S805_DMA_ALIGN_SIZE)) {

			dev_err(chan->device->dev, "%s: Unaligned size: %u.\n", __func__, size);
			goto error_list;
		}
		
	    while (size > 0) {
			
			act_size = size < S805_MAX_TR_SIZE ? size : S805_MAX_TR_SIZE;
			
			if ((desc_tbl->table->count + act_size) > S805_MAX_TR_SIZE) {

				desc_tbl = sg_move_along(d, desc_tbl);
					
				if (!desc_tbl)
					goto error_list;

				/* Setup addresses */
				switch(direction) {
				case DMA_DEV_TO_MEM:
					{
						desc_tbl->table->control |= S805_DTBL_SRC_HOLD;
						desc_tbl->table->src = dev_addr;
						desc_tbl->table->dst = addr + (dma_addr_t) info.bytes;
					}
					break;;
				
				case DMA_MEM_TO_DEV:
					{
						desc_tbl->table->control |= S805_DTBL_DST_HOLD;
						desc_tbl->table->src = addr + (dma_addr_t) info.bytes;
						desc_tbl->table->dst = dev_addr;
					}
					break;;    
					
				case DMA_DEV_TO_DEV:
					{
						desc_tbl->table->control |= (S805_DTBL_SRC_HOLD | S805_DTBL_DST_HOLD);
						desc_tbl->table->src = c->cfg.src_addr;
						desc_tbl->table->dst = c->cfg.dst_addr;
					}
					break;;
					
				default:
					goto error_list;
					
				}		
			}
			
		    desc_tbl->table->count += act_size;
			info.bytes += act_size;
	
		    size -= act_size;
		}
		
		/* Completed sg entry here. */
			
		new_block = true;
		
		if (direction != DMA_DEV_TO_DEV) {
			
			switch(direction) {
			case DMA_DEV_TO_MEM:
				{
					my_burst = &desc_tbl->table->dst_burst;
					my_skip = &desc_tbl->table->dst_skip;
				}
				break;;
				
			case DMA_MEM_TO_DEV:
				{
					my_burst = &desc_tbl->table->src_burst;
					my_skip = &desc_tbl->table->src_skip;
				}
				break;;    

			default:
				goto error_list;
				
			}

			icg = get_sg_icg(&info);
			next_burst = info.next ? sg_dma_len(info.next) : -1;
			fwd_sg(&info);
			next_icg = get_sg_icg(&info);
			
			if (*my_burst == 0) {

				if (next_burst == desc_tbl->table->count &&
					desc_tbl->table->count <= S805_DMA_MAX_BURST) {
					
					if (icg >= 0 && icg <= S805_DMA_MAX_SKIP && (icg == next_icg || !info.next)) {
						
						*my_burst = desc_tbl->table->count; 
						*my_skip = icg;
						new_block = false;	
						
					} 
				}
				
			} else if (*my_burst == next_burst && (*my_skip == next_icg || !info.next)) 
				new_block = false;
			
			addr = info.cursor ? sg_dma_address(info.cursor) : 0; /* Already forwarded. */
			
		} else {

			/* If a new block is needed will be allocated in the above loop for DMA_DEV_TO_DEV */
			
			fwd_sg(&info);
			new_block = false; 
		}
		
		new_block = new_block && info.cursor;
		
		if (new_block) {

			desc_tbl = sg_move_along(d, desc_tbl);
			
			if (!desc_tbl)
				goto error_list;
			
			/* Setup addresses */
			switch(direction) {
			case DMA_DEV_TO_MEM:
				{
					desc_tbl->table->control |= S805_DTBL_SRC_HOLD;
					desc_tbl->table->src = dev_addr;
					desc_tbl->table->dst = addr;
				}
				break;;
				
			case DMA_MEM_TO_DEV:
				{
					desc_tbl->table->control |= S805_DTBL_DST_HOLD;
					desc_tbl->table->src = addr;
					desc_tbl->table->dst = dev_addr;
				}
				break;;
				
			default:
				goto error_list;
				
			}
			
		} else if (!info.cursor) {

			list_add_tail(&desc_tbl->elem, &d->desc_list);
			d->frames ++;
			
		}	
	}
		
	if (!add_zeroed_tdesc(d))
		goto error_list;

	spin_unlock(&d->c->vc.lock);
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);
	
 error_list:

	dev_err(chan->device->dev, "Error allocating descriptors.");
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}

	spin_unlock(&d->c->vc.lock);
	
	kfree(d);
	return NULL; 
}

/**
 * ileaved_def_init_new_tdesc - Auxiliar function to initialise descriptors (interleaved).
 * 
 * @c: Channel holding the pool that will deliver the new empty chunk.
 * @xt: struct dma_interleaved_template with the configuration provided.
 * @burst_and_skip: Countage of bytes spent till the moment, both data bytes and skip or gap bytes. 
 * @count: Countage of bytes spent till the moment, only data bytes, gaps not added.
 * @frames: Number of frames or chunks spent till the moment.
 *
 */
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


/**
 * s805_dma_prep_interleaved - Endpoint function for device_prep_interleaved_dma() (dmaengine interface).
 * Provides DMA_INTERLEAVE capability. 
 *
 * @args: Argument documentation can be found in "linux/dmaengine.h".
 *
 * Note: Documentation is a little bit confusing for "device_prep_interleaved_dma", in the following lines of "linux/dmaengine.h" one can read:
 *
 * 120 -> struct data_chunk - Element of scatter-gather list that makes a frame.
 *
 * [ . . . ]
 *
 * 134 -> struct dma_interleaved_template - Template to convey DMAC the transfer pattern and attributes.
 * 
 * [ . . . ]
 *
 * 147 -> @numf: Number of frames in this template.
 * 148 -> @frame_size: Number of chunks in a frame i.e, size of sgl[].
 *
 * The concept of frame is not very concrete, so the driver is using "@xt->frame_size" as the reference value to iterate the chunks stored in "@xt->sgl", 
 * hence, "@xt->numf" is ignored for this implementation. To support more than one frame per transaction the interface will need to be modified
 * to accept an array of "dma_interleaved_template" structs as a parameter. In the following link a patch implementing this idea is provided:
 *
 *      (*) http://lists.infradead.org/pipermail/linux-arm-kernel/2014-February/233185.html
 *
 */

static struct dma_async_tx_descriptor * 
s805_dma_prep_interleaved (struct dma_chan *chan, 
						   struct dma_interleaved_template *xt, 
						   unsigned long flags) 
{
	
	struct s805_chan *c = to_s805_dma_chan(chan);
	struct s805_desc *d;
	struct s805_table_desc *table;
	s805_dtable *desc_tbl, *temp;
	int idx, count, byte_cnt = 0, act_size = 0;
	long long tmp_size; /* to support up to UINT_MAX sized data_chunks */
	bool new_block = false;
	
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
	
	if ((!xt->src_inc && (xt->src_sgl || (xt->dir != DMA_DEV_TO_MEM && xt->dir != DMA_DEV_TO_DEV))) || 
		(!xt->dst_inc && (xt->dst_sgl || (xt->dir != DMA_MEM_TO_DEV && xt->dir != DMA_DEV_TO_DEV))) ||
		((!xt->dst_inc && !xt->src_inc) && xt->dir != DMA_DEV_TO_DEV) ||
		((xt->dst_inc  && xt->src_inc)  && xt->dir != DMA_MEM_TO_MEM)) {
		
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
	
	count = 0;
	byte_cnt = 0;

	spin_lock(&d->c->vc.lock);
	
	desc_tbl = ileaved_def_init_new_tdesc(c, xt, count, byte_cnt, d->frames);
	
	if (!desc_tbl) {
		
		kfree(d);
		return NULL;
		
	} else
		table = desc_tbl->table;
	
	for (idx = 0; idx < xt->frame_size; idx++) {
		
		//dev_dbg(c->vc.chan.device->dev, "Adding data chunk %d: \n\tsize = %d\n\ticg = %d\n", idx, xt->sgl[idx].size, xt->sgl[idx].icg);
			
		/* 
		 * 2D move:
		 *
		 * It is unsupported for the current kernel (3.10.y) to distinguish between the lengths of the skip and burts for the src and dst.
		 *
		 */
		
		if (xt->dst_sgl || xt->src_sgl) {
			
			if (!IS_ALIGNED(xt->sgl[idx].size, S805_DMA_ALIGN_SIZE)) {
				
				dev_err(chan->device->dev, "%s: Unaligned size: %u.\n", __func__, xt->sgl[idx].size);
				goto error_allocation;
			}

			if (new_block) {
				
				list_add_tail(&desc_tbl->elem, &d->desc_list);
				
				desc_tbl = ileaved_def_init_new_tdesc(c, xt, count, byte_cnt, d->frames);
				
				if (!desc_tbl)
					goto error_allocation;
				else
					table = desc_tbl->table;
				
				d->frames++;
			}
			
			for (tmp_size = xt->sgl[idx].size; tmp_size > 0; tmp_size -= S805_MAX_TR_SIZE) {

				act_size = min(tmp_size, (long long)S805_MAX_TR_SIZE);
				
				if ( (table->count + act_size) >= S805_MAX_TR_SIZE ) {
					
					list_add_tail(&desc_tbl->elem, &d->desc_list);
					
					desc_tbl = ileaved_def_init_new_tdesc(c, xt, count, byte_cnt, d->frames);
					
					if (!desc_tbl)
						goto error_allocation;
					else
						table = desc_tbl->table;
					
					d->frames++;
				}
				
				table->count += act_size;		
				count += act_size;
				byte_cnt += act_size;	
			}

			/* 
			   Notice that the following statement will take into account ICG sizes bigger than S805_DMA_MAX_SKIP, 
			   so if an ICG bigger than the supported by the hardware is demanded a new block will be allocated
			   with the addresses offsetted as demanded. 
			   
			*/

			count += xt->sgl[idx].icg;
			new_block = true;
			
			if (xt->sgl[idx].size <= S805_DMA_MAX_BURST && xt->sgl[idx].icg <= S805_DMA_MAX_SKIP) {
				
				if (xt->src_sgl && (table->src_skip == xt->sgl[idx].icg || (!table->src_skip && table->count == xt->sgl[idx].size))
					&& (table->src_burst == xt->sgl[idx].size || (!table->src_burst && table->count == xt->sgl[idx].size)) ) { 
					
					table->src_burst = xt->sgl[idx].size;	
					table->src_skip = xt->sgl[idx].icg;
					
					new_block = false;
					
				}
				
				if (xt->dst_sgl && (table->dst_skip == xt->sgl[idx].icg || (!table->dst_skip && table->count == xt->sgl[idx].size))
					&& (table->dst_burst == xt->sgl[idx].size || (!table->dst_burst && table->count == xt->sgl[idx].size)) ) {  
					
					table->dst_burst = xt->sgl[idx].size;	
					table->dst_skip = xt->sgl[idx].icg;
					
					new_block = false;
					
				}
				
			} else if (!table->src_burst && !table->dst_burst && xt->sgl[idx].icg == 0)
				new_block = false;
			
			
		} else { /* 1D move */
			
			for (tmp_size = xt->sgl[idx].size; tmp_size > 0; tmp_size -= S805_MAX_TR_SIZE) {
				
				act_size = min(tmp_size, (long long)S805_MAX_TR_SIZE);
				
				if ((table->count + act_size) > S805_MAX_TR_SIZE) {
					
					list_add_tail(&desc_tbl->elem, &d->desc_list);
					
					desc_tbl = ileaved_def_init_new_tdesc(c, xt, count, byte_cnt, d->frames);
					
					if (!desc_tbl)
						goto error_allocation;
					else
						table = desc_tbl->table;
					
					d->frames++;
				}
				
				table->count += act_size;
				count += act_size;
			}
		}
		
	} //idx

	list_add_tail(&desc_tbl->elem, &d->desc_list);
	d->frames ++;
	
	if (!add_zeroed_tdesc(d))
		goto error_allocation;

	spin_unlock(&d->c->vc.lock);
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);
	
 error_allocation:
	
	dev_err(chan->device->dev, "Error allocating descriptors.");
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}

	spin_unlock(&d->c->vc.lock);
	
	kfree(d);
	return NULL;
}

/**
 * cyclic_def_init_new_tdesc - Auxiliar function to initialise descriptors (cyclic).
 * 
 * @src_addr: Source dma address.
 * @dst_addr: Destination dma address.
 * @direction: Direction of the transaction.
 * @byte_count: Countage of bytes spent till the moment.
 * @period_count: Countage of period bytes spent till the moment, reinitialised for each period, 
 * only usefull for direction DMA_MEM_TO_MEM.
 * @addr_reset: Boolean value indicating how adresses must be arranged, only usefull for 
 * direction DMA_MEM_TO_MEM.
 * @frames: Number of frames or chunks spent till the moment.
 *
 */
static s805_dtable * cyclic_def_init_new_tdesc (struct s805_chan *c,
												dma_addr_t src_addr,
												dma_addr_t dst_addr,
												enum dma_transfer_direction direction,
												uint byte_count,
												uint period_count,
												bool addr_reset,
												uint frames) {
	
	s805_dtable *desc_tbl = def_init_new_tdesc(c, frames);
	
	if (!desc_tbl) 
	    return NULL;

	switch (direction) {
	case DMA_DEV_TO_MEM:
		{
			desc_tbl->table->control |= S805_DTBL_SRC_HOLD;
			desc_tbl->table->src = src_addr;
			desc_tbl->table->dst = dst_addr + (dma_addr_t) byte_count;
		}
		break;;
	case DMA_MEM_TO_DEV:
		{
			desc_tbl->table->control |= S805_DTBL_DST_HOLD;
			desc_tbl->table->src = src_addr + (dma_addr_t) byte_count;
			desc_tbl->table->dst = dst_addr;
						
		}
		break;;
	case DMA_MEM_TO_MEM:
		{
			desc_tbl->table->src = src_addr + (dma_addr_t) (addr_reset ? byte_count : period_count);
			desc_tbl->table->dst = dst_addr + (dma_addr_t) (addr_reset ? period_count : byte_count);	
		}
		break;;
	case DMA_DEV_TO_DEV:
		{
			desc_tbl->table->control |= (S805_DTBL_SRC_HOLD | S805_DTBL_DST_HOLD);
			desc_tbl->table->src = src_addr;
			desc_tbl->table->dst = dst_addr;
		}		
		break;;
	default:
		return NULL;
	}

	return desc_tbl;
}

/**
 * s805_dma_prep_dma_cyclic - Endpoint function for device_prep_dma_cyclic() (dmaengine interface).
 * Provides DMA_CYCLIC capability. 
 *
 * @args: Argument documentation can be found in "linux/dmaengine.h".
 *
 */
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
	struct s805_desc *d, * root, * cursor;
	dma_addr_t dst_addr, src_addr;
	s805_dtable * desc_tbl, * temp;
	unsigned int i, j, periods, next_bytes, byte_count = 0, period_count = 0;
	bool addr_reset;
	
	/*
	  DMA_MEM_TO_MEM: This direction is implemented only for test purposes, it is actually a non use case .
	*/
	
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
			if (!c->cfg.dst_addr) {
				dst_addr = buf_addr;
				addr_reset = false;
				
				if (c->cfg.src_addr) {
					
					src_addr = c->cfg.src_addr; 
					dev_width = c->cfg.src_addr_width;
					
				} else {
					
					dev_err(chan->device->dev, "%s: Missing source address.", __func__);
					return NULL;
				}
			}

			if (!c->cfg.src_addr) {
				src_addr = buf_addr;
				addr_reset = true;
				
				if (c->cfg.dst_addr) {
					
					dst_addr = c->cfg.dst_addr;
					dev_width = c->cfg.dst_addr_width;
					
				} else {

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
	
	/* Datasheet p.57 entry 1 & 2 */
	if (dev_width != DMA_SLAVE_BUSWIDTH_8_BYTES) {
		
		dev_err(chan->device->dev, "%s: Unsupported buswidth: %i\n", __func__, dev_width);
		return NULL;
	}

	if (!IS_ALIGNED(period_len, S805_DMA_ALIGN_SIZE)) {

		dev_err(chan->device->dev, "%s: Unaligned period len: %u.\n", __func__, period_len);
		return NULL;
	}

	periods = DIV_ROUND_UP(buf_len, period_len);
	
	if (buf_len % period_len) {
		dev_err(chan->device->dev,
				"%s: buffer_length (%zd) is not a multiple of period_len (%zd).\n",
				__func__, buf_len, period_len);
		
		if ((periods * period_len) > buf_len)
			return NULL;
	}	

	/* Allocate and setup the root descriptor. */
	root = d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;

	d->c = c;
	d->frames = 0;
	INIT_LIST_HEAD(&d->desc_list);
	s805_dma_set_cyclic(d);
	
	cursor = root;

	spin_lock(&d->c->vc.lock);
	
	desc_tbl = cyclic_def_init_new_tdesc(c, src_addr, dst_addr, direction, byte_count, period_count, addr_reset, d->frames);
	
	if (!desc_tbl)  {
		
		kfree (d);
		return NULL;
		
	}
			
	for (i = 0; i < periods; i++) {
		
		for (j = 0; j < period_len; j += S805_MAX_TR_SIZE) {
				
			next_bytes = min((period_len - j), (uint) S805_MAX_TR_SIZE);
				
			if ((desc_tbl->table->count + next_bytes) > S805_MAX_TR_SIZE) {
					
				list_add_tail(&desc_tbl->elem, &d->desc_list);
				d->frames ++;
					
				desc_tbl = cyclic_def_init_new_tdesc(c, src_addr, dst_addr, direction, byte_count, period_count, addr_reset, d->frames);
					
				if (!desc_tbl) 
					goto error_list;
					
			}
				
			desc_tbl->table->count += next_bytes;
			byte_count += next_bytes;
			period_count += next_bytes;
		}

		/* Ensure that the last descriptor of the period will interrupt us. */
		desc_tbl->table->control |= S805_DTBL_IRQ;
 
		list_add_tail(&desc_tbl->elem, &d->desc_list);
		d->frames ++;
			
		if(!add_zeroed_tdesc(d))
			goto error_list;
		
		period_count = 0;
			
		if (i < periods - 1) {
				
			/* Allocate and setup the next descriptor. */
			d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
			if (!d)
				goto error_list;
			
			d->c = c;
			d->frames = 0;
			INIT_LIST_HEAD(&d->desc_list);
			s805_dma_set_cyclic(d);
			
			cursor->next = d;
			cursor->root = root; 
			
			cursor = cursor->next; /* Must be NULL in first instance, so "error_list" tag must be correct. */
				
			desc_tbl = cyclic_def_init_new_tdesc(c, src_addr, dst_addr, direction, byte_count, period_count, addr_reset, d->frames);
				
			if (!desc_tbl) 
				goto error_list;
		} else
			cursor->root = cursor->next = root; /* Close the descriptor chain. */	
	}
	
	spin_unlock(&d->c->vc.lock);
	
	return vchan_tx_prep(&c->vc, &root->vd, flags);
	
 error_list:

	dev_err(chan->device->dev, "%s: Error allocating descriptors.", __func__);
	
	cursor = root;
	
	while (cursor) {
		
		list_for_each_entry_safe (desc_tbl, temp, &cursor->desc_list, elem) {
			
			dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
			list_del(&desc_tbl->elem);
			kfree(desc_tbl);
			
		}
	
		kfree(cursor);
		cursor = cursor->next;
	}

	spin_unlock(&d->c->vc.lock);
	
	return NULL;
}

/**
 * s805_dma_prep_sg - Endpoint function for device_prep_dma_sg() (dmaengine interface).
 * Provides DMA_SG capability. 
 *
 * @args: Argument documentation can be found in "linux/dmaengine.h".
 *
 */
static struct dma_async_tx_descriptor *
s805_dma_prep_sg (struct dma_chan *chan,
				  struct scatterlist *dst_sg, unsigned int dst_nents,
				  struct scatterlist *src_sg, unsigned int src_nents,
				  unsigned long flags)
{
	struct s805_desc *d;
	struct s805_chan *c = to_s805_dma_chan(chan);
	struct scatterlist *aux;
	int j, bytes = 0;
	uint len;

	for_each_sg(src_sg, aux, src_nents, j) {

		len = sg_dma_len(aux);
		
		if (!IS_ALIGNED(len, S805_DMA_ALIGN_SIZE)) {

			dev_err(chan->device->dev, "%s: Unaligned size: %u.\n", __func__, len);
		    return NULL;
		}
		
		bytes += len;
	}
	
	for_each_sg(dst_sg, aux, dst_nents, j)  {

		len = sg_dma_len(aux);
		
		if (!IS_ALIGNED(len, S805_DMA_ALIGN_SIZE)) {

			dev_err(chan->device->dev, "%s: Unaligned size: %u.\n", __func__, len);
		    return NULL;
		}
		
		bytes -= len;
	}
	
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
	
    return s805_scatterwalk (src_sg, dst_sg, vchan_tx_prep(&c->vc, &d->vd, flags), UINT_MAX, true);
}

/**
 * s805_dma_prep_memcpy - Endpoint function for device_prep_dma_memcpy() (dmaengine interface).
 * Provides DMA_MEMCPY capability. 
 *
 * @args: Argument documentation can be found in "linux/dmaengine.h".
 *
 */
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
	unsigned int act_size, bytes = 0;
	long long tmp_size;

	if (!IS_ALIGNED(len, S805_DMA_ALIGN_SIZE)) {
		
		dev_err(chan->device->dev, "%s: Unaligned size: %u.\n", __func__, len);
		return NULL;
	}
	
	/* Allocate and setup the descriptor. */
	d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;
	
	d->c = c;
	d->frames = 0;
	INIT_LIST_HEAD(&d->desc_list);

	spin_lock(&d->c->vc.lock);
	desc_tbl = def_init_new_tdesc(c, d->frames);

	if (!desc_tbl) {
		kfree (d);
		return NULL;
	}
	
	desc_tbl->table->src = src;
	desc_tbl->table->dst = dest;
	
	for (tmp_size = len; tmp_size > 0; tmp_size -= S805_MAX_TR_SIZE) {

		act_size = tmp_size > S805_MAX_TR_SIZE ? S805_MAX_TR_SIZE : tmp_size;
		
		if ((desc_tbl->table->count + act_size) > S805_MAX_TR_SIZE) {
			
			list_add_tail(&desc_tbl->elem, &d->desc_list);
			d->frames ++;
			
			desc_tbl = def_init_new_tdesc(c, d->frames);
			
			if (!desc_tbl)
				goto error_allocation;
			
			desc_tbl->table->src = src + (dma_addr_t) bytes;
			desc_tbl->table->dst = dest + (dma_addr_t) bytes;
		}

		desc_tbl->table->count += act_size;
		bytes += act_size;		
	}

	list_add_tail(&desc_tbl->elem, &d->desc_list);
	d->frames ++;
				
	if(!add_zeroed_tdesc(d))
		goto error_allocation;

	spin_unlock(&d->c->vc.lock);
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);
	
 error_allocation:
	
	dev_err(chan->device->dev, "%s: Error allocating descriptors.", __func__);
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
	}

	spin_unlock(&d->c->vc.lock);
	
	kfree(d);
	return NULL;
}

/**
 * s805_dma_prep_memset - Endpoint function for device_prep_dma_memset() (dmaengine interface).
 * Provides DMA_MEMSET capability. 
 *
 * @args: Argument documentation can be found in "linux/dmaengine.h".
 *
 */
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
	unsigned int act_size, bytes = 0;
	long long tmp_size;

	if (!IS_ALIGNED(len, S805_DMA_ALIGN_SIZE)) {
		
		dev_err(chan->device->dev, "%s: Unaligned size: %u.\n", __func__, len);
		return NULL;
	}
	
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

	/* With a block from the pool we will have enough here, we will save almost an entire page. */
	d->memset->value = dma_pool_alloc(d->c->pool, GFP_NOWAIT, &d->memset->paddr);
	
	if (!d->memset->value) {
		
		kfree(d);
		kfree(d->memset);
		
		return NULL;
		
	} else
		d->memset->value->val = value;

	/*
	  
	  The following statements will concatenate the integer value (32 bits long) into a 
	  long long value (64 bits long), given that the hardware is only capable of moving
	  the data in chunks of 64 bits. This means that if "dest" address is pointing to an
	  array or a buffer of integers (positions of 32 bits long) the desired result will 
	  be achieved, however the buffer MUST be aligned to 8 Bytes to avoid writting 
	  undesired addresses. 
	  
	*/
	
	d->memset->value->val <<= (sizeof(int) * 8);
    d->memset->value->val |= (value & ~0U);

	spin_lock(&d->c->vc.lock);
	
	desc_tbl = def_init_new_tdesc(c, d->frames);

	if (!desc_tbl) {
		kfree (d);
		return NULL;
	}
	
	desc_tbl->table->src = d->memset->paddr;
	desc_tbl->table->dst = dest;
	desc_tbl->table->control |= S805_DTBL_SRC_HOLD;

	for (tmp_size = len; tmp_size > 0; tmp_size -= S805_MAX_TR_SIZE) {

		act_size = tmp_size > S805_MAX_TR_SIZE ? S805_MAX_TR_SIZE : tmp_size;
		
		if ((desc_tbl->table->count + act_size) > S805_MAX_TR_SIZE) {

			list_add_tail(&desc_tbl->elem, &d->desc_list);
			d->frames ++;
			
			desc_tbl = def_init_new_tdesc(c, d->frames);
			
			if (!desc_tbl)
				goto error_allocation;
			
			desc_tbl->table->src = d->memset->paddr;
			desc_tbl->table->dst = dest + (dma_addr_t) bytes;	
			desc_tbl->table->control |= S805_DTBL_SRC_HOLD;
			
		}

		desc_tbl->table->count += act_size;
		bytes += act_size;
	}

	list_add_tail(&desc_tbl->elem, &d->desc_list);
	d->frames ++;

	if(!add_zeroed_tdesc(d))
		goto error_allocation;

	spin_unlock(&d->c->vc.lock);
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);

 error_allocation:
	
	dev_err(chan->device->dev, "%s: Error allocating descriptors.", __func__);
	
	list_for_each_entry_safe (desc_tbl, temp, &d->desc_list, elem) {
		
		dma_pool_free(c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);
		
	}

	spin_unlock(&d->c->vc.lock);
	
	dma_free_coherent(chan->device->dev, sizeof(long long), d->memset->value, d->memset->paddr);

	kfree(d->memset);
	kfree(d);
	return NULL;
	
} 

/**
 * s805_dma_prep_interrupt - Endpoint function for device_prep_dma_interrupt() (dmaengine interface).
 * Provides DMA_ASYNC_TX and DMA_INTERRUPT capabilities. 
 *
 * @args: Argument documentation can be found in "linux/dmaengine.h".
 *
 */
struct dma_async_tx_descriptor *
s805_dma_prep_interrupt (struct dma_chan *chan,
						 unsigned long flags)
{

	struct s805_chan *c = to_s805_dma_chan(chan);
	struct s805_desc *d;
	
	/* Allocate and setup the descriptor. */
	d = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT);
	if (!d)
		return NULL;
	
	d->c = to_s805_dma_chan(chan);
	s805_dma_set_flags(d, flags);
	
	INIT_LIST_HEAD(&d->desc_list);
	
	return vchan_tx_prep(&c->vc, &d->vd, flags);
	

}

/**
 * s805_dma_thread_enable - Start the given thread. 
 *
 * @thread_id: Id of the thread to be enabled, between 0 and (%S805_DMA_MAX_HW_THREAD - 1).
 *
 */
static inline void s805_dma_thread_enable ( uint thread_id ) { 
	
	u32 reg_val;

	/* Ensure that the engine is running (token from crypto module) */
	reg_val = RD(S805_DMA_CTRL);
	WR(reg_val | S805_DMA_ENABLE, S805_DMA_CTRL);
	
    reg_val = RD(S805_DMA_THREAD_CTRL);
	WR(reg_val | S805_DMA_THREAD_ENABLE(thread_id), S805_DMA_THREAD_CTRL);
	
}

/**
 * s805_dma_thread_disable - Stop the given thread. 
 *
 * @thread_id: Id of the thread to be disabled, between 0 and (%S805_DMA_MAX_HW_THREAD - 1).
 *
 */
static inline void s805_dma_thread_disable ( uint thread_id ) { 
	
	u32 reg_val;
	
	reg_val = RD(S805_DMA_THREAD_CTRL);
	WR(reg_val & ~S805_DMA_THREAD_ENABLE(thread_id), S805_DMA_THREAD_CTRL);
}

/**
 * s805_dma_thread_disable - Write general CLK and DMA_CTRL register to enable engine. 
 *
 */
static inline void s805_dma_enable_hw ( void ) { 
	
	uint i;
	u32 status = RD(S805_DMA_CLK);
	
	/* Main clock */
	WR(status | S805_DMA_ENABLE, S805_DMA_CLK);
	
	status = RD(S805_DMA_CTRL);
	
	/* Autosuspend, future Kconfig option. */
	status &= ~S805_DMA_DMA_PM;
	status |= S805_DMA_ENABLE;
	
	WR(status, S805_DMA_CTRL);
	
	/* Fast IRQ */
	WR(S805_DMA_FIRQ_BIT, S805_DMA_FIRQ_SEL);
	
	/* Default thread slice (1 page) */
	WR(S805_DMA_SET_SLICE(S805_DMA_DEF_SLICE), S805_DMA_THREAD_CTRL);
	
	for (i = 0; i < S805_DMA_MAX_HW_THREAD; i++)
		s805_dma_thread_disable(i);
}

/**
 * s805_dma_desc_free - Will free the associated descriptor. Passed to virtual channels to free resources.
 *
 * @vd: Virtual descriptor associated to our descriptor.
 *
 */
static void s805_dma_desc_free(struct virt_dma_desc *vd)
{
	struct s805_desc * aux, *cursor, * me = to_s805_dma_desc(&vd->tx);
	struct s805_chan * c = me->c;
	s805_dtable * desc_tbl, * temp;	
	
	if (s805_desc_is_cyclic(me)) {
		
		cursor = me->next;

		while (cursor != me) {
				
			list_for_each_entry_safe (desc_tbl, temp, &cursor->desc_list, elem) {

				dma_pool_free(cursor->c->pool, desc_tbl->table, desc_tbl->paddr);
				list_del(&desc_tbl->elem);
				kfree(desc_tbl);
		
			}

			aux = cursor;
			cursor = cursor->next;
			kfree(aux);
		}
	}
		
	list_for_each_entry_safe (desc_tbl, temp, &me->desc_list, elem) {

		dma_pool_free(me->c->pool, desc_tbl->table, desc_tbl->paddr);
		list_del(&desc_tbl->elem);
		kfree(desc_tbl);	
	}
	
	if (me->memset) {
		
		dma_pool_free(me->c->pool, me->memset->value, me->memset->paddr);
		kfree(me->memset);
	}
	
	dev_dbg(c->vc.chan.device->dev, "Descriptor 0x%p: Freed.", &vd->tx);
    
	kfree(me);
	
	c->pending --;
	
	if (!c->pending)
		c->status = S805_DMA_SUCCESS;
}

/**
 * s805_dma_get_next_addr - Meant to be used for decriptors with more than %S805_DMA_MAX_DESC, 
 * to get the next chunk to be processed when the yet unprocessed %S805_DMA_MAX_DESC chunks finish, 
 * it is, the chunk @desc + %S805_DMA_MAX_DESC.
 *
 * @desc: First chunk of the batch.
 *
 */
static s805_dtable * s805_dma_get_next_addr (s805_dtable * desc) {

	s805_dtable * dtbl = desc;
	int i;

	/* Must never reach the end of the list. */
	for (i = 0; i < S805_DMA_MAX_DESC; i++)
		dtbl = list_next_entry(dtbl, elem);
	
	return dtbl;
}

/**
 * s805_dma_allocate_tr - Write s805 DMAC registers with start and end addresses of table descriptor list.
 * Will return the next chunk to be treated if there are morre than %S805_DMA_MAX_DESC chunks in the list,
 * otherwise %NULL will be returned.
 *
 * @thread_id: Id of the thread that will hold the transaction.
 * @desc: First descriptor of the transaction.
 * @frames: Amount of frames or chunks to be processed.
 *
 */
static s805_dtable * s805_dma_allocate_tr (uint thread_id, s805_dtable * desc, uint frames) 
{
	
    u32 status;
	u32 str_addr, end_addr;
	
	uint amount = min(frames, (uint) S805_DMA_MAX_DESC); 

	s805_dma_thread_disable(thread_id);
		
	str_addr = desc->paddr;
	end_addr = str_addr + (amount * sizeof(struct s805_table_desc));

	
	/* pr_debug("Allocating %u descriptors for thread %u: 0x%08X - 0x%08X (next = %s)\n", */
	/* 		 amount, */
	/* 		 thread_id, */
	/* 		 str_addr, */
	/* 		 end_addr, */
	/* 		 amount < frames ? "true" : "false"); */

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
	
	return amount < frames ? s805_dma_get_next_addr(desc) : NULL;
}

/**
 * s805_dma_schedule_tr - Schedule an issued transaction. Channel lock held by callers.
 *
 * @c: The channel holding the list of issued descriptors.
 *
 */
static void s805_dma_schedule_tr ( struct s805_chan * c ) {

	struct virt_dma_desc *vd, *tmp;
	struct s805_desc * d;
	
#ifdef DEBUG
	struct s805_desc * cursor;
	s805_dtable *desc;
	uint i = 0;
#endif
	
	list_for_each_entry_safe (vd, tmp, &c->vc.desc_issued, node) {

		list_del(&vd->node);
		d = to_s805_dma_desc(&vd->tx);
		
#ifdef DEBUG

		/* Cyclic transfer, always the root here. */
		cursor = d;

		while (cursor) {
					
			list_for_each_entry (desc, &cursor->desc_list, elem) {

				/* Last descriptors will be zeroed */
				if (!list_is_last(&desc->elem, &cursor->desc_list)) {
					
					dev_dbg(d->c->vc.chan.device->dev, "0x%p %03u (0x%08X): ctrl = 0x%08X, src = 0x%08X, dst = 0x%08X, byte_cnt = %08u, src_burst = %05u, src_skip = %05u, dst_burst = %05u, dst_skip = %05u, crypto = 0x%08X\n",
							&cursor->vd.tx,
							i,
							desc->paddr,
							desc->table->control, 
							desc->table->src, 
							desc->table->dst, 
							desc->table->count,
							desc->table->src_burst,
							desc->table->src_skip,
							desc->table->dst_burst,
							desc->table->dst_skip,
							desc->table->crypto);
					i++;
				}
			}	
			
			/* If NULL, either this is not a cyclic transfer, or is the last descriptor of the chain (the last period). */
			cursor = (cursor->next && (cursor->next != cursor->root)) ? cursor->next : NULL;
			
			if (cursor)
				dev_dbg(d->c->vc.chan.device->dev, "\t\t\t\t|--------------------------------------------------------------------------------------------|\n");
		}
		
#endif
		
		if (list_empty(&d->desc_list)) {
			
			/* 
			   This descriptor comes from device_prep_interrupt, and nobody has added info into it, so mark the cookie as completed to trigger 
			   the associated callback, and try to process any pending descriptors.
			*/
			
			c->status = S805_DMA_SUCCESS;
			
			vchan_cookie_complete(&d->vd);
		  	
			continue;
		}
		
		spin_lock(&mgr->lock);
		list_add_tail(&d->elem, &mgr->scheduled);
		spin_unlock(&mgr->lock);

		d->c->pending ++;
		c->status = S805_DMA_IN_PROGRESS;	
	}
}

/**
 * s805_dma_fetch_tr - Perform a batch of previously scheduled transactions, zero if none is available, 
 * at most %S805_DMA_MAX_HW_THREAD. General @mgr->lock held by callers.
 *
 * @ini_thread: Id of the first free thread.
 *
 */
static void s805_dma_fetch_tr ( uint ini_thread ) {
	
	uint thread, thread_mask = 0;
	struct s805_desc * d, * aux;
	
#ifdef CONFIG_S805_DMAC_SERIALIZE
	uint thread_disable = S805_DMA_MAX_THREAD;
#else
	uint thread_disable = S805_DMA_MAX_HW_THREAD;
#endif
	
#ifdef CONFIG_S805_DMAC_TO
	if (mgr->timer_busy)
		s805_dma_to_stop(); /* To avoid false positives */
#endif
	
	mgr->busy = (ini_thread > 0);
	mgr->__pending = ini_thread;
	
	for (thread = ini_thread; thread < mgr->max_thread; thread ++) {

		s805_dma_thread_disable(thread);
		
	    d = list_first_entry_or_null(&mgr->scheduled, struct s805_desc, elem);
		
		while (d) {
			
			if (d->c->status != S805_DMA_PAUSED && d->c->status != S805_DMA_TERMINATED) {
				
				if (s805_desc_is_cyclic(d)) {
					
					if (!mgr->cyclic_busy) {
						
						mgr->cyclic_busy = true;
						break;
						
					} else
						goto next;
					
#ifdef S805_CRYPTO_CIPHER
					
				} else if (s805_desc_is_crypto_cipher(d)) {

					if (!mgr->cipher_busy) {
						
						mgr->cipher_busy = true;
						break;
						
					} else
						goto next;
#endif
				} else
					break;
				
			} else {

			next:
				aux = d;
				
				if (list_is_last(&d->elem, &mgr->scheduled))
				    d = NULL;
				else 
					d = list_next_entry(d, elem);
				
				if (aux->c->status == S805_DMA_TERMINATED) {

					if (s805_desc_is_cyclic(aux) && mgr->cyclic_busy) 
						mgr->cyclic_busy = false;
					
					list_del(&aux->elem);
					s805_dma_desc_free(&aux->vd);
					
				}
			}			
		}
		
		if (d) {
			
			list_move_tail(&d->elem, &mgr->in_progress);
			
		    d->next_chunk = s805_dma_allocate_tr (thread,
												  d->next_chunk ? d->next_chunk : list_first_entry(&d->desc_list, s805_dtable, elem),
												  d->frames);
			
			if (d->c->status == S805_DMA_SUCCESS)
				d->c->status = S805_DMA_IN_PROGRESS;
			
			thread_mask |= (1 << thread);
			
		    mgr->busy = true;
			mgr->__pending ++;
		} 
	}


#ifndef CONFIG_S805_DMAC_SERIALIZE
#ifdef CONFIG_S805_DMAC_TO
	
	if (mgr->busy) {
   
		/* Coming from timeout */
		if (mgr->thread_reset)
			mgr->thread_reset --;

	}
	
#endif //TIME-OUT
	
	if ((mgr->max_thread != S805_DMA_MAX_THREAD) &&
		(!mgr->thread_reset || list_empty(&mgr->scheduled))) {
		
		mgr->thread_reset = 0;
		mgr->max_thread = S805_DMA_MAX_THREAD;	
	}
	
#endif //SERIALIZE

#ifdef CONFIG_S805_DMAC_TO
	if (mgr->busy)
		s805_dma_to_start(S805_DMA_TIME_OUT);
#endif
	
	for (thread = 0; thread < thread_disable; thread ++) {
		
		if ((thread < ini_thread) || (thread_mask & (1 << thread)))
			s805_dma_thread_enable(thread);
		else
			s805_dma_thread_disable(thread);
	}
}

/**
 * s805_dma_process_next_desc - Schedule issued descriptors for a channel, and, if the driver is free, process them.
 * Will return the status of the channel after the operation.
 *
 * @c: Channel holding the transactions to be performed.
 *
 */
static s805_status s805_dma_process_next_desc ( struct s805_chan *c )
{
	
	if (c->status != S805_DMA_PAUSED && c->status != S805_DMA_TERMINATED)	
		s805_dma_schedule_tr(c);
	
	
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

/**
 * s805_dma_process_completed - Process a completed batch of descriptors, this function will be run in a tasklet
 * scheduled by the ISR when all the pending transactions for a batch are finished.
 *
 * @null: Unused.
 *
 */
static void s805_dma_process_completed ( unsigned long null )
{
	struct s805_desc * d, * temp;
	uint thread = 0;

#ifdef CONFIG_S805_DMAC_TO
	if (mgr->timer_busy)
		s805_dma_to_stop();
#endif
	
	list_for_each_entry_safe (d, temp, &mgr->in_progress, elem) {
		
		/* All the transactions has been completed, process the finished descriptors.*/
		
		if (likely(d->c->status != S805_DMA_TERMINATED)) {
			
			if (likely(!d->next_chunk)) {
				
				list_del(&d->elem);
				
				if (s805_desc_is_cyclic(d)) {
					
					/* Call cyclic callback. */
					vchan_cyclic_callback(&d->root->vd);
					
					if (d->next == d->root) 
						mgr->cyclic_busy = false;
					
					if (d->next->c->status != S805_DMA_PAUSED && mgr->cyclic_busy) {
						
						list_add_tail(&d->next->elem, &mgr->in_progress);
						
						/* Must always return NULL */
						s805_dma_allocate_tr (thread,
											  list_first_entry(&d->next->desc_list, s805_dtable, elem),
											  d->next->frames);
						
						thread ++;
							
					} else {

						mgr->cyclic_busy = false;
						
						spin_lock(&mgr->lock);
						list_add_tail(&d->next->elem, &mgr->scheduled);
						spin_unlock(&mgr->lock);	
					}
					
				} else {
					
					dev_dbg(d->c->vc.chan.device->dev, "Marking cookie %d completed for channel %s.\n", d->vd.tx.cookie, dma_chan_name(&d->c->vc.chan));

				    if (!s805_desc_is_crypto_crc(d)) {

#ifdef S805_CRYPTO_CIPHER
						
						if (s805_desc_is_crypto_cipher(d))
							mgr->cipher_busy = false;
#endif
						spin_lock(&d->c->vc.lock);
					
						vchan_cookie_complete(&d->vd);
					
						spin_unlock(&d->c->vc.lock);

					} else 
						d->vd.tx.callback(d->vd.tx.callback_param); /* !!! Won't free the descriptor, temporal until CRC irq is received.*/
				}
				
			} else {
				
				/* 
				   If we reach this code the scheduled descriptor have more than S805_DMA_MAX_DESC data chunks, 
				   so we need to restart the transaction from the last descriptor processed. 
				*/
								
				d->frames -= S805_DMA_MAX_DESC;
				
				dev_dbg(d->c->vc.chan.device->dev, "Re-scheduling cookie %d for channel %s, frames left: %u.\n", d->vd.tx.cookie, dma_chan_name(&d->c->vc.chan), d->frames);
				
				if (d->c->status != S805_DMA_PAUSED) {
					
					list_move_tail(&d->elem, &mgr->in_progress);
					
					d->next_chunk = s805_dma_allocate_tr (thread, d->next_chunk, d->frames);
					
					thread ++;
					
				} else {

					spin_lock(&mgr->lock);
					list_move_tail(&d->elem, &mgr->scheduled);
					spin_unlock(&mgr->lock);
				}
			}

		} else {
			
			dev_dbg(d->c->vc.chan.device->dev, "Terminating transaction %d for channel %s.\n", d->vd.tx.cookie, dma_chan_name(&d->c->vc.chan));

			if (s805_desc_is_cyclic(d))
				mgr->cyclic_busy = false;

#ifdef S805_CRYPTO_CIPHER
			
			/* Must never happen ... */
			if (s805_desc_is_crypto_cipher(d))
				mgr->cipher_busy = false;
#endif
			
			list_del(&d->elem);
			s805_dma_desc_free(&d->vd);
		}
	}

	spin_lock(&mgr->lock);
   	s805_dma_fetch_tr(thread);
	spin_unlock(&mgr->lock);
}

/**
 * s805_dma_callback - ISR: All transactions must submit one IRQ when a batch of at most four chunks is finished, 
 * Notice that if more than one transaction is scheduled in a batch we won't know which of them finished, so we 
 * account the transaction in mrg->@__pending till no transaction is left, when this happens a tasklet will be
 * scheduled with high priority to process completed transactions.
 *
 * @irq: IRQ number for our device (%S805_DMA_IRQ).
 * @data: Pointer to @mgr.
 *
 */

static irqreturn_t s805_dma_callback (int irq, void *data)
{
	struct s805_dmadev *m = data;
	
	preempt_disable();
	
	if (! --m->__pending)
		tasklet_hi_schedule(&m->tasklet_completed);

    preempt_enable();
	
	return IRQ_HANDLED;
}

/**
 * s805_dma_dismiss_chann - Dismiss all scheduled transactions for a channel. Protected by @mgr->lock. 
 *
 * @c: Channel holding the transactions to be dismissed.
 *
 */
static void s805_dma_dismiss_chann ( struct s805_chan * c ) {
	
	struct s805_desc * d, * tmp;

	list_for_each_entry_safe (d, tmp, &mgr->scheduled, elem) {
		
		if ( d->c == c )  {
			
			list_del(&d->elem);
			s805_dma_desc_free(&d->vd);
		}
	}
}

/**
 * s805_dma_chan_wait_for_pending - Wait for a channel to finish its pending transactions. 
 *
 * @c: Channel to wait for.
 *
 */
static s805_status s805_dma_chan_wait_for_pending (struct s805_chan * c) {

	unsigned long now;
	unsigned int alive = (S805_DMA_TIME_OUT / 10) * 2; /* Two timeouts or about 300 ms if time out is not set. */
	
	while (c->pending > 0 && alive) {
		
		/* Wait for the remaining part of the current jiffie. */
		now = jiffies;
		while (time_before(jiffies, now + 1))
			cpu_relax();
		
		alive --;
	}
	
	if (!alive)
		dev_err(c->vc.chan.device->dev, "%s (%s): timed-out!\n", __func__, dma_chan_name(&c->vc.chan));

	return alive ? S805_DMA_SUCCESS : S805_DMA_ERROR;
}

/**
 * s805_dma_chan_terminate - Wait for a terminated channel to abort and finish its transactions. 
 *
 * @c: Channel to wait for.
 *
 */
static s805_status s805_dma_chan_terminate ( struct s805_chan * c, s805_status init ) {
	
	s805_status status = S805_DMA_ERROR;

	if (init != S805_DMA_PAUSED)
		status = s805_dma_chan_wait_for_pending(c);
	
	/* Dismiss all scheduled operations */
	if (status == S805_DMA_ERROR) {
		
		spin_lock(&mgr->lock);
		s805_dma_dismiss_chann(c);
		spin_unlock(&mgr->lock);
		
	}
	
	return s805_dma_chan_wait_for_pending(c);
}

/**
 * s805_dma_control - Endpoint function for device_control (dmaengine interface).
 * The status of the channel after the operation will be returned.
 *
 * @chann: Channel to wait for.
 * @cmd: Command to perform.
 * @arg: Pointer to a dma_slave_config struct, for DMA_SLAVE_CONFIG only.
 *
 */
static int s805_dma_control (struct dma_chan *chan, 
							 enum dma_ctrl_cmd cmd,
							 unsigned long arg) 
{
	
    struct s805_chan * c = to_s805_dma_chan(chan);
	
	switch (cmd) {
	case DMA_TERMINATE_ALL:
		{	
			if (!c->pending)
				return c->status;
			
			c->status = S805_DMA_TERMINATED;
			
			spin_lock(&c->vc.lock);
			
			vchan_dma_desc_free_list(&c->vc, &c->vc.desc_submitted);
			
			spin_unlock(&c->vc.lock);
			
			/* 
			   Returned status will be S805_DMA_TERMINATED it will became 
			   S805_DMA_SUCCESS when no pending transaction is left. 
			*/
		}
		break;;
		
	case DMA_PAUSE:
		{
			
			/* 
			   If there is a transaction in progress we will let the current batch of desciptors finish, 
			   a new batch won't be scheduled however. 
			*/
			
			c->status = S805_DMA_PAUSED;

		}
		break;;
		
	case DMA_RESUME:
		{
			if (c->status == S805_DMA_PAUSED) {
				
				spin_lock(&c->vc.lock);
				c->status = S805_DMA_SUCCESS;
				
				if (vchan_issue_pending(&c->vc)) {
					
					s805_dma_process_next_desc(c);
					spin_unlock(&c->vc.lock);
					
				} else {
					
					spin_unlock(&c->vc.lock);
					
					spin_lock(&mgr->lock);
					if (!mgr->busy) 
						s805_dma_fetch_tr(0);
					
					spin_unlock(&mgr->lock);
				}
			}
		}
	    break;;
		
	case DMA_SLAVE_CONFIG:
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
				(cfg->dst_addr_width != DMA_SLAVE_BUSWIDTH_8_BYTES || !cfg->dst_addr)) 
				{
					return -EINVAL;
				}
			
			c->cfg = *cfg;
		}
		break;;
		
	default:
		break;;
	}
	
	return c->status;
}

/**
 * get_residue - Auxiliar function to get the residue for a descriptor.
 *
 * @me: Descriptor to get the residue for.
 *
 */
static u32 get_residue (struct s805_desc * me) {

	struct s805_desc * cursor;
	s805_dtable *desc, *temp;
	u32 residue = 0;

	/* Cyclic transfer*/
	if (s805_desc_is_cyclic(me)) {

		cursor = me;
		
		/* Will count the prediods we are lacking till the end of the buffer. */
		while (cursor != me->root) {
		   		
			list_for_each_entry_safe (desc, temp, &cursor->desc_list, elem)
				residue += desc->table->count;
			
			cursor = cursor->next;
		}
		
	} else {
		
		list_for_each_entry_safe (desc, temp, &me->desc_list, elem)
			residue += desc->table->count;
	}
	
	return residue;
}

/**
 * s805_dma_tx_status - Endpoint function for dma_tx_status (dmaengine interface).
 * Will return the status of the channel.
 *
 * @chann: Channel holding the transaction identified by @cookie.
 * @cookie: Identifier of a transaction in a channel.
 * @txstate: Struct to put the residue of the transaction. 
 *
 */
enum dma_status s805_dma_tx_status(struct dma_chan *chan,
								   dma_cookie_t cookie,
								   struct dma_tx_state *txstate) 
{
	
	enum dma_status ret;
	struct s805_desc * d, * temp;
	u32 residue = 0;
	
	ret = dma_cookie_status(chan, cookie, txstate);
	
	if (ret == DMA_SUCCESS)
		return ret;
	
	/* Underprotected: to be tested! */
	
	list_for_each_entry_safe (d, temp, &mgr->scheduled, elem) {
		
		if (d->vd.tx.cookie == cookie)
			residue = get_residue (d);
	}
	
	dma_set_residue(txstate, residue);
	
	return ret;
}

/**
 * s805_dma_issue_pending - Endpoint function for dma_issue_pending (dmaengine interface).
 *
 * @chann: Channel holding the transactions to be issued.
 *
 */
static void s805_dma_issue_pending(struct dma_chan *chan)
{
	struct s805_chan *c = to_s805_dma_chan(chan);

	/* 
	   If any prevoiusly terminated channel tries to issue a new transaction while the former ones are yet unfreed
	   new transactions won't be scheduled, and there is no way to inform the user about this from here, so, 
	   we need to either add a new command to dma_control to return the current status (notice that any "non-standard" 
	   command will return the current status of the channel) for the user to take care of this, or modify this 
	   function to return the status of the channel after the operation. As mentioned above, if a user runs 
	   device->dma_control() the current status will be returned, however this is not offered in the interface.   
	*/
	
	spin_lock(&c->vc.lock);
	if (vchan_issue_pending(&c->vc))  {
		
		s805_dma_process_next_desc(c);
		spin_unlock(&c->vc.lock);
		
	} else
		spin_unlock(&c->vc.lock);
}

/**
 * s805_dma_free_chan_resources - Endpoint function for device_free_chan_resources (dmaengine interface).
 *
 * @chann: Channel to free resources for.
 *
 */
static void s805_dma_free_chan_resources(struct dma_chan *chan)
{
	struct s805_chan *c = to_s805_dma_chan(chan);
	s805_status init = c->status;
	
	c->status = S805_DMA_TERMINATED;
	
	vchan_free_chan_resources(&c->vc);
	
	s805_dma_chan_terminate (c, init);
	
	dma_pool_destroy(c->pool);
}

/**
 * s805_dma_alloc_chan_resources - Endpoint function for device_alloc_chan_resources (dmaengine interface).
 *
 * @chann: Channel to allocate resources for.
 *
 */
static int s805_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct s805_chan *c = to_s805_dma_chan(chan);
	struct device *dev = c->vc.chan.device->dev;

    
	c->pool = dma_pool_create_restore(dev_name(dev),
									  dev,
									  sizeof(struct s805_table_desc),
									  sizeof(struct s805_table_desc),
									  0);
	if (!c->pool) {
		
		dev_err(dev, "Unable to allocate descriptor pool.\n");
		return -ENOMEM;
		
	}
	
	c->status = S805_DMA_SUCCESS;
	c->pending = 0;
	
	return 0;  
}

/**
 * s805_dma_chan_init - Probe an s805 DMAC dma channel.
 *
 * @m: General manager device.
 *
 */
static int s805_dma_chan_init (struct s805_dmadev *m)
{
	struct s805_chan *c;
	
	c = devm_kzalloc(m->ddev.dev, sizeof(struct s805_chan), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	
	c->vc.desc_free = s805_dma_desc_free;
	vchan_init(&c->vc, &m->ddev);
	
	c->status = S805_DMA_SUCCESS;
	
	return 0;
}

/**
 * s805_dmamgr_probe - Allocate the global structures that will hold s805 DMAC manager information.
 *
 * @pdev: Platform device.
 *
 */
static int s805_dmamgr_probe(struct platform_device *pdev)
{	

	mgr = devm_kzalloc(&pdev->dev, sizeof(struct s805_dmadev), GFP_KERNEL);
	if (!mgr)
		return -ENOMEM;
	
	pdev->dev.dma_parms = &mgr->dma_parms;
	
	/* Init internal structs */
	mgr->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&mgr->ddev.channels);
	
	spin_lock_init(&mgr->lock);
	
	INIT_LIST_HEAD(&mgr->scheduled);
	INIT_LIST_HEAD(&mgr->in_progress);
	
	mgr->tasklet_completed = (struct tasklet_struct) { NULL, 0, ATOMIC_INIT(0), s805_dma_process_completed, 0 };
	
	mgr->irq_number = S805_DMA_IRQ;
	mgr->max_thread = S805_DMA_MAX_THREAD;
	
#ifdef CONFIG_S805_DMAC_TO
	if (s805_dma_to_init())
		return -1;
#endif

	dev_info(&pdev->dev, "DMA legacy API manager at 0x%p.\n", mgr);
	
	return request_irq(mgr->irq_number, s805_dma_callback, 0, "s805_dmaengine_irq", mgr); /* IRQF_IRQPOLL ?? */
}


/**
 * s805_dma_free - Free s805 DMAC manager device.
 *
 * @m: General manager device.
 *
 */
static void s805_dma_free(struct s805_dmadev *m)
{
	/* Check for active descriptors. */
	struct s805_chan *c, *next;
	
	list_for_each_entry_safe(c, next, &m->ddev.channels,
							 vc.chan.device_node) {
		
		list_del(&c->vc.chan.device_node);
		tasklet_kill(&c->vc.task);
	}
	
	free_irq(m->irq_number, m);
	
#ifdef CONFIG_S805_DMAC_TO
	s805_dma_to_shutdown();
#endif
	
}

/**
 * get_chan_num_cmdline - Get the desired amount of dma channels from the kernel cmd line.
 *
 * @str: String for our option.
 *
 */
static int get_chan_num_cmdline (char *str)
{
	
	get_option(&str, &dma_channels);
    return 1;
	
}

__setup("dma_channels=", get_chan_num_cmdline);


/**
 * s805_dma_probe - Probe subsystem.
 *
 * @pdev: Platform device.
 *
 */
static int s805_dma_probe (struct platform_device *pdev)
{
	
	int ret, i;
	
	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;
	
	ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));
	
	if (ret)
		return ret;
	
	dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	
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
	
	/* Datasheet p.57, entry 3 */
	dma_set_max_seg_size(&pdev->dev, S805_MAX_TR_SIZE); // is this correct?
	
	/* 
	   DMA_CAPABILITIES: All channels needs to be either private or public, we don't set the DMA_PRIVATE capabilitie 
	   to make them all public so we can give support to the async_tx api and network or audio drivers. 
	*/

	//dma_cap_set(DMA_PRIVATE, mgr->ddev.cap_mask);
	dma_cap_set(DMA_SLAVE, mgr->ddev.cap_mask);
	dma_cap_set(DMA_INTERRUPT, mgr->ddev.cap_mask);
	
	dma_cap_set(DMA_ASYNC_TX, mgr->ddev.cap_mask);
	dma_cap_set(DMA_INTERLEAVE, mgr->ddev.cap_mask);

	/* Those needs to be exposed in linux/dmaengine.h, maybe backported from 4.x */
	dma_cap_set(DMA_CYCLIC, mgr->ddev.cap_mask);
	dma_cap_set(DMA_SG, mgr->ddev.cap_mask);
	dma_cap_set(DMA_MEMCPY,  mgr->ddev.cap_mask);
	dma_cap_set(DMA_MEMSET,  mgr->ddev.cap_mask);
	
	
	/* Demanded by dmaengine interface: */ 
	mgr->ddev.device_tx_status = s805_dma_tx_status;
	mgr->ddev.device_issue_pending = s805_dma_issue_pending;
	mgr->ddev.device_control = s805_dma_control;
	mgr->ddev.device_alloc_chan_resources = s805_dma_alloc_chan_resources;
	mgr->ddev.device_free_chan_resources = s805_dma_free_chan_resources;
	
	/* Capabilities: */
	mgr->ddev.device_prep_slave_sg = s805_dma_prep_slave_sg;
	mgr->ddev.device_prep_interleaved_dma = s805_dma_prep_interleaved;
	mgr->ddev.device_prep_dma_cyclic = s805_dma_prep_dma_cyclic;
	mgr->ddev.device_prep_dma_sg = s805_dma_prep_sg;
	mgr->ddev.device_prep_dma_memcpy = s805_dma_prep_memcpy;
	mgr->ddev.device_prep_dma_memset = s805_dma_prep_memset;
	mgr->ddev.device_prep_dma_interrupt = s805_dma_prep_interrupt;
	
	platform_set_drvdata(pdev, mgr);
	
    dev_info(&pdev->dev, "Entering s805 DMA engine probe, chan available: %u, IRQ: %u\n", mgr->chan_available, S805_DMA_IRQ);
	
	for (i = 0; i < mgr->chan_available; i++) {
		
	    if (s805_dma_chan_init(mgr))
			goto err_no_dma;
		
	}
	
	dev_dbg(&pdev->dev, "Initialized %i DMA channels\n", i);
	
	ret = dma_async_device_register(&mgr->ddev);
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
	s805_dma_free(mgr);
	
	return ret;
}

static int s805_dma_remove(struct platform_device *pdev)
{
	struct s805_dmadev *m = platform_get_drvdata(pdev);

	dma_async_device_unregister(&m->ddev);
	s805_dma_free(m);

	return 0;
}

static struct platform_driver s805_dma_driver = {
	.probe	= s805_dma_probe,
	.remove	= s805_dma_remove,
	.driver = {
		.name = "s805-dmac",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(s805_dma_of_match),
	},
};

static int __init s805_init(void)
{
	return platform_driver_register(&s805_dma_driver);
}

static void __exit s805_exit(void)
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
MODULE_AUTHOR("szz-dvl");
MODULE_LICENSE("GPL v2");
