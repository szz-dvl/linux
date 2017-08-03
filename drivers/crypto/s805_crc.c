#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>
#include <crypto/algapi.h>
#include <linux/s805_dmac.h>

#define S805_CRC_IRQ                         INT_AIU_CRC
#define S805_CRC_PARSER_IRQ                  INT_PARSER
#define S805_CRC_CHECK_SUM                   P_NDMA_CRC_OUT
#define S805_CRC_CTRL                        P_AIU_CRC_CTRL

#define S805_CRC_DIGEST_SIZE                 4
#define S805_CRC_BLOCK_SIZE                  8 /* DMAC Block moves are aligned to 8 Bytes, seems fair. */

#define S805_CRC_EXTRA_DEBUG                 S805_CRC_CTRL
#define S805_CRC_IRQ_MASK                    P_MEDIA_CPU_INTR_MASK
#define S805_CRC_IRQ_BITS                    (BIT(10) | BIT(27))

#define S805_CRC_P0LY_1                      P_AIU_CRC_POLY_COEF1
#define S805_CRC_P0LY_0                      P_AIU_CRC_POLY_COEF0

#define S805_CRC_P0LY_COEFS                  0x04C11DB7
#define S805_CRC_P0LY_COEFS_R                0xEDB88320
#define S805_CRC_P0LY_COEFS_RR               0x82608EDB

#define S805_CRC_P0LY_COEFS_16               0x8005
#define S805_CRC_P0LY_COEFS_R_16             0xA001
#define S805_CRC_P0LY_COEFS_RR_16            0xC002

#define S805_CRC_ENABLE                      (BIT(15) | BIT(6) | BIT(8) | BIT(9) | BIT(10) | BIT(11) | BIT(12) | BIT(13))
#define S805_CRC_AIU_CLK_GATE                P_HHI_GCLK_OTHER
#define S805_CRC_ENABLE_CLK                  (BIT(14) | BIT(16))
#define S805_CRC_CLK81                       P_HHI_GCLK_MPEG2
#define S805_CRC_ENABLE_CLK81                BIT(29)

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

/* struct sg_dest { */

/* 	struct scatterlist dst; */
/* 	struct sg_dest * next; */
/* }; */
	
struct s805_crc_reqctx {

	struct dma_async_tx_descriptor * tx_desc;
	s805_init_desc * init_nfo;
	bool initialized;
	bool finalized;

	/* struct scatterlist dst; */
	
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

	//desc_tbl->table->dst = ~0U; //Send data to the parser ¿¿??
	
	/* Crypto block */
	desc_tbl->table->crypto |= S805_DTBL_CRC_POST_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->crypto |= S805_DTBL_CRC_RESET(!frames); /* To be tested. !frames*/
	desc_tbl->table->crypto |= S805_DTBL_CRC_NO_WRITE(0);
	
	desc_tbl->table->crypto |= S805_DTBL_CRC_COUNT((frames + 1)); /* Is this correct?, is the field really unused?. To be tested.*/
	
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

		WR(S805_CRC_P0LY_COEFS_16, S805_CRC_P0LY_1);
		WR(S805_CRC_P0LY_COEFS_16, S805_CRC_P0LY_0);
		WR(0, S805_CRC_CHECK_SUM);
		//WR(0xFF00, S805_CRC_CTRL);
		
		
		tx_cookie = dmaengine_submit(ctx->tx_desc);
		
		if(tx_cookie < 0) {
			
			dev_err(crc_mgr->dev, "%s: Failed to get cookie.\n", __func__);
			return tx_cookie;
				
		}

		kfree(ctx->init_nfo); /* !!! */
		dma_async_issue_pending(&crc_mgr->chan->vc.chan);

		return 0;
		
	} else
		spin_unlock(&crc_mgr->lock);
	
	return 1;
	
}

