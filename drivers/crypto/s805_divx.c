#include <linux/platform_device.h>
#include <linux/module.h>
#include <crypto/algapi.h>
#include <../backports/include/crypto/internal/acompress.h>
#include <linux/dma-mapping.h>
#include <linux/s805_dmac.h>

#define S805_DIVX_BLOCK_SIZE                 2 /*??*/
#define S805_DIVX_CTRL                       P_NDMA_RIJNDAEL_CONTROL
#define S805_DIVX_RK_FIFO                    P_NDMA_RIJNDAEL_RK_FIFO

#define S805_DTBL_DIVX_POST_ENDIAN(type)     (type & 0x7)
#define S805_CTRL_DIVX_NR_VALUE(val)         (val & 0xf)
#define S805_CTRL_DIVX_PUSH_RK_FIFO          BIT(31)

#define S805_CTRL_DIVX_NR_VALUE_10           10
#define S805_CTRL_DIVX_NR_VALUE_12           12
#define S805_CTRL_DIVX_NR_VALUE_14           14

#ifdef CRYPTO_DEV_S805_DIVX_NR_10
#define S805_CTRL_DIVX_NR_VALUE_CFG          S805_CTRL_DIVX_NR_VALUE_10
#elif defined CRYPTO_DEV_S805_DIVX_NR_12
#define S805_CTRL_DIVX_NR_VALUE_CFG          S805_CTRL_DIVX_NR_VALUE_12
#elif defined CRYPTO_DEV_S805_DIVX_NR_14
#define S805_CTRL_DIVX_NR_VALUE_CFG          S805_CTRL_DIVX_NR_VALUE_14
#endif

#ifndef S805_CTRL_DIVX_NR_VALUE_CFG
#define S805_CTRL_DIVX_NR_VALUE_CFG          S805_CTRL_DIVX_NR_VALUE_12
#endif

struct s805_divx_mgr {

	struct device * dev;
    struct s805_chan * chan;
	struct list_head jobs;
	spinlock_t lock;

	bool busy;
	
};

struct s805_divx_chain {

	struct dma_async_tx_descriptor * tx_desc;
	dma_addr_t rk_fifo; /* Source address of DivX compressed data? */
	
	struct s805_divx_chain * next;
};

struct s805_divx_reqctx {

	struct s805_divx_chain * cursor;
	struct list_head elem;
};

struct s805_divx_mgr * divx_mgr;

static const struct of_device_id s805_divx_of_match[] =
{
    {.compatible = "aml,amls805-divx"},
	{},
};


/* Auxiliar function to initialize descriptors. */
static s805_dtable * def_init_divx_tdesc (unsigned int frames)
{
	
	s805_dtable * desc_tbl = kzalloc(sizeof(s805_dtable), GFP_NOWAIT);
	
	if (!desc_tbl) 
	    return NULL;
	
	desc_tbl->table = dma_pool_alloc(divx_mgr->chan->pool, GFP_NOWAIT | __GFP_ZERO, &desc_tbl->paddr); /* __GFP_ZERO: Not Working. */
	
	if (!desc_tbl->table) {
		
		kfree(desc_tbl);
		return NULL;
		
	} else
		*desc_tbl->table = (struct s805_table_desc) { 0 };
	
	/* Control common part */
	desc_tbl->table->control |= S805_DTBL_PRE_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->control |= S805_DTBL_INLINE_TYPE(INLINE_DIVX);

	if (!((frames + 1) % S805_DMA_MAX_DESC))
		desc_tbl->table->control |= S805_DTBL_IRQ;

	/* Crypto block */
	desc_tbl->table->crypto |= S805_DTBL_DIVX_POST_ENDIAN(ENDIAN_NO_CHANGE);
	
	return desc_tbl;
	
}

s805_dtable * sg_divx_move_along (s805_dtable * cursor, s805_init_desc * init_nfo) {

	if (cursor) {
		list_add_tail(&cursor->elem, &init_nfo->d->desc_list);
 		init_nfo->d->frames ++;
	}
	
	return def_init_divx_tdesc(init_nfo->d->frames);
}

static int s805_divx_launch_job (struct s805_divx_reqctx *ctx, bool chain) {
	
	dma_cookie_t tx_cookie;
	
	spin_lock(&divx_mgr->lock);
	if (!divx_mgr->busy || chain) {
		divx_mgr->busy = true;
		spin_unlock(&divx_mgr->lock);

		/* I'm guessing that NR value refers to "noise reduction", in "crypto/Kconfig" are offered the available values, only at kernel compile time. */
		WR(ctx->cursor->rk_fifo, S805_DIVX_RK_FIFO);
		WR(S805_CTRL_DIVX_NR_VALUE(S805_CTRL_DIVX_NR_VALUE_CFG) | S805_CTRL_DIVX_PUSH_RK_FIFO, S805_DIVX_CTRL);
			
		tx_cookie = dmaengine_submit(ctx->cursor->tx_desc);
		
		if(tx_cookie < 0) {
			
			dev_err(divx_mgr->dev, "%s: Failed to get cookie.\n", __func__);
			return tx_cookie;
			
		}
		
		dma_async_issue_pending(&divx_mgr->chan->vc.chan);
		return 0;
		
	} else
		spin_unlock(&divx_mgr->lock);
	
	return 1;	
}

