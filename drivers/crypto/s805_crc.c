#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>
#include <crypto/algapi.h>
#include <linux/s805_dmac.h>

#define S805_CRC_CHECK_SUM              P_NDMA_CRC_OUT
#define S805_CRC_DIGEST_SIZE            4
#define S805_CRC_BLOCK_SIZE             1

#define S805_DTBL_CRC_NO_WRITE(val)          ((val & 0x1) << 4) 
#define S805_DTBL_CRC_RESET(val)             ((val & 0x1) << 3)
#define S805_DTBL_CRC_POST_ENDIAN(type)      (type & 0x7)

struct s805_crc_mgr {

	struct device * dev;
    struct s805_chan * chan;
	struct list_head jobs;
	spinlock_t lock;

	bool busy;
	
};

struct s805_crc_ctx {

	uint	keylen;

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
	desc_tbl->table->crypto |= S805_DTBL_CRC_RESET(!frames ? 1 : 0); /* To be tested. */
	desc_tbl->table->crypto |= S805_DTBL_CRC_NO_WRITE(1);
	
	return desc_tbl;
	
}

s805_dtable * sg_crc_move_along (s805_dtable * cursor, s805_init_desc * init_nfo) {

	if (cursor) {
		list_add_tail(&cursor->elem, &init_nfo->d->desc_list);
 		init_nfo->d->frames ++;
	}
	
	return def_init_crc_tdesc(init_nfo->d->frames);
}


static int s805_crc_hash_init (struct ahash_request *req) {

	return 0;

}

static int s805_crc_hash_update (struct ahash_request *req) {

	return 0;

}

static int s805_crc_hash_final (struct ahash_request *req) {

	return 0;

}

static int s805_crc_hash_finup (struct ahash_request *req) {

	return 0;

}

static int s805_crc_hash_digest (struct ahash_request *req) {

	return 0;

}

static int s805_crc_hash_set_key (struct crypto_ahash *tfm, const u8 *key, unsigned int keylen) {

	return -ENOSYS;

}

static int s805_crc_hash_export (struct ahash_request *req, void *out) {

	return -ENOSYS;

}

static int s805_crc_cra_init(struct crypto_tfm *tfm)
{
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
	.setkey     = s805_crc_hash_set_key,
	.export     = s805_crc_hash_export,
	.halg.digestsize = S805_CRC_DIGEST_SIZE,
	.halg.statesize  = S805_CRC_DIGEST_SIZE, /* look 4 info. */
	.halg.base	     = {
		.cra_name		    = "crc-32",
		.cra_driver_name	= "s805-crc-32",
		.cra_priority		= 100,
		.cra_flags		    = CRYPTO_ALG_TYPE_AHASH | CRYPTO_ALG_ASYNC,
		.cra_blocksize		= S805_CRC_BLOCK_SIZE,
		.cra_ctxsize		= sizeof(struct s805_crc_ctx),
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