static void s805_crc_handle_completion (void * req_ptr) {

    struct ahash_request *req = req_ptr;
	struct s805_crc_reqctx *job = ahash_request_ctx(req);
	uint i;
	u32 * res = (u32 *) req->result;

	u32 result = RD(S805_CRC_CHECK_SUM);
	memcpy(req->result, &result, S805_CRC_DIGEST_SIZE);

	/* WR(~0U, S805_CRC_CHECK_SUM); */
	
	for (i = 1; i < 12; i++) {

		u32 result = RD(S805_CRC_EXTRA_DEBUG + (i - 1));
	
		memcpy(&res[i], &result, S805_CRC_DIGEST_SIZE);
	}

	//print_hex_dump_bytes("Dest Content: ", DUMP_PREFIX_NONE, sg_virt(&job->dst), sg_dma_len(&job->dst));
	//dma_free_coherent(NULL, sg_dma_len(&job->dst), sg_virt(&job->dst), sg_dma_address(&job->dst));
	//WR(0xFFFF, S805_CRC_CTRL);
	
	spin_lock(&crc_mgr->lock);
	list_del(&job->elem);
	spin_unlock(&crc_mgr->lock);
	
	job->initialized = false;
	
	job = list_first_entry_or_null (&crc_mgr->jobs, struct s805_crc_reqctx, elem);
	
	req->base.complete(&req->base, 0);
	
	if (job)  
		s805_crc_launch_job(job, true);
	else {
		spin_lock(&crc_mgr->lock);
	    crc_mgr->busy = false;
		spin_unlock(&crc_mgr->lock);
	}
}

/* Debug */
static bool crc_map_dst (struct scatterlist * sg, uint len) {

	sg_init_table(sg, 1);
    sg_set_buf(sg, dma_alloc_coherent(NULL,
									  len,
									  &sg_dma_address(sg),
									  GFP_ATOMIC), len);
	
	if (dma_mapping_error(NULL, sg_dma_address(sg))) {
		
		pr_err("%s: Dma allocation failed (%p, 0x%08x).\n", __func__, sg_virt(sg), sg_dma_address(sg));
		return false;
		
	} else
		memset(sg_virt(sg), 0, len);
	
	return true;
	
}

static int s805_crc_add_data (struct ahash_request *req, bool last) {

	struct s805_crc_reqctx *ctx = ahash_request_ctx(req);

	if (!IS_ALIGNED(req->nbytes, S805_CRC_BLOCK_SIZE)) {
		
	    crypto_ahash_set_flags(crypto_ahash_reqtfm(req), CRYPTO_TFM_RES_BAD_BLOCK_LEN);
		return -EINVAL;
		
	}
	
	if (!ctx->initialized) {

		dev_err(crc_mgr->dev, "%s: Uninitialized request.\n", __func__);
		return -ENOSYS;

	}

	if (ctx->finalized) {

		dev_err(crc_mgr->dev, "%s: Already finalized request.\n", __func__);
		return -EINVAL;
		
	}

	/* if (!crc_map_dst(&ctx->dst, sg_dma_len(req->src))) */
	/* 	return -ENOMEM; &ctx->dst */
	
	/* ctx->tx_desc = s805_scatterwalk (req->src, req->src, ctx->init_nfo, ctx->tx_desc, req->nbytes, false); //dst = NULL */

	/* if (!ctx->tx_desc) { */
		
	/* 	dev_err(crc_mgr->dev, "%s: Failed to add data chunk.\n", __func__); */
	/* 	return -ENOMEM; */
	/* } */

	ctx->tx_desc = s805_scatterwalk (req->src, req->src, ctx->init_nfo, ctx->tx_desc, req->nbytes, last); //dst = NULL

	if (!ctx->tx_desc) {
		
		dev_err(crc_mgr->dev, "%s: Failed to add data chunk.\n", __func__);
		return -ENOMEM;
	}
	
	return 0;

}

