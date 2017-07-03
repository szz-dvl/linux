#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <linux/s805_dmac.h>

struct s805_crypto_mgr {

	struct device * dev;
	
	int keylen;
	u32 key[AES_KEYSIZE_256 / sizeof(u32)];
	
};

struct s805_crypto_mgr * mgr;

static int s805_aes_cra_init(struct crypto_tfm *tfm)
{
	//tfm->crt_ablkcipher.reqsize = sizeof(struct atmel_aes_reqctx);

	return 0;
}

static void s805_aes_cra_exit(struct crypto_tfm *tfm)
{
}

static int s805_aes_setkey(struct crypto_ablkcipher *tfm, const u8 *key,
						   unsigned int keylen)
{
	struct s805_crypto_mgr *ctx = crypto_ablkcipher_ctx(tfm);

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
	.cra_ctxsize		= sizeof(struct s805_crypto_mgr),
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
	.cra_ctxsize		= sizeof(struct s805_crypto_mgr),
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
	.cra_ctxsize		= sizeof(struct s805_crypto_mgr),
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
	
    mgr = kzalloc(sizeof(struct s805_crypto_mgr), GFP_KERNEL);
	if (!mgr) {
		dev_err(&pdev->dev, "s805 AES mgr device failed to allocate.\n");
		return -ENOMEM;
	}

    mgr->dev = &pdev->dev;
	
	err = s805_aes_register_algs();
	
	if (err) {
		kfree(mgr);
		return err;
	}
	
    dev_info(mgr->dev, "Loaded S805 AES crypto driver\n");

	return 0;
}

static int s805_aes_remove(struct platform_device *pdev)
{

	kfree(mgr);

	return 0;
}

static struct platform_driver s805_aes_driver = {
	.probe		= s805_aes_probe,
	.remove		= s805_aes_remove,
	.driver		= {
		.name	= "s805_crypto_aes",
		.owner	= THIS_MODULE,
	},
};

module_platform_driver(s805_aes_driver);

MODULE_ALIAS("platform:s805-aes");
MODULE_DESCRIPTION("s805 AES hw acceleration support.");
MODULE_AUTHOR("szz-dvl");
MODULE_LICENSE("GPL v2");
