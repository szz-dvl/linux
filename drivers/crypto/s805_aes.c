#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/random.h>
#include <crypto/aes.h>
#include <crypto/skcipher.h>
#include <linux/s805_dmac.h>
#include <linux/s805_crypto.h>

/* Registers & Bitmaps for the s805 DMAC AES algorithm. */

#define S805_AES_KEY_0                        P_NDMA_AES_KEY_0
#define S805_AES_KEY_1                        P_NDMA_AES_KEY_1
#define S805_AES_KEY_2                        P_NDMA_AES_KEY_2
#define S805_AES_KEY_3                        P_NDMA_AES_KEY_3
#define S805_AES_KEY_4                        P_NDMA_AES_KEY_4
#define S805_AES_KEY_5                        P_NDMA_AES_KEY_5
#define S805_AES_KEY_6                        P_NDMA_AES_KEY_6
#define S805_AES_KEY_7                        P_NDMA_AES_KEY_7

#define S805_AES_IV_0                         P_NDMA_AES_IV_0
#define S805_AES_IV_1                         P_NDMA_AES_IV_1
#define S805_AES_IV_2                         P_NDMA_AES_IV_2
#define S805_AES_IV_3                         P_NDMA_AES_IV_3

#define S805_DTBL_AES_POST_ENDIAN(type)       ((type & 0xf) << 4)
#define S805_DTBL_AES_PRE_ENDIAN(type)        (type & 0xf)
#define S805_DTBL_AES_KEY_TYPE(type)          ((type & 0x3) << 8)
#define S805_DTBL_AES_DIR(dir)                ((dir & 0x1) << 10)
#define S805_DTBL_AES_RESET_IV(restr)         ((restr & 0x1) << 11)
#define S805_DTBL_AES_MODE(mode)              ((mode & 0x3) << 12)

struct s805_aes_mgr {

	struct device * dev;
    struct s805_chan * chan;
	struct list_head jobs;
	spinlock_t lock;
	
	bool busy;
};

struct s805_aes_mgr * aes_mgr;

struct s805_aes_ctx {

	uint	keylen;
	u32		key[AES_MAX_KEYLENGTH_U32];

	uint pending;
	spinlock_t lock;
};

struct s805_aes_reqctx {

    struct dma_async_tx_descriptor * tx_desc;
	s805_aes_mode mode;
	/* s805_aes_dir dir; */
	
	struct list_head elem;
};

static const struct of_device_id s805_aes_of_match[] =
{
    {.compatible = "aml,amls805-aes"},
    {},
};

/* Auxiliar function to initialize descriptors. */
static s805_dtable * def_init_aes_tdesc (unsigned int frames, s805_aes_key_type type, s805_aes_mode mode, s805_aes_dir dir)
{

	s805_dtable * desc_tbl = kzalloc(sizeof(s805_dtable), GFP_NOWAIT);
	
	if (!desc_tbl) 
	    return NULL;

	desc_tbl->table = dma_pool_alloc(aes_mgr->chan->pool, GFP_NOWAIT | __GFP_ZERO, &desc_tbl->paddr); /* __GFP_ZERO: Not Working. */
	
	if (!desc_tbl->table) {
		
		kfree(desc_tbl);
		return NULL;
		
	} else
		*desc_tbl->table = (struct s805_table_desc) { 0 };

	/* Control common part */
	desc_tbl->table->control |= S805_DTBL_PRE_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->control |= S805_DTBL_INLINE_TYPE(INLINE_AES);

	if (!((frames + 1) % S805_DMA_MAX_DESC))
		desc_tbl->table->control |= S805_DTBL_IRQ;

	/* Crypto block */
	desc_tbl->table->crypto |= S805_DTBL_AES_POST_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->crypto |= S805_DTBL_AES_PRE_ENDIAN(ENDIAN_NO_CHANGE); /* mode == AES_MODE_CTR && !dir ? ENDIAN_TYPE_7 : ENDIAN_NO_CHANGE*/
	desc_tbl->table->crypto |= S805_DTBL_AES_KEY_TYPE(type);
	desc_tbl->table->crypto |= S805_DTBL_AES_DIR(dir);

	/* 
	   The driver will reset the CBC chaining pipeline ONLY for the first frame of the data chunk, 
	   so for CBC mode all the data gathered in one request will be dependent of the rest of the data
	   in the request, meaning that, in a ddition to key an IVs, no chunk for this requests will be 
	   decrypable without the rest of the chunks of the same request. This may generate some unwanted data 
	   dependencies, so developer must be aware of that, and, join in a request only data that is expected
	   to be decrypted at the same time too. If the latter condition is satisfied, this must increase encryption 
	   secureness for CBC modes. Same thing will apply for DES variants CBC modes.
	*/
	desc_tbl->table->crypto |= S805_DTBL_AES_RESET_IV(mode ? !frames : 0); /* mode ? 1 : 0 (mode == AES_MODE_CBC) */
	desc_tbl->table->crypto |= S805_DTBL_AES_MODE(mode);

	/* CTR limit, CTR Endian: Untested! */
	
	return desc_tbl;
	
}