static int s805_crc_init_ctx (struct ahash_request *req) {

	struct s805_crc_reqctx *ctx = ahash_request_ctx(req);

	if (!IS_ALIGNED(req->nbytes, S805_CRC_BLOCK_SIZE)) {
		
	    crypto_ahash_set_flags(crypto_ahash_reqtfm(req), CRYPTO_TFM_RES_BAD_BLOCK_LEN);
		return -EINVAL;
		
	}
	
	if (ctx->initialized)
		return 1;
	else if (ctx->finalized)
		ctx->finalized = false;
	
	ctx->tx_desc = dmaengine_prep_dma_interrupt (&crc_mgr->chan->vc.chan, S805_DMA_CRYPTO_FLAG | S805_DMA_CRYPTO_CRC_FLAG); //S805_DMA_CRYPTO_FLAG : Won't free the descriptor table !!

	if (!ctx->tx_desc) {
		
		dev_err(crc_mgr->dev, "%s: Failed to allocate dma descriptor.\n", __func__);
		return -ENOMEM;
	}

	ctx->tx_desc->callback = (void *) &s805_crc_handle_completion;
	ctx->tx_desc->callback_param = (void *) req;

	ctx->init_nfo = kzalloc(sizeof(s805_init_desc),
							crypto_ahash_get_flags(crypto_ahash_reqtfm(req)) & CRYPTO_TFM_REQ_MAY_SLEEP
							? GFP_KERNEL
							: GFP_NOWAIT);

	if (!ctx->init_nfo) {
		
	    dev_err(crc_mgr->dev, "%s: Failed to allocate initialization info structure.\n", __func__);
		return -ENOMEM;
	}

	ctx->init_nfo->type = CRC_DESC;
	
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
	
	if (!s805_close_desc (ctx->tx_desc)) {
		
		dev_err(crc_mgr->dev, "%s: Failed to close descriptor.\n", __func__);
		return -ENOSYS;
		
	}
	
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
	.halg.statesize  = sizeof(struct s805_crc_reqctx),
	.halg.base	     = {
		.cra_name		    = "crc-32-hw",
		.cra_driver_name	= "s805-crc-32",
		.cra_priority		= 100,
		.cra_flags		    = CRYPTO_ALG_TYPE_AHASH | CRYPTO_ALG_ASYNC,
		.cra_blocksize		= S805_CRC_BLOCK_SIZE,
		.cra_ctxsize		= 0,
		.cra_alignmask		= S805_CRC_BLOCK_SIZE - 1,
		.cra_module		    = THIS_MODULE,
		.cra_init		    = s805_crc_cra_init,
		.cra_exit		    = s805_crc_cra_exit,
	}
};

static irqreturn_t s805_crc_callback (int irq, void *data)
{

	/* Never got one! */
	
    struct s805_crc_mgr * m = data;

	u32 result = RD(S805_CRC_CHECK_SUM);
	
	dev_warn(m->dev, "%s: %u.\n", __func__, result);
	
	return IRQ_HANDLED;
}

static irqreturn_t s805_parser_callback (int irq, void *data)
{

	/* Must be set ~0U in dest addresses??, is that the same parser ... ¿? */
	
    struct s805_crc_mgr * m = data;

	u32 result = RD(S805_CRC_CHECK_SUM);
	
	dev_warn(m->dev, "%s: %u.\n", __func__, result);
	
	return IRQ_HANDLED;
}

static int s805_crc_hw_enable ( void ) {

	u32 status;// = RD(S805_DMA_CLK);
	WR(/* status | S805_CRC_ENABLE */~0U, S805_DMA_CLK);

	status = RD(S805_CRC_AIU_CLK_GATE);
	WR(status | S805_CRC_ENABLE_CLK, S805_CRC_AIU_CLK_GATE);

	status = RD(S805_CRC_CLK81);
	WR(status | S805_CRC_ENABLE_CLK81, S805_CRC_CLK81);

	status = RD(S805_CRC_IRQ_MASK);
	WR(status | S805_CRC_IRQ_BITS, S805_CRC_IRQ_MASK);
	
	if (request_irq(S805_CRC_PARSER_IRQ, s805_parser_callback, 0, "s805_parser_irq", crc_mgr))
		return -1;
			
	return request_irq(S805_CRC_IRQ, s805_crc_callback, 0, "s805_crc_irq", crc_mgr);
}

static int s805_crc_probe(struct platform_device *pdev)
{

	int err;
	static dma_cap_mask_t mask;
	struct dma_chan * chan;
	
    crc_mgr = kzalloc(sizeof(struct s805_crc_mgr), GFP_KERNEL);
	if (!crc_mgr) {
		dev_err(&pdev->dev, "s805 CRC-32 mgr: Device failed to allocate.\n");
		return -ENOMEM;
	}

    crc_mgr->dev = &pdev->dev;

	if (s805_crc_hw_enable()) {

		dev_err(&pdev->dev, "s805 CRC-32 mgr: Unable to set up hw.\n");
		kfree(crc_mgr);
		return -ENOMEM;

	}
	
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
	free_irq(S805_CRC_IRQ, crc_mgr);
	free_irq(S805_CRC_PARSER_IRQ, crc_mgr);
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
