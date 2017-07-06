#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/s805_dmac.h>
#include <linux/s805_crypto.h>

/* Registers & Bitmaps for the s805 DMAC AES algorithm. */
#define TDES_MIN_BLOCK_SIZE                   8
#define TDES_ECB_KEY_SIZE                     8
#define TDES_CBC_KEY_SIZE                     32

#define S805_TDES_CTRL                        P_NDMA_TDES_CONTROL
#define S805_TDES_KEY_HI                      P_NDMA_TDES_KEY_HI
#define S805_TDES_KEY_LO                      P_NDMA_TDES_KEY_LO

#define S805_DTBL_TDES_POST_ENDIAN(type)      (type & 0x7)
#define S805_DTBL_TDES_CURR_KEY(idx)          ((idx & 0x3) << 3)
#define S805_DTBL_TDES_RESTART(val)           ((val & 0x1) << 6)

#define S805_CTRL_TDES_MODE(mode)             ((mode & 0x3) << 5)
#define S805_CTRL_TDES_DIR(dir)               ({ dir ? (5 << 6 | 1 << 4) : (2 << 6 | 0 << 4); })

#define S805_CTRL_TDES_PUSH_MODE              BIT(30)
#define S805_CTRL_TDES_PUSH_KEY(idx)          (BIT(31) | ((idx & 0x3) << 1))


struct s805_tdes_mgr {

	struct device * dev;
    struct s805_chan * chan;
	struct list_head jobs;
	spinlock_t lock;

	bool busy;
	
};

struct s805_tdes_mgr * tdes_mgr;

struct s805_tdes_ctx {

	uint	keylen;
	u64		key[TDES_CBC_KEY_SIZE / sizeof(u64)];

};

struct s805_tdes_reqctx {

    struct dma_async_tx_descriptor * tx_desc;
	s805_tdes_dir dir;
	s805_tdes_mode mode;
	
	struct list_head elem;
};

static const struct of_device_id s805_tdes_of_match[] =
{
    {.compatible = "aml,amls805-tdes"},
	{},
};

/* Auxiliar function to initialize descriptors. */
static s805_dtable * def_init_tdes_tdesc (unsigned int frames, s805_tdes_mode mode)
{
	
	s805_dtable * desc_tbl = kzalloc(sizeof(s805_dtable), GFP_NOWAIT);
	
	if (!desc_tbl) 
	    return NULL;
	
	desc_tbl->table = dma_pool_alloc(tdes_mgr->chan->pool, GFP_NOWAIT | __GFP_ZERO, &desc_tbl->paddr); /* __GFP_ZERO: Not Working. */
	
	if (!desc_tbl->table) {
		
		kfree(desc_tbl);
		return NULL;
		
	} else
		*desc_tbl->table = (struct s805_table_desc) { 0 };
	
	/* Control common part */
	desc_tbl->table->control |= S805_DTBL_PRE_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->control |= S805_DTBL_INLINE_TYPE(INLINE_TDES);

	if (!((frames + 1) % S805_DMA_MAX_DESC))
		desc_tbl->table->control |= S805_DTBL_IRQ;

	/* Crypto block */
	desc_tbl->table->crypto |= S805_DTBL_TDES_POST_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->crypto |= S805_DTBL_TDES_CURR_KEY(mode); /* To be tested. */
	desc_tbl->table->crypto |= S805_DTBL_TDES_RESTART(mode);
	
	return desc_tbl;
	
}

s805_dtable * sg_tdes_move_along (s805_dtable * cursor, s805_init_desc * init_nfo) {

	if (cursor) {
		list_add_tail(&cursor->elem, &init_nfo->d->desc_list);
 		init_nfo->d->frames ++;
	}
	
	return def_init_tdes_tdesc(init_nfo->d->frames, init_nfo->tdes_mode);
}

static int s805_tdes_cra_init(struct crypto_tfm *tfm)
{
	tfm->crt_ablkcipher.reqsize = sizeof(struct s805_tdes_reqctx);
	
	return 0;
}

