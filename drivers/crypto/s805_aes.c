#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <linux/s805_dmac.h>


/* Registers & Bitmaps for the s805 DMAC AES algorithm. */

#define S805_AES_KEY_0       P_NDMA_AES_KEY_0
#define S805_AES_KEY_1       P_NDMA_AES_KEY_1
#define S805_AES_KEY_2       P_NDMA_AES_KEY_2
#define S805_AES_KEY_3       P_NDMA_AES_KEY_3
#define S805_AES_KEY_4       P_NDMA_AES_KEY_4
#define S805_AES_KEY_5       P_NDMA_AES_KEY_5
#define S805_AES_KEY_6       P_NDMA_AES_KEY_6
#define S805_AES_KEY_7       P_NDMA_AES_KEY_7

#define S805_DTBL_AES_POST_ENDIAN(type)       ((type & 0xf) << 4)
#define S805_DTBL_AES_PRE_ENDIAN(type)        (type & 0xf)
#define S805_DTBL_AES_KEY_TYPE(type)          ((type & 0x3) << 8)
#define S805_DTBL_AES_DIR(dir)                ((dir & 0x1) << 10)
#define S805_DTBL_AES_RESET_IV(restr)         ((restr & 0x1) << 11)
#define S805_DTBL_AES_MODE(mode)              ((mode & 0x3) << 12)

typedef enum aes_key_type {
	AES_KEY_TYPE_128,
	AES_KEY_TYPE_192,
	AES_KEY_TYPE_256,
	AES_KEY_TYPE_RESERVED,
} s805_aes_key_type;

typedef enum aes_mode {
	AES_MODE_ECB,
    AES_MODE_CBC,
	AES_MODE_CTR,
	AES_MODE_RESERVED,
} s805_aes_mode;

typedef enum aes_dir {
	AES_DIR_DECRYPT,
    AES_DIR_ENCRYPT
} s805_aes_dir;

struct s805_crypto_mgr {

	struct device * dev;
    struct s805_chan * chan;
	
};

struct s805_crypto_mgr * mgr;

struct s805_aes_ctx {

	int		keylen;
	u32		key[AES_KEYSIZE_256 / sizeof(u32)];

	//u16		block_size;
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
	
	desc_tbl->table = dma_pool_alloc(mgr->c->pool, GFP_NOWAIT | __GFP_ZERO, &desc_tbl->paddr); /* __GFP_ZERO: Not Working. */
	
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
	desc_tbl->table->crypto |= S805_DTBL_AES_PRE_ENDIAN(ENDIAN_NO_CHANGE);
	desc_tbl->table->crypto |= S805_DTBL_AES_KEY_TYPE(type);
	desc_tbl->table->crypto |= S805_DTBL_AES_DIR(dir);
	desc_tbl->table->crypto |= S805_DTBL_AES_RESET_IV(frames ? 0 : 1); /* To be tested! */
	desc_tbl->table->crypto |= S805_DTBL_AES_MODE(mode);
	
	return desc_tbl;
	
}

static int s805_aes_cra_init(struct crypto_tfm *tfm)
{
	//tfm->crt_ablkcipher.reqsize = sizeof(struct s805_aes_reqctx); /* ?? */

	return 0;
}

static void s805_aes_cra_exit(struct crypto_tfm *tfm)
{
}

