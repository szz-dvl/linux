#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>
#include <crypto/algapi.h>
#include <linux/s805_dmac.h>

#define S805_CRC_CHECK_SUM                   P_NDMA_CRC_OUT
#define S805_CRC_DIGEST_SIZE                 4
#define S805_CRC_BLOCK_SIZE                  2 /* ?? */

#define S805_DTBL_CRC_NO_WRITE(val)          ((val & 0x1) << 4) 
#define S805_DTBL_CRC_RESET(val)             ((val & 0x1) << 3)
#define S805_DTBL_CRC_COUNT(count)           ((count & 0x3FFFFF) << 5)
#define S805_DTBL_CRC_POST_ENDIAN(type)      (type & 0x7)

struct s805_crc_mgr {

	struct device * dev;
    struct s805_chan * chan;
	struct list_head jobs;
	spinlock_t lock;

	bool busy;
	
};

struct s805_crc_reqctx {

	struct dma_async_tx_descriptor * tx_desc;
	s805_init_desc * init_nfo;
	bool initialized;
	bool finalized;
	
	struct list_head elem;

};

struct s805_crc_mgr * crc_mgr;

static const struct of_device_id s805_crc_of_match[] =
{
    {.compatible = "aml,amls805-crc"},
	{},
};


/* Auxiliar function to initialize descriptors. */
static s805_dtable * def_init_crc_tdesc (unsigned int frames)
{
	
	s805_dtable * desc_tbl = kzalloc(sizeof(s805_dtable), GFP_NOWAIT);
	
	if (!desc_tbl) 
	    return NULL;
	
	desc_tbl->table = dma_pool_alloc(crc_mgr->chan->pool, GFP_NOWAIT | __GFP_ZERO, &desc_tbl->paddr); /* __GFP_ZERO: Not Working. */
	
	if (!desc_tbl->table) {
		
		kfree(desc_tbl);
		return NULL;
		
	} else
		*desc_tbl->table = (struct s805_table_desc) { 0 };
	
	/* Control common part */
	desc_tbl->table->control |= S805_DTBL_PRE_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->control |= S805_DTBL_INLINE_TYPE(INLINE_CRC);

	if (!((frames + 1) % S805_DMA_MAX_DESC))
		desc_tbl->table->control |= S805_DTBL_IRQ;

	/* Crypto block */
	desc_tbl->table->crypto |= S805_DTBL_CRC_POST_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->crypto |= S805_DTBL_CRC_RESET(frames ? 0 : 1); /* To be tested. */
	desc_tbl->table->crypto |= S805_DTBL_CRC_NO_WRITE(1);

	desc_tbl->table->crypto |= S805_DTBL_CRC_COUNT(frames); /* Is this correct?, is the field really unused?, if it is not is the frame number what it expects? ... To be tested.*/
	
	return desc_tbl;
	
}

s805_dtable * sg_crc_move_along (s805_dtable * cursor, s805_init_desc * init_nfo) {

	if (cursor) {
		list_add_tail(&cursor->elem, &init_nfo->d->desc_list);
 		init_nfo->d->frames ++;
	}
	
	return def_init_crc_tdesc(init_nfo->d->frames);
}

static int s805_crc_launch_job (struct s805_crc_reqctx *ctx, bool chain) {
	
	dma_cookie_t tx_cookie;

	spin_lock(&crc_mgr->lock);
	if (!crc_mgr->busy || chain) {
		crc_mgr->busy = true;
		spin_unlock(&crc_mgr->lock);

		ctx->finalized = true;
		tx_cookie = dmaengine_submit(ctx->tx_desc);
		
		if(tx_cookie < 0) {
			
			dev_err(crc_mgr->dev, "%s: Failed to get cookie.\n", __func__);
			return tx_cookie;
			
		}
		
		dma_async_issue_pending(&crc_mgr->chan->vc.chan);
		return 0;
		
	} else
		spin_unlock(&crc_mgr->lock);
	
	return 1;
	
}

static void s805_crc_handle_completion (void * req_ptr) {

    struct ahash_request *req = req_ptr;
	struct s805_crc_reqctx *job = ahash_request_ctx(req);
	
	u32 result = RD(S805_CRC_CHECK_SUM);
	
	memcpy(req->result, &result, S805_CRC_DIGEST_SIZE);
	
	req->base.complete(&req->base, 0);
	
	spin_lock(&crc_mgr->lock);
	list_del(&job->elem);
	spin_unlock(&crc_mgr->lock);
	
	kfree(job->init_nfo);
	job->initialized = false;

	/* It will be user responsibility to free the result field, is this correct?, must we force the user to allocate the field too? */
	
	job = list_first_entry_or_null (&crc_mgr->jobs, struct s805_crc_reqctx, elem);

	if (job)  
		s805_crc_launch_job(job, true);
	else {
		spin_lock(&crc_mgr->lock);
	    crc_mgr->busy = false;
		spin_unlock(&crc_mgr->lock);
	}
}