static void s805_tdes_cra_exit(struct crypto_tfm *tfm)
{
}

static inline void s805_tdes_set_hw_regs (struct ablkcipher_request *req) {

	struct s805_tdes_ctx *ctx = crypto_ablkcipher_ctx(crypto_ablkcipher_reqtfm(req));
	struct s805_tdes_reqctx *rctx = ablkcipher_request_ctx(req);
	
	uint idx;
	u64 key;
	u32 khi, klow, ctrl;
	uint limit = rctx->mode == TDES_MODE_ECB ? 1 : 4; 
	
	for (idx = 0; idx < limit; idx ++) {
		
		key = ctx->key[idx];
		
		khi  = (key >> 32);  
		klow = (key & ~0U);
		
		WR (khi, S805_TDES_KEY_HI);
		WR (klow, S805_TDES_KEY_LO);
		
		WR (S805_CTRL_TDES_PUSH_KEY(idx), S805_TDES_CTRL); 
	}

	ctrl = S805_CTRL_TDES_MODE(rctx->mode) | S805_CTRL_TDES_DIR(rctx->dir) | S805_CTRL_TDES_PUSH_MODE;
	
	WR(ctrl, S805_TDES_CTRL);
}

static int s805_tdes_ecb_setkey(struct crypto_ablkcipher *tfm, const u8 *key, unsigned int keylen)
{
	struct s805_tdes_ctx *ctx = crypto_ablkcipher_ctx(tfm);

	if (keylen != TDES_ECB_KEY_SIZE) {
		
		dev_err(tdes_mgr->dev, "%s: s805 TDES: Bad keylen (%u) for ecb mode.\n", __func__, keylen);
		return -EINVAL;
	}

	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;

	return 0;
}

static int s805_tdes_cbc_setkey(struct crypto_ablkcipher *tfm, const u8 *key, unsigned int keylen)
{
	struct s805_tdes_ctx *ctx = crypto_ablkcipher_ctx(tfm);

	if (keylen != TDES_CBC_KEY_SIZE) {
		
		dev_err(tdes_mgr->dev, "%s: s805 TDES: Bad keylen (%u) for cbc mode.\n", __func__, keylen);
		return -EINVAL;
	}

	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;
	
	return 0;
}

static int s805_tdes_crypt_launch_job (struct ablkcipher_request *req, bool chain) {

	struct s805_tdes_reqctx *rctx = ablkcipher_request_ctx(req);
	dma_cookie_t tx_cookie;

	spin_lock(&tdes_mgr->lock);
	if (!tdes_mgr->busy || chain) {
		tdes_mgr->busy = true;
		spin_unlock(&tdes_mgr->lock);
		
		s805_tdes_set_hw_regs (req);
	
		tx_cookie = dmaengine_submit(rctx->tx_desc);
	
		if(tx_cookie < 0) {
		
			dev_err(tdes_mgr->dev, "%s: Failed to get cookie.\n", __func__);
			return tx_cookie;
		
		}

		dma_async_issue_pending(&tdes_mgr->chan->vc.chan);

		return 0;
		
	} else
		spin_unlock(&tdes_mgr->lock);
	
	return 1;
}

static void s805_tdes_crypt_handle_completion (void * req_ptr) {
	
	struct ablkcipher_request *req = req_ptr;
	struct s805_tdes_reqctx *job = ablkcipher_request_ctx(req);
	
	req->base.complete(&req->base, 0);
	
	spin_lock(&tdes_mgr->lock);
	list_del(&job->elem);
	spin_unlock(&tdes_mgr->lock);

	job = list_first_entry_or_null (&tdes_mgr->jobs, struct s805_tdes_reqctx, elem);

	if (job)  
		s805_tdes_crypt_launch_job(to_ablkcipher_request(job), true);
	else {
		spin_lock(&tdes_mgr->lock);
		tdes_mgr->busy = false;
		spin_unlock(&tdes_mgr->lock);
	}
}

