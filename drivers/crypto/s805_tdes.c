#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/s805_dmac.h>
#include <linux/dma-mapping.h>
#include <crypto/des.h>
#include "s805_crypto.h"

/* Registers & Bitmaps for the s805 DMAC TDES algorithm. */
#define TDES_KEY_SIZE                         DES3_EDE_KEY_SIZE
#define DDES_KEY_SIZE                         DES_KEY_SIZE * 2

#define S805_TDES_CTRL                        P_NDMA_TDES_CONTROL
#define S805_TDES_KEY_HI                      P_NDMA_TDES_KEY_HI
#define S805_TDES_KEY_LO                      P_NDMA_TDES_KEY_LO

#define S805_DTBL_TDES_POST_ENDIAN(type)      (type & 0x7)
#define S805_DTBL_TDES_CURR_KEY(idx)          ((idx & 0x3) << 3)
#define S805_DTBL_TDES_RESTART(val)           ((val & 0x1) << 6)

#define S805_CTRL_TDES_MODE(mode)             ((mode & 0x3) << 5)
#define S805_CTRL_TDES_DIR(dir)               ({ dir ? (5 << 6 | 1 << 4) : (2 << 6 | 0 << 4); })

#define S805_CTRL_TDES_PUSH_MODE              BIT(30)
#define S805_CTRL_TDES_PUSH_KEY(idx)          (BIT(31) | ((idx & 0x3) << 0)) /* Datasheet mistaken here, info token from crypto module. */

typedef enum tdes_dir {
	TDES_DIR_ENCRYPT,
    TDES_DIR_DECRYPT
} s805_tdes_dir;

typedef enum des_type {
    DES_SIMPLE,
	DES_MULTI
} s805_des_type;

typedef enum tdes_mode {
	TDES_MODE_ECB,
    TDES_MODE_CBC
} s805_tdes_mode;

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
	u64		key[TDES_KEY_SIZE / sizeof(u64)];

};

struct s805_tdes_reqctx {

	struct dma_async_tx_descriptor * tx_desc;
	
	s805_tdes_dir dir;
	s805_tdes_mode mode;
	s805_des_type type;
	
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
	desc_tbl->table->crypto |= S805_DTBL_TDES_CURR_KEY(0);

	/* See note for CBC chainig pipeline reset at: s805_aes.c, */
	desc_tbl->table->crypto |= S805_DTBL_TDES_RESTART(mode ? !frames : 0);
	
	return desc_tbl;
	
}

s805_dtable * sg_tdes_move_along (struct s805_desc * d, s805_dtable * cursor) {

	struct s805_tdes_reqctx *rctx = ablkcipher_request_ctx((struct ablkcipher_request *)d->req);
	
	if (cursor) {
		
		list_add_tail(&cursor->elem, &d->desc_list);
 	    d->frames ++;
	}
	
	return def_init_tdes_tdesc(d->frames, rctx->mode);
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
	
	u64 key;
	u32 khi, klow;
	uint idx, limit = TDES_KEY_SIZE / sizeof(u64);
	
	for (idx = 0; idx < limit; idx ++) {
		
		key = rctx->type ? ctx->key[idx] : ctx->key[0];

		klow = (key & ~0U);
		khi  = (key >> 32);  
		
		WR (khi, S805_TDES_KEY_HI);
		WR (klow, S805_TDES_KEY_LO);
		
		WR (S805_CTRL_TDES_PUSH_KEY(rctx->dir ? (limit - 1) - idx : idx), S805_TDES_CTRL);
	}
	
	WR(S805_CTRL_TDES_MODE(rctx->mode) | S805_CTRL_TDES_DIR(rctx->dir) | S805_CTRL_TDES_PUSH_MODE, S805_TDES_CTRL);
}

static int s805_tdes_setkey(struct crypto_ablkcipher *tfm, const u8 *key, unsigned int keylen)
{
	struct s805_tdes_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	u64 * kcomp = (u64 *) key;
	uint i, j;
	bool bad_key = false;

	/* Wrong key sizes filtered out by interface. */

	for (i = 0; i < keylen / sizeof(u64) && !bad_key; i++ ) {

		for (j = i + 1; j < keylen / sizeof(u64) && !bad_key; j++ ) {

			if (kcomp[i] == kcomp[j])
			    bad_key = true;
		}
	}
	
	if (bad_key) {
		
		/* 	
		    The driver won't allow repeated key components for TDES, since we will be 
			performing either a DES or a double DES transform. 
			
		*/
		
		crypto_ablkcipher_set_flags(tfm, CRYPTO_TFM_RES_BAD_KEY_SCHED);
		return -EINVAL;
	}

	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;

	return 0;
}