static int s805_crc_add_data (struct ahash_request *req, bool last) {

	struct s805_crc_reqctx *ctx = ahash_request_ctx(req);

	if (!ctx->initialized) {

		dev_err(crc_mgr->dev, "%s: Uninitialized request.\n", __func__);
		return -ENOSYS;

	}

	if (ctx->finalized) {

		dev_err(crc_mgr->dev, "%s: Already finalized request.\n", __func__);
		return -EINVAL;
		
	}
	
	ctx->tx_desc = s805_scatterwalk (req->src, NULL, ctx->init_nfo, ctx->tx_desc, last);

	if (!ctx->tx_desc) {
		
		dev_err(crc_mgr->dev, "%s: Failed to add data chunk.\n", __func__);
		return -ENOMEM;
	}

	return 0;

}

static int s805_crc_init_ctx (struct ahash_request *req) {

	struct s805_crc_reqctx *ctx = ahash_request_ctx(req);

	if (ctx->initialized)
		return 1;
	else if (ctx->finalized)
		ctx->finalized = false;
	
	ctx->tx_desc = dmaengine_prep_dma_interrupt (&crc_mgr->chan->vc.chan, 0);

	if (!ctx->tx_desc) {
		
		dev_err(crc_mgr->dev, "%s: Failed to allocate dma descriptor.\n", __func__);
		return -ENOMEM;
	}

	ctx->tx_desc->callback = (void *) &s805_crc_handle_completion;
	ctx->tx_desc->callback_param = (void *) req;
	
	ctx->init_nfo = kzalloc(sizeof(s805_init_desc), GFP_NOWAIT); /* Must we allow atomic context here? */

	if (!ctx->init_nfo) {
	    dev_err(crc_mgr->dev, "%s: Failed to allocate initialization info structure.\n", __func__);
		return -ENOMEM;
	}

	ctx->init_nfo->type = CRC_DESC;

	if (!req->result) {

		/* User responsibility? */
		req->result = kzalloc(S805_CRC_DIGEST_SIZE, GFP_NOWAIT);

		if (!req->result) {
			dev_err(crc_mgr->dev, "%s: Failed to allocate result field.\n", __func__);
			kfree(ctx->init_nfo);
			return -ENOMEM;
		}
	}
	
	req->base.data = req->result; /* To easily recover the result from completion callback. */
	ctx->initialized = true;
	
	return 0;

}

static int s805_crc_hash_init (struct ahash_request *req) {

    return s805_crc_init_ctx (req);

}

static int s805_crc_hash_update (struct ahash_request *req) {
	
    return s805_crc_add_data (req, false);

}

static int s805_crc_hash_final (struct ahash_request *req) {

	struct s805_crc_reqctx *ctx = ahash_request_ctx(req);

	if (!ctx->initialized) {

		dev_err(crc_mgr->dev, "%s: Uninitialized request.\n", __func__);
		return -ENOSYS;

	}

	if (ctx->finalized) {

		dev_err(crc_mgr->dev, "%s: Already finalized request.\n", __func__);
		return -EINVAL;
		
	}
	
	s805_close_desc (ctx->tx_desc);
	
    spin_lock(&crc_mgr->lock);
	list_add_tail(&ctx->elem, &crc_mgr->jobs);
	spin_unlock(&crc_mgr->lock);
	
	return s805_crc_launch_job(ctx, false);
	
	return 0;

}

static int s805_crc_hash_finup (struct ahash_request *req) {

	struct s805_crc_reqctx *ctx = ahash_request_ctx(req);
    int err = s805_crc_add_data (req, true);
	
	if (err) {
		
		dev_err(crc_mgr->dev, "%s: Failed to add last data chunk.\n", __func__);
		return err;
	}

	spin_lock(&crc_mgr->lock);
	list_add_tail(&ctx->elem, &crc_mgr->jobs);
	spin_unlock(&crc_mgr->lock);
	
	return s805_crc_launch_job(ctx, false);

}