s805_dtable * sg_aes_move_along (s805_dtable * cursor, s805_init_desc * init_nfo) {

	if (cursor) {
		list_add_tail(&cursor->elem, &init_nfo->d->desc_list);
 		init_nfo->d->frames ++;
	}
	
	return def_init_aes_tdesc(init_nfo->d->frames, init_nfo->aes_nfo.type, init_nfo->aes_nfo.mode, init_nfo->aes_nfo.dir);
	
}

static int s805_aes_cra_init(struct crypto_tfm *tfm)
{

	struct s805_aes_ctx *ctx = (struct s805_aes_ctx *) tfm->__crt_ctx;

	spin_lock_init(&ctx->lock);
	ctx->pending = 0;

	tfm->crt_ablkcipher.reqsize = sizeof(struct s805_aes_reqctx);
	
	return 0;
}

static void s805_aes_cra_exit(struct crypto_tfm *tfm)
{
}


static void s805_aes_rndiv_gen (struct skcipher_givcrypt_request * req, unsigned int ivsize)
{
	
	get_random_bytes_arch (req->giv, ivsize);
	
}

static void s805_aes_seqiv_gen (struct skcipher_givcrypt_request * req, unsigned int ivsize)
{
	struct s805_aes_ctx *ctx = crypto_ablkcipher_ctx(crypto_ablkcipher_reqtfm(&req->creq));
	u64 seq;
	
	get_random_bytes_arch (&seq, sizeof(u64));
	
	memset(req->giv, 0, ivsize - sizeof(u64));
	
	seq = cpu_to_be64(seq); /* Randomly generated, dosn't have much sense to me ... */
	memcpy(req->giv + ivsize - sizeof(u64), &seq, sizeof(u64));
	crypto_xor(req->giv, (const u8 *)ctx->key, ivsize);

	/* memcpy(req->giv, ctx->key, ctx->keylen); */
	/* if (ivsize < ctx->keylen) */
	/* 	get_random_bytes_arch (req->giv + ctx->keylen, ivsize - ctx->keylen); */
	
}


static int s805_aes_iv_gen (struct skcipher_givcrypt_request * req, s805_aes_mode mode /* s805_aes_dir dir */) {

	/*
	  Hints:
	  
	   * http://elixir.free-electrons.com/linux/v3.10.104/source/crypto/chainiv.c
	   * http://elixir.free-electrons.com/linux/v3.10.104/source/crypto/eseqiv.c
	   * http://elixir.free-electrons.com/linux/v3.10.104/source/crypto/seqiv.c
	   
	*/

	uint i, sum = 0;
	unsigned int ivsize = crypto_ablkcipher_ivsize(crypto_ablkcipher_reqtfm(&req->creq));
	
	if (!req->giv) {

		dev_err(aes_mgr->dev, "%s: No memory for IV generation, aborting.\n", __func__);
		return -ENOMEM;	
	}

	for (i = 0; i < (ivsize / sizeof(u32)) && !sum; i++)
		sum += req->giv[i];

	if (!sum) {

		if (mode == AES_MODE_CBC)
			s805_aes_rndiv_gen(req, ivsize);
		else /* CTR */
			s805_aes_seqiv_gen(req, ivsize);
	} 
	
	return 0;
	
}