static int s805_tdes_crypt_schedule_job (struct ablkcipher_request *req) {

	struct s805_tdes_reqctx *rctx = ablkcipher_request_ctx(req);
	   
	spin_lock(&tdes_mgr->lock);
		
	list_add_tail(&rctx->elem, &tdes_mgr->jobs);
	
	spin_unlock(&tdes_mgr->lock);

	return s805_tdes_crypt_launch_job(req, false);
}

static int s805_tdes_crypt_prep (struct ablkcipher_request *req, s805_tdes_mode mode, s805_tdes_dir dir) {
	
	struct s805_tdes_reqctx *rctx = ablkcipher_request_ctx(req);
	struct scatterlist * aux;
	s805_init_desc * init_nfo;
	int len, j = 0;
	
	
	/* Allocate and setup the information for descriptor initialization */
	init_nfo = kzalloc(sizeof(struct s805_desc), GFP_NOWAIT); /* May we do this with GFP_KERNEL?? */

	if (!init_nfo) {
	    dev_err(tdes_mgr->dev, "%s: Failed to allocate initialization info structure.\n", __func__);
		return -ENOMEM;
	}
	
	init_nfo->type = TDES_DESC;
	init_nfo->tdes_mode = mode;
	
	aux = req->src;
	
	while (aux) {

		len = sg_dma_len(aux);

		/* Needed here ?? */
		if (!IS_ALIGNED(len, TDES_MIN_BLOCK_SIZE)) {
		    dev_err(tdes_mgr->dev, "%s: Block %d of src sg list is not aligned with TDES_BLOCK_SIZE (%u Bytes).\n", __func__, j, TDES_MIN_BLOCK_SIZE);
			kfree(init_nfo);
			return -EINVAL;
		}
		
		aux = sg_next(aux);
		j ++;
	}

	rctx->tx_desc = dmaengine_prep_dma_interrupt (&tdes_mgr->chan->vc.chan, 0);

	if (!rctx->tx_desc) {
		
		dev_err(tdes_mgr->dev, "%s: Failed to allocate dma descriptor.\n", __func__);
		kfree(init_nfo);
		return -ENOMEM;
	}

	rctx->tx_desc = s805_scatterwalk (req->src, req->dst, init_nfo, rctx->tx_desc, true);

	if (!rctx->tx_desc) {
		
		dev_err(tdes_mgr->dev, "%s: Failed to allocate dma descriptors.\n", __func__);
		kfree(init_nfo);
		return -ENOMEM;
		
	}
	
	kfree(init_nfo);
	
	rctx->tx_desc->callback = (void *) &s805_tdes_crypt_handle_completion;
	rctx->tx_desc->callback_param = (void *) req;
	rctx->dir = dir;
	rctx->mode = mode;
	
	return s805_tdes_crypt_schedule_job (req);
}

static int s805_tdes_ecb_encrypt(struct ablkcipher_request *req) {

    return s805_tdes_crypt_prep (req, TDES_MODE_ECB, TDES_DIR_ENCRYPT);

}

static int s805_tdes_ecb_decrypt(struct ablkcipher_request *req) {

	return s805_tdes_crypt_prep (req, TDES_MODE_ECB, TDES_DIR_DECRYPT);
	
}

static int s805_tdes_cbc_encrypt(struct ablkcipher_request *req) {

	return s805_tdes_crypt_prep (req, TDES_MODE_CBC, TDES_DIR_ENCRYPT);

}

static int s805_tdes_cbc_decrypt(struct ablkcipher_request *req) {

	return s805_tdes_crypt_prep (req, TDES_MODE_CBC, TDES_DIR_DECRYPT);

}