static int s805_crc_hash_digest (struct ahash_request *req) {

	struct s805_crc_reqctx *ctx = ahash_request_ctx(req);
	int err = s805_crc_init_ctx (req);

	if (err < 0) {
		
		dev_err(crc_mgr->dev, "%s: Failed to initialize context.\n", __func__);
		return err;
	}

	err = s805_crc_add_data (req, true);
	
	if (err) {
		
		dev_err(crc_mgr->dev, "%s: Failed to add last data chunk.\n", __func__);
		return err;
	}
	
	spin_lock(&crc_mgr->lock);
	list_add_tail(&ctx->elem, &crc_mgr->jobs);
	spin_unlock(&crc_mgr->lock);
	
	return s805_crc_launch_job(ctx, false);

}

static int s805_crc_hash_export (struct ahash_request *req, void *out) {
	
	out = ahash_request_ctx(req);

	return 0;
}

static int s805_crc_hash_import (struct ahash_request *req, const void *in) {

	memcpy(req->__ctx, in, sizeof(struct s805_crc_reqctx));
	
	return 0;

}

static int s805_crc_cra_init(struct crypto_tfm *tfm)
{
    crypto_ahash_set_reqsize(__crypto_ahash_cast(tfm),
							 sizeof(struct s805_crc_reqctx));
	
	return 0;

}

static void s805_crc_cra_exit(struct crypto_tfm *tfm)
{
}

static struct ahash_alg crc_alg = {
	.init		= s805_crc_hash_init,
	.update		= s805_crc_hash_update,
	.final		= s805_crc_hash_final,
	.finup		= s805_crc_hash_finup,
	.digest		= s805_crc_hash_digest,
	.import     = s805_crc_hash_import,
	.export     = s805_crc_hash_export,
	.halg.digestsize = S805_CRC_DIGEST_SIZE,
	.halg.statesize  = S805_CRC_DIGEST_SIZE, /* look 4 info. */
	.halg.base	     = {
		.cra_name		    = "crc-32",
		.cra_driver_name	= "s805-crc-32",
		.cra_priority		= 100,
		.cra_flags		    = CRYPTO_ALG_TYPE_AHASH | CRYPTO_ALG_ASYNC,
		.cra_blocksize		= S805_CRC_BLOCK_SIZE,
		.cra_ctxsize		= 0,
		.cra_alignmask		= 0,
		.cra_module		    = THIS_MODULE,
		.cra_init		    = s805_crc_cra_init,
		.cra_exit		    = s805_crc_cra_exit,
	}
};

static int s805_crc_probe(struct platform_device *pdev)
{

	int err;
	static dma_cap_mask_t mask;
	struct dma_chan * chan;
	
    crc_mgr = kzalloc(sizeof(struct s805_crc_mgr), GFP_KERNEL);
	if (!crc_mgr) {
		dev_err(&pdev->dev, "s805 CRC-32 mgr device failed to allocate.\n");
		return -ENOMEM;
	}

    crc_mgr->dev = &pdev->dev;

	INIT_LIST_HEAD(&crc_mgr->jobs);
	spin_lock_init(&crc_mgr->lock);
	
	err = crypto_register_ahash(&crc_alg);
	
	if (err) {
		
		dev_err(crc_mgr->dev, "s805 CRC-32: failed to register algorithm.\n");
		kfree(crc_mgr);
		return err;
	}

	dma_cap_zero(mask);
	dma_cap_set(DMA_INTERRUPT, mask);
	
    chan = dma_request_channel ( mask, NULL, NULL );
	
	if (!chan) {
		
		dev_err(crc_mgr->dev, "s805 CRC-32: failed to get dma channel.\n");
		kfree(crc_mgr);
		crypto_unregister_ahash(&crc_alg);
		
		return -ENOSYS;
		
	} else {
		
		dev_info(crc_mgr->dev, "s805 CRC-32: grabbed dma channel (%s).\n", dma_chan_name(chan));
		crc_mgr->chan = to_s805_dma_chan(chan);
	}
	
    dev_info(crc_mgr->dev, "Loaded S805 CRC-32 crypto driver\n");

	return 0;
}

static int s805_crc_remove(struct platform_device *pdev)
{

	crypto_unregister_ahash(&crc_alg);
	dma_release_channel ( &crc_mgr->chan->vc.chan );
	kfree(crc_mgr);
	
	return 0;
}

static struct platform_driver s805_crc_driver = {
	.probe		= s805_crc_probe,
	.remove		= s805_crc_remove,
	.driver		= {
		.name = "s805-dmac-crc",
		.owner	= THIS_MODULE,
		.of_match_table = s805_crc_of_match
	},
};

module_platform_driver(s805_crc_driver);

MODULE_ALIAS("platform:s805-crc");
MODULE_DESCRIPTION("s805 CRC-32 hw acceleration support.");
MODULE_AUTHOR("szz-dvl");
MODULE_LICENSE("GPL v2");