static int s805_ddes_setkey(struct crypto_ablkcipher *tfm, const u8 *key, unsigned int keylen)
{

	struct s805_tdes_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	u64 * kcomp = (u64 *) key;
	
	/* Wrong key sizes filtered out by interface. */
	
	if (kcomp[0] == kcomp[1]) {
		
		crypto_ablkcipher_set_flags(tfm, CRYPTO_TFM_RES_BAD_KEY_SCHED);
		return -EINVAL;
	}

	/* Under driver point of view this is, actually, a particular case of TDES where K3 == K1 while K1 != K2. */
	memcpy(ctx->key, key, keylen);
	memcpy(&ctx->key[2], key, keylen/2);
	
	ctx->keylen = keylen;

	return 0;
}

static int s805_des_setkey(struct crypto_ablkcipher *tfm, const u8 *key, unsigned int keylen)
{
	struct s805_tdes_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	
	/* Wrong key sizes filtered out by interface. */
	
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
	
	spin_lock(&tdes_mgr->lock);
	list_del(&job->elem);
	spin_unlock(&tdes_mgr->lock);

	/* Must we run this in a thread ?? */
	req->base.complete(&req->base, 0);
	
	job = list_first_entry_or_null (&tdes_mgr->jobs, struct s805_tdes_reqctx, elem);
	
	if (job) {
		s805_tdes_crypt_launch_job(to_ablkcipher_request(job), true);
	} else {
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

static int s805_tdes_crypt_prep (struct ablkcipher_request *req, s805_tdes_mode mode, s805_tdes_dir dir, s805_des_type type) {
	
	struct s805_tdes_reqctx *rctx = ablkcipher_request_ctx(req);
	struct scatterlist * aux;
	int len, j = 0;
	
	if (!IS_ALIGNED(req->nbytes, DES_BLOCK_SIZE)) {
		
	    crypto_ablkcipher_set_flags(crypto_ablkcipher_reqtfm(req), CRYPTO_TFM_RES_BAD_BLOCK_LEN);
		return -EINVAL;
	}
		
	rctx->dir = dir;
	rctx->mode = mode;
	rctx->type = type;
	
	aux = req->src;
	
	while (aux) {

		len = sg_dma_len(aux);

		if (!IS_ALIGNED(len, DES_BLOCK_SIZE)) {
			
		    crypto_ablkcipher_set_flags(crypto_ablkcipher_reqtfm(req), CRYPTO_TFM_RES_BAD_BLOCK_LEN);
			return -EINVAL;
		}
		
		aux = sg_next(aux);
		j ++;
	}

	rctx->tx_desc = dmaengine_prep_dma_interrupt (&tdes_mgr->chan->vc.chan, S805_DMA_CRYPTO_FLAG | S805_DMA_CRYPTO_TDES_FLAG);
	
	if (!rctx->tx_desc) {
		
		dev_err(tdes_mgr->dev, "%s: Failed to allocate dma descriptor.\n", __func__);
		return -ENOMEM;
	}
	
	s805_crypto_set_req(rctx->tx_desc, req);
	rctx->tx_desc = s805_scatterwalk (req->src, req->dst, rctx->tx_desc, req->nbytes, true);
	
	if (!rctx->tx_desc) {
		
		dev_err(tdes_mgr->dev, "%s: Failed to allocate data chunks.\n", __func__);
		return -ENOMEM;
		
	}
	
	rctx->tx_desc->callback = (void *) &s805_tdes_crypt_handle_completion;
	rctx->tx_desc->callback_param = (void *) req;
	
	return s805_tdes_crypt_schedule_job (req);
}

static int s805_tdes_ecb_encrypt(struct ablkcipher_request *req) {

    return s805_tdes_crypt_prep (req, TDES_MODE_ECB, TDES_DIR_ENCRYPT, DES_MULTI);

}

static int s805_tdes_ecb_decrypt(struct ablkcipher_request *req) {

	return s805_tdes_crypt_prep (req, TDES_MODE_ECB, TDES_DIR_DECRYPT, DES_MULTI);
	
}

static int s805_tdes_cbc_encrypt(struct ablkcipher_request *req) {

	return s805_tdes_crypt_prep (req, TDES_MODE_CBC, TDES_DIR_ENCRYPT, DES_MULTI);

}

static int s805_tdes_cbc_decrypt(struct ablkcipher_request *req) {

	return s805_tdes_crypt_prep (req, TDES_MODE_CBC, TDES_DIR_DECRYPT, DES_MULTI);

}

static int s805_des_ecb_encrypt(struct ablkcipher_request *req) {

    return s805_tdes_crypt_prep (req, TDES_MODE_ECB, TDES_DIR_ENCRYPT, DES_SIMPLE);

}

static int s805_des_ecb_decrypt(struct ablkcipher_request *req) {

	return s805_tdes_crypt_prep (req, TDES_MODE_ECB, TDES_DIR_DECRYPT, DES_SIMPLE);
	
}

static int s805_des_cbc_encrypt(struct ablkcipher_request *req) {

	return s805_tdes_crypt_prep (req, TDES_MODE_CBC, TDES_DIR_ENCRYPT, DES_SIMPLE);

}

static int s805_des_cbc_decrypt(struct ablkcipher_request *req) {

	return s805_tdes_crypt_prep (req, TDES_MODE_CBC, TDES_DIR_DECRYPT, DES_SIMPLE);

}

static struct crypto_alg s805_tdes_algs[] = {
{
	.cra_name		    = "ecb(des)-hw",
	.cra_driver_name	= "s805-ecb-des",
	.cra_priority		= 300,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= DES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_tdes_ctx),
	.cra_alignmask		= DES_BLOCK_SIZE - 1,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_tdes_cra_init,
	.cra_exit		    = s805_tdes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = DES_KEY_SIZE,
		.max_keysize	     = DES_KEY_SIZE,
		.setkey		         = s805_des_setkey,
		.encrypt	         = s805_des_ecb_encrypt,
		.decrypt	         = s805_des_ecb_decrypt,
	}
},
{
	.cra_name		    = "cbc(des)-hw",
	.cra_driver_name	= "s805-cbc-des",
	.cra_priority		= 300,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= DES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_tdes_ctx),
	.cra_alignmask		= DES_BLOCK_SIZE - 1,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_tdes_cra_init,
	.cra_exit		    = s805_tdes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = DES_KEY_SIZE,
		.max_keysize	     = DES_KEY_SIZE,
		.setkey		         = s805_des_setkey,
		.encrypt	         = s805_des_cbc_encrypt,
	    .decrypt	         = s805_des_cbc_decrypt,
	}
},
{
	.cra_name		    = "ecb(ddes)-hw",
	.cra_driver_name	= "s805-ecb-ddes",
	.cra_priority		= 300,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= DES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_tdes_ctx),
	.cra_alignmask		= DES_BLOCK_SIZE - 1,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_tdes_cra_init,
	.cra_exit		    = s805_tdes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = DDES_KEY_SIZE,
		.max_keysize	     = DDES_KEY_SIZE,
		.setkey		         = s805_ddes_setkey,
		.encrypt	         = s805_tdes_ecb_encrypt,
		.decrypt	         = s805_tdes_ecb_decrypt,
	}
},
{
	.cra_name		    = "cbc(ddes)-hw",
	.cra_driver_name	= "s805-cbc-ddes",
	.cra_priority		= 300,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= DES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_tdes_ctx),
	.cra_alignmask		= DES_BLOCK_SIZE - 1,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_tdes_cra_init,
	.cra_exit		    = s805_tdes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = DDES_KEY_SIZE,
		.max_keysize	     = DDES_KEY_SIZE,
		.setkey		         = s805_ddes_setkey,
		.encrypt	         = s805_tdes_cbc_encrypt,
	    .decrypt	         = s805_tdes_cbc_decrypt,
	}
},
{
	.cra_name		    = "ecb(tdes)-hw",
	.cra_driver_name	= "s805-ecb-tdes",
	.cra_priority		= 300,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= DES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_tdes_ctx),
	.cra_alignmask		= DES_BLOCK_SIZE - 1,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_tdes_cra_init,
	.cra_exit		    = s805_tdes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = TDES_KEY_SIZE,
		.max_keysize	     = TDES_KEY_SIZE,
		.setkey		         = s805_tdes_setkey,
		.encrypt	         = s805_tdes_ecb_encrypt,
		.decrypt	         = s805_tdes_ecb_decrypt,
	}
},
{
	.cra_name		    = "cbc(tdes)-hw",
	.cra_driver_name	= "s805-cbc-tdes",
	.cra_priority		= 300,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= DES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_tdes_ctx),
	.cra_alignmask		= DES_BLOCK_SIZE - 1,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_tdes_cra_init,
	.cra_exit		    = s805_tdes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = TDES_KEY_SIZE,
		.max_keysize	     = TDES_KEY_SIZE,
		.setkey		         = s805_tdes_setkey,
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