static inline void s805_aes_cpykey_to_hw (const unsigned long * key, unsigned int keylen) {
	
	WR(key[0], S805_AES_KEY_0);
	WR(key[1], S805_AES_KEY_1);
	WR(key[2], S805_AES_KEY_2);
	WR(key[3], S805_AES_KEY_3);

	switch (keylen) {
	case AES_KEYSIZE_192:
		WR(key[4], S805_AES_KEY_4);
		WR(key[5], S805_AES_KEY_5);
	case AES_KEYSIZE_256:
		WR(key[6], S805_AES_KEY_6);
		WR(key[7], S805_AES_KEY_7);
	default:
		return;
	}

}
static int s805_aes_setkey(struct crypto_ablkcipher *tfm, const u8 *key,
						   unsigned int keylen)
{
	struct s805_aes_ctx *ctx = crypto_ablkcipher_ctx(tfm);

	switch (keylen) {
	case AES_KEYSIZE_128:
	case AES_KEYSIZE_192:
	case AES_KEYSIZE_256:
		memcpy(ctx->key, key, keylen);
		ctx->keylen = keylen;
		s805_aes_cpykey_to_hw ( (const unsigned long *)key, keylen );
		return 0;
	default:
		crypto_ablkcipher_set_flags(tfm, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}
	
	return 0;
}

static int s805_aes_ecb_encrypt(struct ablkcipher_request *req) {

	return 0;

}

static int s805_aes_ecb_decrypt(struct ablkcipher_request *req) {

	return 0;

}

static int s805_aes_cbc_encrypt(struct ablkcipher_request *req) {

	return 0;

}

static int s805_aes_cbc_decrypt(struct ablkcipher_request *req) {

	return 0;

}

static int s805_aes_ctr_encrypt(struct ablkcipher_request *req) {

	return 0;

}

static int s805_aes_ctr_decrypt(struct ablkcipher_request *req) {

	return 0;

}

static struct crypto_alg s805_aes_algs[] = {
{
	.cra_name		    = "ecb(aes)",
	.cra_driver_name	= "s805-ecb-aes",
	.cra_priority		= 100,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= AES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_aes_ctx),
	.cra_alignmask		= 0,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_aes_cra_init,
	.cra_exit		    = s805_aes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = AES_MIN_KEY_SIZE,
		.max_keysize	     = AES_MAX_KEY_SIZE,
		.setkey		         = s805_aes_setkey,
		.encrypt	         = s805_aes_ecb_encrypt,
		.decrypt	         = s805_aes_ecb_decrypt,
	}
},
{
	.cra_name		    = "cbc(aes)",
	.cra_driver_name	= "s805-cbc-aes",
	.cra_priority		= 100,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= AES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_aes_ctx),
	.cra_alignmask		= 0,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_aes_cra_init,
	.cra_exit		    = s805_aes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = AES_MIN_KEY_SIZE,
		.max_keysize	     = AES_MAX_KEY_SIZE,
		.ivsize		         = AES_BLOCK_SIZE,
		.setkey		         = s805_aes_setkey,
		.encrypt	         = s805_aes_cbc_encrypt,
		.decrypt	         = s805_aes_cbc_decrypt,
	}
},
{
	.cra_name		    = "ctr(aes)",
	.cra_driver_name	= "s805-ctr-aes",
	.cra_priority		= 100,
	.cra_flags		    = CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize		= AES_BLOCK_SIZE,
	.cra_ctxsize		= sizeof(struct s805_aes_ctx),
	.cra_alignmask		= 0,
	.cra_type		    = &crypto_ablkcipher_type,
	.cra_module		    = THIS_MODULE,
	.cra_init		    = s805_aes_cra_init,
	.cra_exit		    = s805_aes_cra_exit,
	.cra_u.ablkcipher   = {
		.min_keysize	     = AES_MIN_KEY_SIZE,
		.max_keysize	     = AES_MAX_KEY_SIZE,
		.ivsize		         = AES_BLOCK_SIZE,
		.setkey		         = s805_aes_setkey,
		.encrypt	         = s805_aes_ctr_encrypt,
		.decrypt	         = s805_aes_ctr_decrypt,
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
	
    mgr = kzalloc(sizeof(struct s805_crypto_mgr), GFP_KERNEL);
	if (!mgr) {
		dev_err(&pdev->dev, "s805 AES mgr device failed to allocate.\n");
		return -ENOMEM;
	}

    mgr->dev = &pdev->dev;

	dma_cap_zero(mask);
	dma_cap_set(DMA_INTERRUPT, mask);
	
    chan = dma_request_channel ( mask, NULL, NULL );

	if (!chan) {

		dev_err(mgr->dev, "s805 AES: failed to get dma channel.\n");
		kfree(mgr);
		return -ENOSYS;
		
	} else {
		
		dev_info(mgr->dev, "s805 AES: grabbed dma channel (%s).\n", dma_chan_name(chan));
		mgr->chan = to_s805_dma_chan(chan);
	}
	
	err = s805_aes_register_algs();
	
	if (err) {
		
		dev_err(mgr->dev, "s805 AES: failed to register algorithms.\n");
		kfree(mgr);
		return err;
	}
	
    dev_info(mgr->dev, "Loaded S805 AES crypto driver\n");

	return 0;
}

static int s805_aes_remove(struct platform_device *pdev)
{
	
	dma_release_channel ( mgr->chan.vc.chan );
	kfree(mgr);

	return 0;
}

static struct platform_driver s805_aes_driver = {
	.probe		= s805_aes_probe,
	.remove		= s805_aes_remove,
	.driver		= {
		.name	= "s805_crypto_aes",
		.owner	= THIS_MODULE,
		.of_match_table = s805_aes_of_match
	},
};

module_platform_driver(s805_aes_driver);

MODULE_ALIAS("platform:s805-aes");
MODULE_DESCRIPTION("s805 AES hw acceleration support.");
MODULE_AUTHOR("szz-dvl");
MODULE_LICENSE("GPL v2");
