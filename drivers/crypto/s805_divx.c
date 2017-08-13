#include <linux/platform_device.h>
#include <linux/module.h>
#include <crypto/algapi.h>
#include <../backports/include/crypto/internal/acompress.h>
#include <linux/dma-mapping.h>
#include <linux/s805_dmac.h>

#define S805_DIVX_BLOCK_SIZE                        8 /*??*/
#define S805_DIVX_CTRL                              P_NDMA_RIJNDAEL_CONTROL
#define S805_DIVX_RK_FIFO                           P_NDMA_RIJNDAEL_RK_FIFO

#define S805_DTBL_DIVX_POST_ENDIAN(type)            (type & 0x7)
#define S805_CTRL_DIVX_NR_VALUE(val)                (val & 0xf)
#define S805_CTRL_DIVX_PUSH_RK_FIFO                 BIT(31)

#define S805_CTRL_DIVX_NR_VALUE_10                  10
#define S805_CTRL_DIVX_NR_VALUE_12                  12
#define S805_CTRL_DIVX_NR_VALUE_14                  14

#ifdef CONFIG_CRYPTO_DEV_S805_DIVX_NR_10
#define S805_CTRL_DIVX_NR_VALUE_CFG                 S805_CTRL_DIVX_NR_VALUE_10
#elif defined CONFIG_CRYPTO_DEV_S805_DIVX_NR_12
#define S805_CTRL_DIVX_NR_VALUE_CFG                 S805_CTRL_DIVX_NR_VALUE_12
#elif defined CONFIG_CRYPTO_DEV_S805_DIVX_NR_14
#define S805_CTRL_DIVX_NR_VALUE_CFG                 S805_CTRL_DIVX_NR_VALUE_14
#endif

#ifndef S805_CTRL_DIVX_NR_VALUE_CFG
#define S805_CTRL_DIVX_NR_VALUE_CFG                 S805_CTRL_DIVX_NR_VALUE_12
#endif

struct s805_divx_mgr {

	struct device * dev;
    struct s805_chan * chan;
	struct list_head jobs;
	spinlock_t lock;

	bool busy;
	
};

struct s805_divx_reqctx {

    struct dma_async_tx_descriptor * tx_desc;
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
	desc_tbl->table->control |= S805_DTBL_NO_BREAK;
	
	/* 
	   We fix dst address to point to RK_FIFO register, is this correct? 
	   will the device be able to move data in chunks of 4 Bytes? 
	   will the FIFO consume the usual 8 Bytes quick enough? 
	*/
	
	desc_tbl->table->control |= S805_DTBL_DST_HOLD;
	desc_tbl->table->dst = S805_DIVX_RK_FIFO;
	
	if (!((frames + 1) % S805_DMA_MAX_DESC))
		desc_tbl->table->control |= S805_DTBL_IRQ;

	/* Crypto block */
	desc_tbl->table->crypto |= S805_DTBL_DIVX_POST_ENDIAN(ENDIAN_NO_CHANGE);
	
	return desc_tbl;
	
}

s805_dtable * sg_divx_move_along (struct s805_desc * d, s805_dtable * cursor) {

	if (cursor) {
		
		list_add_tail(&cursor->elem, &d->desc_list);
 	    d->frames ++;		
	}
	
	return def_init_divx_tdesc(d->frames);
}

static int s805_divx_launch_job (struct s805_divx_reqctx *ctx, bool chain) {
	
	dma_cookie_t tx_cookie;
	
	spin_lock(&divx_mgr->lock);
	if (!divx_mgr->busy || chain) {
		divx_mgr->busy = true;
		spin_unlock(&divx_mgr->lock);

		/* I'm guessing that NR value refers to "noise reduction", in "crypto/Kconfig" are offered the available values, only at kernel compile time. */
		WR(S805_CTRL_DIVX_NR_VALUE(S805_CTRL_DIVX_NR_VALUE_CFG) | S805_CTRL_DIVX_PUSH_RK_FIFO, S805_DIVX_CTRL);
			
		tx_cookie = dmaengine_submit(ctx->tx_desc);
		
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
	
	spin_lock(&divx_mgr->lock);
	list_del(&job->elem);
	spin_unlock(&divx_mgr->lock);
	
	job = list_first_entry_or_null (&divx_mgr->jobs, struct s805_divx_reqctx, elem);
	
	if (job)  
		s805_divx_launch_job(job, true);
	else {
		spin_lock(&divx_mgr->lock);
	    divx_mgr->busy = false;
		spin_unlock(&divx_mgr->lock);
	}

	/* Hopefully DivX decompression will happen in an "inline manner", so decrypted data will be in the src scatterlist provided by the user. in a thread!*/
	req->base.complete(&req->base, 0);
}

static int s805_divx_decompress (struct acomp_req *req) {

	struct s805_divx_reqctx *ctx = acomp_request_ctx(req);
	
	/* I'm missing a lot of info here, so I leave this implementation here and if any correction is needed, please apply it. */
	
	if (!req->src) {

		dev_err(divx_mgr->dev, "%s: No data received, aborting.\n", __func__);
		return -EINVAL;
		
	}

	ctx->tx_desc = dmaengine_prep_dma_interrupt (&divx_mgr->chan->vc.chan, S805_DMA_CRYPTO_FLAG | S805_DMA_CRYPTO_DIVX_FLAG);

	if (!ctx->tx_desc) {
		
		dev_err(divx_mgr->dev, "%s: Failed to get dma descriptor.\n", __func__);
		return -ENOMEM;
			
	}
		
	ctx->tx_desc = s805_scatterwalk (req->src, NULL, ctx->tx_desc, UINT_MAX, true);

	if (!ctx->tx_desc) {
		
		dev_err(divx_mgr->dev, "%s: Failed to allocate data chunks.\n", __func__);
		return -ENOMEM;
		
	}

	ctx->tx_desc->callback = (void *) &s805_divx_handle_completion;
	ctx->tx_desc->callback_param = (void *) req;
		
	spin_lock(&divx_mgr->lock);
	list_add_tail(&ctx->elem, &divx_mgr->jobs);
	spin_unlock(&divx_mgr->lock);
	
	return s805_divx_launch_job(ctx, false);
}

static int s805_divx_init (struct crypto_acomp *tfm) {

	tfm->reqsize = sizeof(struct s805_divx_reqctx); /* redundant? */

	return 0;
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