static inline void s805_aes_cpyiv_to_hw (struct skcipher_givcrypt_request * req /* ,rctx->mode, rctx->dir */) {

	u32 * aux = (u32 *) req->giv;
	
	/* WR(cpu_to_be32(aux[0]), S805_AES_IV_3); */
	/* WR(cpu_to_be32(aux[1]), S805_AES_IV_2); */
	/* WR(cpu_to_be32(aux[2]), S805_AES_IV_1); */
	/* WR(cpu_to_be32(aux[3]), S805_AES_IV_0); */
		

	WR(aux[0], S805_AES_IV_0);
	WR(aux[1], S805_AES_IV_1);
	WR(aux[2], S805_AES_IV_2);
	WR(aux[3], S805_AES_IV_3);

}

static inline void s805_aes_cpykey_to_hw (const u32 * key, unsigned int keylen) {
	
	WR(key[0], S805_AES_KEY_0);
	WR(key[1], S805_AES_KEY_1);
	WR(key[2], S805_AES_KEY_2);
	WR(key[3], S805_AES_KEY_3);
	
	if (keylen >= AES_KEYSIZE_192) {
		
		WR(key[4], S805_AES_KEY_4);
		WR(key[5], S805_AES_KEY_5);
		
	}

	if (keylen >= AES_KEYSIZE_256) {
		
		WR(key[6], S805_AES_KEY_6);
		WR(key[7], S805_AES_KEY_7);
		
	}
}

static int s805_aes_setkey(struct crypto_ablkcipher *tfm, const u8 *key,
						   unsigned int keylen)
{
	struct s805_aes_ctx *ctx = crypto_ablkcipher_ctx(tfm);

	spin_lock(&ctx->lock);
	if (!ctx->pending) {
		spin_unlock(&ctx->lock);
		
		switch (keylen) {
		case AES_KEYSIZE_128:
		case AES_KEYSIZE_192:
		case AES_KEYSIZE_256:
			memcpy(ctx->key, key, keylen);
			ctx->keylen = keylen;
			return 0;
		default:
			crypto_ablkcipher_set_flags(tfm, CRYPTO_TFM_RES_BAD_KEY_LEN);
			return -EINVAL;
		}
		
	} else
		spin_unlock(&ctx->lock);
	
	dev_err(aes_mgr->dev, "%s: s805 AES engine is busy, please wait till all the pending jobs (%u) finish.\n", __func__, ctx->pending);
	return -ENOSYS;
}

static int s805_aes_crypt_launch_job (struct ablkcipher_request *req, bool chain) {

	struct s805_aes_ctx *ctx = crypto_ablkcipher_ctx(crypto_ablkcipher_reqtfm(req));
	struct s805_aes_reqctx *rctx = ablkcipher_request_ctx(req);
	dma_cookie_t tx_cookie;
	
	spin_lock(&aes_mgr->lock);
	if (!aes_mgr->busy || chain) {
		aes_mgr->busy = true;
		spin_unlock(&aes_mgr->lock);
	
		s805_aes_cpykey_to_hw ((const u32 *) ctx->key, ctx->keylen);

		if (rctx->mode)
		    s805_aes_cpyiv_to_hw(skcipher_givcrypt_cast(&req->base) /* ,rctx->mode, rctx->dir */);
		
		tx_cookie = dmaengine_submit(rctx->tx_desc);
		
		if(tx_cookie < 0) {
		
			dev_err(aes_mgr->dev, "%s: Failed to get DMA cookie.\n", __func__);
			return tx_cookie;
			
		}
		
		dma_async_issue_pending(&aes_mgr->chan->vc.chan);
		return 0;
		
	} else
		spin_unlock(&aes_mgr->lock);
	
	return 1;
	
}