static void s805_divx_handle_completion (void * req_ptr) {
	
    struct acomp_req *req = req_ptr;
	struct s805_divx_reqctx * job = acomp_request_ctx(req);
    struct s805_divx_chain * aux = job->cursor;
	
	if (job->cursor->next)

		job->cursor = job->cursor->next;
		
	else {

		req->base.complete(&req->base, 0);
		
		spin_lock(&divx_mgr->lock);
		list_del(&job->elem);
		spin_unlock(&divx_mgr->lock);

		job = list_first_entry_or_null (&divx_mgr->jobs, struct s805_divx_reqctx, elem);
	}

	kfree(aux);
	
	if (job)  
		s805_divx_launch_job(job, true);
	else {
		spin_lock(&divx_mgr->lock);
	    divx_mgr->busy = false;
		spin_unlock(&divx_mgr->lock);
	}
}

static int s805_divx_decompress (struct acomp_req *req) {

	struct s805_divx_reqctx *ctx = acomp_request_ctx(req);
	s805_init_desc * init_nfo;
	struct scatterlist * aux_src, * aux_dst;
	struct s805_divx_chain * link, * aux;
	uint len;
	int j = 0;

	if (req->dst) {

		dev_err(divx_mgr->dev, "%s: Dst address already allocated, aborting.\n", __func__);
		return -EINVAL;
		
	}

	if (!req->src) {

		dev_err(divx_mgr->dev, "%s: No data received, aborting.\n", __func__);
		return -EINVAL;
		
	}
	
	init_nfo = kzalloc(sizeof(s805_init_desc), GFP_NOWAIT);
	
	if (!init_nfo) {
	    dev_err(divx_mgr->dev, "%s: Failed to allocate initialization info structure.\n", __func__);
		return -ENOMEM;
	}

    init_nfo->type = DIVX_DESC;
   
    link = ctx->cursor;
	sg_init_table (req->dst, sg_nents(req->src));
	aux_dst = req->dst;
	
	for_each_sg(req->src, aux_src, sg_nents(req->src), j) {

		len = sg_dma_len(aux_src);
		
	    ctx->cursor = kzalloc(sizeof(struct s805_divx_chain), GFP_NOWAIT);

		if (!ctx->cursor) {
			
			dev_err(divx_mgr->dev, "%s: Failed to allocate DivX link.\n", __func__);
		    goto dma_mapping_error;
		}

	    ctx->cursor->rk_fifo = sg_dma_address(aux_src);
	    ctx->cursor->tx_desc = dmaengine_prep_dma_interrupt (&divx_mgr->chan->vc.chan, 0);

		if (!ctx->cursor->tx_desc) {
		
			dev_err(divx_mgr->dev, "%s: Failed to allocate dma descriptor %d.\n", __func__, j);
			goto dma_mapping_error;
			
		}

	    ctx->cursor->tx_desc->callback = (void *) &s805_divx_handle_completion;
		ctx->cursor->tx_desc->callback_param = (void *) req;
	
		sg_set_buf(aux_dst,
				   dma_alloc_coherent(divx_mgr->chan->vc.chan.device->dev,
									  len,
									  &sg_dma_address(aux_dst),
									  GFP_ATOMIC), /* Memory won't be zeroed */
				   len);
	    
		if (dma_mapping_error(divx_mgr->chan->vc.chan.device->dev, sg_dma_address(aux_dst))) {
			
			dev_err(divx_mgr->dev, "%s: Failed to allocate dst buffer.\n", __func__);
			goto dma_mapping_error;
		}

		/* 
		   Source adresses will be writed in the descriptors as well in the RK_FIFO register (at the moment of launching the job), 
		   since I don't know where the hw will look for the value. 
		*/
	    ctx->cursor->tx_desc = s805_scatterwalk (aux_src, aux_dst, init_nfo, ctx->cursor->tx_desc, true);

		if (!ctx->cursor->tx_desc) {
		
			dev_err(divx_mgr->dev, "%s: Failed to allocate data chunk %d.\n", __func__, j);
			goto dma_mapping_error;
			
		}
		
	    ctx->cursor = ctx->cursor->next;
		aux_dst = sg_next(aux_dst);
		
	}

	ctx->cursor = link;
	kfree (init_nfo);
	req->base.data = req->dst; /* To easily recover the result from completion callback. */

	spin_lock(&divx_mgr->lock);
	list_add_tail(&ctx->elem, &divx_mgr->jobs);
	spin_unlock(&divx_mgr->lock);
	
	return s805_divx_launch_job(ctx, false);

 dma_mapping_error:

	aux_dst = req->dst;
	
	while (link) {

		if (link->tx_desc)
			s805_desc_early_free (link->tx_desc);

		if (sg_dma_address(aux_dst) != DMA_ERROR_CODE) 
			dma_free_coherent(divx_mgr->chan->vc.chan.device->dev, sg_dma_len(aux_dst), sg_virt(aux_dst), sg_dma_address(aux_dst));
		
		aux = link->next;
		kfree(link);
		
		link = aux;
	}
	
	return -ENOMEM;
}