static struct crypto_alg s805_tdes_algs[] = {
{
	.cra_name		    = "ecb(tdes)",
	.cra_driver_name	= "s805-ecb-tdes",
	.cra_priority		= 300,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= TDES_MIN_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_tdes_ctx),
	.cra_alignmask		= 0,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_tdes_cra_init,
	.cra_exit		    = s805_tdes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = TDES_ECB_KEY_SIZE,
		.max_keysize	     = TDES_ECB_KEY_SIZE,
		.setkey		         = s805_tdes_ecb_setkey,
		.encrypt	         = s805_tdes_ecb_encrypt,
		.decrypt	         = s805_tdes_ecb_decrypt,
	}
},
{
	.cra_name		    = "cbc(tdes)",
	.cra_driver_name	= "s805-cbc-aes",
	.cra_priority		= 300,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= TDES_MIN_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_tdes_ctx),
	.cra_alignmask		= 0,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_tdes_cra_init,
	.cra_exit		    = s805_tdes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = TDES_CBC_KEY_SIZE,
		.max_keysize	     = TDES_CBC_KEY_SIZE,
		.setkey		         = s805_tdes_cbc_setkey,
		.encrypt	         = s805_tdes_cbc_encrypt,
		.decrypt	         = s805_tdes_cbc_decrypt,
	}
}
};

static int s805_tdes_register_algs ( void )
{
	int err, i, j;
	
	for (i = 0; i < ARRAY_SIZE(s805_tdes_algs); i++) {
		err = crypto_register_alg(&s805_tdes_algs[i]);
		if (err)
			goto err_tdes_algs;
	}

	return 0;

 err_tdes_algs:
	for (j = 0; j < i; j++)
		crypto_unregister_alg(&s805_tdes_algs[j]);
	
	return err;
}

static int s805_tdes_probe(struct platform_device *pdev)
{

	int err;
	static dma_cap_mask_t mask;
	struct dma_chan * chan;
	
    tdes_mgr = kzalloc(sizeof(struct s805_tdes_mgr), GFP_KERNEL);
	if (!tdes_mgr) {
		dev_err(&pdev->dev, "s805 TDES mgr device failed to allocate.\n");
		return -ENOMEM;
	}

    tdes_mgr->dev = &pdev->dev;

	INIT_LIST_HEAD(&tdes_mgr->jobs);
	spin_lock_init(&tdes_mgr->lock);
	
	err = s805_tdes_register_algs();
	
	if (err) {
		
		dev_err(tdes_mgr->dev, "s805 TDES: failed to register algorithms.\n");
		kfree(tdes_mgr);
		return err;
	}

	dma_cap_zero(mask);
	dma_cap_set(DMA_INTERRUPT, mask);
	
    chan = dma_request_channel ( mask, NULL, NULL );
	
	if (!chan) {
		
		dev_err(tdes_mgr->dev, "s805 TDES: failed to get dma channel.\n");
		kfree(tdes_mgr);
		return -ENOSYS;
		
	} else {
		
		dev_info(tdes_mgr->dev, "s805 TDES: grabbed dma channel (%s).\n", dma_chan_name(chan));
		tdes_mgr->chan = to_s805_dma_chan(chan);
	}
	
    dev_info(tdes_mgr->dev, "Loaded S805 TDES crypto driver\n");

	return 0;
}

static int s805_tdes_remove(struct platform_device *pdev)
{

	int i, err, ret = 0;
	
	for (i = 0; i < ARRAY_SIZE(s805_tdes_algs); i++) {
		err = crypto_unregister_alg(&s805_tdes_algs[i]);
		if (err) {
		    dev_err(tdes_mgr->dev, "s805 TDES: Error unregistering algorithms.\n");
			ret = err;
		}
	}
	
	dma_release_channel ( &tdes_mgr->chan->vc.chan );
	kfree(tdes_mgr);
	
	return ret;
}

static struct platform_driver s805_tdes_driver = {
	.probe		= s805_tdes_probe,
	.remove		= s805_tdes_remove,
	.driver		= {
		.name = "s805-dmac-tdes",
		.owner	= THIS_MODULE,
		.of_match_table = s805_tdes_of_match
	},
};

module_platform_driver(s805_tdes_driver);

MODULE_ALIAS("platform:s805-tdes");
MODULE_DESCRIPTION("s805 TDES hw acceleration support.");
MODULE_AUTHOR("szz-dvl");
MODULE_LICENSE("GPL v2");