static void s805_aes_crypt_handle_completion (void * req_ptr) {
	
	struct ablkcipher_request *req = req_ptr;
	struct s805_aes_ctx *ctx = crypto_ablkcipher_ctx(crypto_ablkcipher_reqtfm(req));
	struct s805_aes_reqctx *job = ablkcipher_request_ctx(req);

	spin_lock(&ctx->lock);
	ctx->pending --;
	spin_unlock(&ctx->lock);

	spin_lock(&aes_mgr->lock);
	list_del(&job->elem);
	spin_unlock(&aes_mgr->lock);

	job = list_first_entry_or_null (&aes_mgr->jobs, struct s805_aes_reqctx, elem);
	
	req->base.complete(&req->base, 0);
    
	if (job)  
		s805_aes_crypt_launch_job(to_ablkcipher_request(job), true);
	else {
		spin_lock(&aes_mgr->lock);
		aes_mgr->busy = false;
		spin_unlock(&aes_mgr->lock);
	}
}

static int s805_aes_crypt_schedule_job (struct ablkcipher_request *req) {

	struct s805_aes_ctx *ctx = crypto_ablkcipher_ctx(crypto_ablkcipher_reqtfm(req));
	struct s805_aes_reqctx *rctx = ablkcipher_request_ctx(req);
	
	spin_lock(&ctx->lock);
	ctx->pending ++;
	spin_unlock(&ctx->lock);
  	
	spin_lock(&aes_mgr->lock);
	list_add_tail(&rctx->elem, &aes_mgr->jobs);
	spin_unlock(&aes_mgr->lock);
		
	return s805_aes_crypt_launch_job(req, false);
}

static int s805_aes_crypt_get_key_type (uint keylen) {
	
	switch (keylen) {
	case AES_KEYSIZE_128:
		return 0;
	case AES_KEYSIZE_192:
		return 1;
	case AES_KEYSIZE_256:
		return 2;
	default:
		return -EINVAL;
	}
}

static int s805_aes_crypt_prep (struct ablkcipher_request * req, s805_aes_mode mode, s805_aes_dir dir) {

	struct s805_aes_ctx *ctx = crypto_ablkcipher_ctx(crypto_ablkcipher_reqtfm(req));
	struct s805_aes_reqctx *rctx = ablkcipher_request_ctx(req);
	struct scatterlist * aux;
	s805_init_desc * init_nfo;
	int keytype;
	int len, ret, j = 0;

	if (mode) {

		ret = s805_aes_iv_gen (skcipher_givcrypt_cast(&req->base), mode /* dir */);
		
		if (ret) 
			return ret;
			
	}

	if (!IS_ALIGNED(req->nbytes, AES_BLOCK_SIZE)) {

	    crypto_ablkcipher_set_flags(crypto_ablkcipher_reqtfm(req), CRYPTO_TFM_RES_BAD_BLOCK_LEN);
		return -EINVAL;
	}
	
	/* Allocate and setup the information for descriptor initialization */
	init_nfo = kzalloc(sizeof(s805_init_desc),
					   crypto_ablkcipher_get_flags(crypto_ablkcipher_reqtfm(req)) & CRYPTO_TFM_REQ_MAY_SLEEP
					   ? GFP_KERNEL
					   : GFP_NOWAIT);

	if (!init_nfo) {
	    dev_err(aes_mgr->dev, "%s: Failed to allocate initialization info structure.\n", __func__);
		return -ENOMEM;
	}

	keytype = s805_aes_crypt_get_key_type (ctx->keylen);

	if (keytype < 0) {
		
		crypto_ablkcipher_set_flags(crypto_ablkcipher_reqtfm(req), CRYPTO_TFM_RES_BAD_KEY_LEN);
		kfree(init_nfo);
		return keytype;
		
	}
	
	init_nfo->type = AES_DESC;
	init_nfo->aes_nfo.type = keytype; 
	init_nfo->aes_nfo.mode = mode;
	init_nfo->aes_nfo.dir = dir;

	aux = req->src;
	
	while (aux) {

		len = sg_dma_len(aux);
		
		if (!IS_ALIGNED(len, AES_BLOCK_SIZE)) {
			
			crypto_ablkcipher_set_flags(crypto_ablkcipher_reqtfm(req), CRYPTO_TFM_RES_BAD_BLOCK_LEN);
			kfree(init_nfo);
			return -EINVAL;
		}
		
		aux = sg_next(aux);
		j ++;
	}
	
	rctx->tx_desc = dmaengine_prep_dma_interrupt (&aes_mgr->chan->vc.chan, S805_DMA_CRYPTO_FLAG | S805_DMA_CRYPTO_AES_FLAG);

	if (!rctx->tx_desc) {
		
		dev_err(aes_mgr->dev, "%s: Failed to allocate dma descriptor.\n", __func__);
		kfree(init_nfo);
		return -ENOMEM;
	}

	rctx->tx_desc = s805_scatterwalk (req->src, req->dst, init_nfo, rctx->tx_desc, req->nbytes, true);
	
	if (!rctx->tx_desc) {
		
		dev_err(aes_mgr->dev, "%s: Failed to allocate dma descriptors.\n", __func__);
		kfree(init_nfo);
		return -ENOMEM;
	}
	
	kfree(init_nfo);

	//rctx->dir = dir;
	rctx->mode = mode;
	rctx->tx_desc->callback = (void *) &s805_aes_crypt_handle_completion;
	rctx->tx_desc->callback_param = (void *) req;
	
	return s805_aes_crypt_schedule_job (req);
}