static int s805_divx_init (struct crypto_acomp *tfm) {

	tfm->reqsize = sizeof(struct s805_divx_reqctx); /* redundant? */

	return 0;
}

static void s805_divx_dst_free (struct scatterlist *dst)
{
	struct scatterlist * aux;
	uint j = 0;
	
	for_each_sg(dst, aux, sg_nents(dst), j)
		dma_free_coherent(divx_mgr->chan->vc.chan.device->dev, sg_dma_len(aux), sg_virt(aux), sg_dma_address(aux));
	
}

static void s805_divx_exit (struct crypto_acomp *tfm)
{	
}

static void s805_divx_cra_exit(struct crypto_tfm *tfm)
{
}

static int s805_divx_cra_init(struct crypto_tfm *tfm)
{ 	
	return 0;
}


static struct acomp_alg divx_alg = {
	.decompress = s805_divx_decompress,
	.dst_free   = s805_divx_dst_free,
	.init       = s805_divx_init,
	.exit       = s805_divx_exit,
	.reqsize    = sizeof(struct s805_divx_reqctx),
	.base	    = {
		.cra_name		    = "DivX",
		.cra_driver_name	= "s805-DivX",
		.cra_priority		= 100,
		.cra_flags		    = CRYPTO_ALG_TYPE_ACOMPRESS | CRYPTO_ALG_ASYNC,
		.cra_blocksize		= S805_DIVX_BLOCK_SIZE,
		.cra_ctxsize		= 0,
		.cra_alignmask		= 0,
		.cra_module		    = THIS_MODULE,
		.cra_init		    = s805_divx_cra_init,
		.cra_exit		    = s805_divx_cra_exit,
	}
};

static int s805_divx_probe(struct platform_device *pdev)
{

	int err;
	static dma_cap_mask_t mask;
	struct dma_chan * chan;
	
    divx_mgr = kzalloc(sizeof(struct s805_divx_mgr), GFP_KERNEL);
	if (!divx_mgr) {
		dev_err(&pdev->dev, "s805 DivX mgr device failed to allocate.\n");
		return -ENOMEM;
	}

    divx_mgr->dev = &pdev->dev;

	INIT_LIST_HEAD(&divx_mgr->jobs);
	spin_lock_init(&divx_mgr->lock);
	
	err = crypto_register_acomp(&divx_alg);
	
	if (err) {
		
		dev_err(divx_mgr->dev, "s805 DivX: failed to register algorithm.\n");
		kfree(divx_mgr);
		return err;
	}

	dma_cap_zero(mask);
	dma_cap_set(DMA_INTERRUPT, mask);
	
    chan = dma_request_channel ( mask, NULL, NULL );
	
	if (!chan) {
		
		dev_err(divx_mgr->dev, "s805 DivX: failed to get dma channel.\n");
		kfree(divx_mgr);
		crypto_unregister_acomp(&divx_alg);
		
		return -ENOSYS;
		
	} else {
		
		dev_info(divx_mgr->dev, "s805 DivX: grabbed dma channel (%s).\n", dma_chan_name(chan));
		divx_mgr->chan = to_s805_dma_chan(chan);
	}
	
    dev_info(divx_mgr->dev, "Loaded S805 DivX crypto driver\n");

	return 0;
}

static int s805_divx_remove(struct platform_device *pdev)
{

	crypto_unregister_acomp(&divx_alg);
	dma_release_channel ( &divx_mgr->chan->vc.chan );
	kfree(divx_mgr);
	
	return 0;
}

static struct platform_driver s805_divx_driver = {
	.probe		= s805_divx_probe,
	.remove		= s805_divx_remove,
	.driver		= {
		.name = "s805-dmac-divx",
		.owner	= THIS_MODULE,
		.of_match_table = s805_divx_of_match
	},
};

module_platform_driver(s805_divx_driver);

MODULE_ALIAS("platform:s805-divx");
MODULE_DESCRIPTION("s805 DivX hw acceleration support.");
MODULE_AUTHOR("szz-dvl");
MODULE_LICENSE("GPL v2");
