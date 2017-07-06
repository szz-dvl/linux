#include <linux/platform_device.h>
#include <linux/module.h>
#include <crypto/algapi.h>
#include <../backports/include/crypto/internal/acompress.h>
#include <linux/s805_dmac.h>

#define S805_DIVX_BLOCK_SIZE        2

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

static int s805_divx_compress (struct acomp_req *req) {

	return 0;

}

static int s805_divx_decompress (struct acomp_req *req) {

	return 0;

}

static void s805_divx_dst_free (struct scatterlist *dst)
{	
}

static int s805_divx_init (struct crypto_acomp *tfm) {

	return 0;

}

static void s805_divx_exit (struct crypto_acomp *tfm)
{	
}

static int s805_divx_cra_init(struct crypto_tfm *tfm)
{
 	
	return 0;

}

static void s805_divx_cra_exit(struct crypto_tfm *tfm)
{
}

static struct acomp_alg divx_alg = {
	.compress   = s805_divx_compress,
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