static int s805_aes_ecb_encrypt(struct ablkcipher_request * req) {

    return s805_aes_crypt_prep (req, AES_MODE_ECB, AES_DIR_ENCRYPT);

}

static int s805_aes_ecb_decrypt(struct ablkcipher_request * req) {

	return s805_aes_crypt_prep (req, AES_MODE_ECB, AES_DIR_DECRYPT);
	
}

static int s805_aes_cbc_encrypt(struct skcipher_givcrypt_request * req) {

	return s805_aes_crypt_prep (&req->creq, AES_MODE_CBC, AES_DIR_ENCRYPT);

}

static int s805_aes_cbc_decrypt(struct skcipher_givcrypt_request * req) {

	return s805_aes_crypt_prep (&req->creq, AES_MODE_CBC, AES_DIR_DECRYPT);

}

static int s805_aes_ctr_encrypt(struct skcipher_givcrypt_request * req) {

    return s805_aes_crypt_prep (&req->creq, AES_MODE_CTR, AES_DIR_ENCRYPT);

}

static int s805_aes_ctr_decrypt(struct skcipher_givcrypt_request * req) {

	return s805_aes_crypt_prep (&req->creq, AES_MODE_CTR, AES_DIR_DECRYPT);

}

static struct crypto_alg s805_aes_algs[] = {
{
	.cra_name		    = "ecb(aes)-hw",
	.cra_driver_name	= "s805-ecb-aes",
	.cra_priority		= 100,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC, //CRYPTO_ALG_GENIV | CRYPTO_ALG_TYPE_GIVCIPHER | 
	.cra_blocksize		= AES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_aes_ctx),
	.cra_alignmask		= AES_BLOCK_SIZE - 1,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_aes_cra_init,
	.cra_exit		    = s805_aes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = AES_MIN_KEY_SIZE,
		.max_keysize	     = AES_MAX_KEY_SIZE,
		.setkey		         = s805_aes_setkey,
		.encrypt	         = s805_aes_ecb_encrypt,
		.decrypt	         = s805_aes_ecb_decrypt
	}
},
{
	.cra_name		    = "cbc(aes)-hw",
	.cra_driver_name	= "s805-cbc-aes",
	.cra_priority		= 100,
	.cra_flags		    = CRYPTO_ALG_TYPE_GIVCIPHER | CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC, //CRYPTO_ALG_GENIV | 
	.cra_blocksize		= AES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_aes_ctx),
	.cra_alignmask		= AES_BLOCK_SIZE - 1,
	.cra_type		    = &crypto_givcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_aes_cra_init,
	.cra_exit		    = s805_aes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = AES_MIN_KEY_SIZE,
		.max_keysize	     = AES_MAX_KEY_SIZE,
		.ivsize		         = AES_BLOCK_SIZE,
		.setkey		         = s805_aes_setkey,
		.givencrypt	         = s805_aes_cbc_encrypt,
		.givdecrypt	         = s805_aes_cbc_decrypt
	}
},
{
	.cra_name		    = "ctr(aes)-hw",
	.cra_driver_name	= "s805-ctr-aes",
	.cra_priority		= 100,
	.cra_flags		    = CRYPTO_ALG_TYPE_GIVCIPHER | CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC, //CRYPTO_ALG_GENIV | 
	.cra_blocksize		= AES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_aes_ctx),
	.cra_alignmask		= AES_BLOCK_SIZE - 1,
	.cra_type		    = &crypto_givcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_aes_cra_init,
	.cra_exit		    = s805_aes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = AES_MIN_KEY_SIZE,
		.max_keysize	     = AES_MAX_KEY_SIZE,
		.ivsize		         = AES_BLOCK_SIZE,
		.setkey		         = s805_aes_setkey,
		.givencrypt	         = s805_aes_ctr_encrypt,
		.givdecrypt	         = s805_aes_ctr_decrypt
	}
}
};

static int s805_aes_register_algs ( void )
{
	int err, i, j;
	
	for (i = 0; i < ARRAY_SIZE(s805_aes_algs); i++) {
		err = crypto_register_alg(&s805_aes_algs[i]);
		if (err)
			goto err_aes_algs;
	}

	return 0;

 err_aes_algs:
	for (j = 0; j < i; j++)
		crypto_unregister_alg(&s805_aes_algs[j]);
	
	return err;
}

static int s805_aes_probe(struct platform_device *pdev)
{

	int err;
	static dma_cap_mask_t mask;
	struct dma_chan * chan;
	
    aes_mgr = kzalloc(sizeof(struct s805_aes_mgr), GFP_KERNEL);
	if (!aes_mgr) {
		dev_err(&pdev->dev, "s805 AES mgr device failed to allocate.\n");
		return -ENOMEM;
	}

    aes_mgr->dev = &pdev->dev;
	
	INIT_LIST_HEAD(&aes_mgr->jobs);
	spin_lock_init(&aes_mgr->lock);
	
	err = s805_aes_register_algs();
	
	if (err) {
		
		dev_err(aes_mgr->dev, "s805 AES: failed to register algorithms.\n");
		kfree(aes_mgr);
		return err;
	}
	
	dma_cap_zero(mask);
	dma_cap_set(DMA_INTERRUPT, mask);
	
    chan = dma_request_channel ( mask, NULL, NULL );

	if (!chan) {

		dev_err(aes_mgr->dev, "s805 AES: failed to get dma channel.\n");
		kfree(aes_mgr);
		return -ENOSYS;
		
	} else {
		
		dev_info(aes_mgr->dev, "s805 AES: grabbed dma channel (%s).\n", dma_chan_name(chan));
		aes_mgr->chan = to_s805_dma_chan(chan);
	}
	
    dev_info(aes_mgr->dev, "Loaded S805 AES crypto driver\n");

	return 0;
}

static int s805_aes_remove(struct platform_device *pdev)
{
	int i, err, ret = 0;
	
	for (i = 0; i < ARRAY_SIZE(s805_aes_algs); i++) {
		err = crypto_unregister_alg(&s805_aes_algs[i]);
		if (err) {
		    dev_err(aes_mgr->dev, "s805 AES: Error unregistering algorithms.\n");
			ret = err;
		}
	}
	
	dma_release_channel ( &aes_mgr->chan->vc.chan );
	kfree(aes_mgr);
	
	return ret;
}

static struct platform_driver s805_aes_driver = {
	.probe		= s805_aes_probe,
	.remove		= s805_aes_remove,
	.driver		= {
		.name = "s805-dmac-aes",
		.owner	= THIS_MODULE,
		.of_match_table = s805_aes_of_match
	},
};

module_platform_driver(s805_aes_driver);

MODULE_ALIAS("platform:s805-aes");
MODULE_DESCRIPTION("s805 AES hw acceleration support.");
MODULE_AUTHOR("szz-dvl");
MODULE_LICENSE("GPL v2");
