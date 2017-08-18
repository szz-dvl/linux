#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/scatterlist.h>
#include <linux/ioport.h>
#include <linux/s805_dmac.h>

#define MESON8B_GPIO_CTRL_SIZE                  8
#define MESON8B_GPIO_ALIGN_SIZE                 8
#define MESON8B_CBUS_PHYS(reg)                  IO_CBUS_PHY_BASE + CBUS_REG_OFFSET(reg)
#define MESON8B_GPIO_Y_STR                      MESON8B_CBUS_PHYS(0x200F)
#define MESON8B_GPIO_STR                        MESON8B_GPIO_Y_STR
#define MESON8B_GPIO_VIRT_STR                   P_PREG_PAD_GPIO1_EN_N

typedef struct soft_pwm_gpio {

	uint num;
	uint duty;

	struct list_head elem;
	
} gpio;

typedef struct soft_pwm_cycle {

	struct dma_async_tx_descriptor * tx_desc;
    u64 * buf;
	uint len;
	dma_addr_t paddr;
	
} cycle;

struct meson_soft_pwm {

	struct device * dev;
    struct s805_chan * chan;
	
	struct list_head gpios;
	cycle * cycle;
   	
	uint freq;

	bool busy;
};

struct meson_soft_pwm * spwm_mgr;

#define gpio_for_each_safe(pin)											\
    gpio * aux__;														\
	list_for_each_entry_safe(pin, aux__, &spwm_mgr->gpios, elem)

#define gpio_for_each(pin)								\
	list_for_each_entry(pin, &spwm_mgr->gpios, elem)

#define gpio_add(pin)							\
    list_add_tail (&pin->elem, &spwm_mgr->gpios);

static uint glob_freq = 0;
static uint glob_duty = 0;
static uint glob_enable = 0;

static const struct of_device_id meson_spwm_of_match[] =
{
    {.compatible = "meson8b,meson8b-soft-pwm"},
    {},
};

static void meson_swpm_start_cycle (void);
static void meson_swpm_stop_cycle (void);

static void meson_swpm_set_gpio_duty (gpio * pin) {

	uint i;
	uint limit = spwm_mgr->cycle->len / MESON8B_GPIO_CTRL_SIZE;
    uint now;
	
	dev_warn(spwm_mgr->dev, "%s: gpio = %u, duty = %u\n", __func__, pin->num, pin->duty);
	
	for (i = 0; i < limit; i++) {
		
		now = ( limit / ( limit - i ));
		
		if ( now >= (100 / ( 100 - pin->duty )) )	
			spwm_mgr->cycle->buf[i] &= ~BIT(pin->num); /* Low 32, must hit CBUS 0x2010 */
		else 
			spwm_mgr->cycle->buf[i] |= BIT(pin->num);  /* Low 32, must hit CBUS 0x2010 */
		
		
		//dev_warn(spwm_mgr->dev, "buff[%d]: 0x%016llx, now: %u\n", i, spwm_mgr->cycle->buf[i], now);
	}
}

static gpio * meson_swpm_get_gpio ( uint num ) {

	gpio * pin;

	gpio_for_each(pin) {

		if (pin->num == num )
			return pin;
		
	}

	return NULL;
}

static ssize_t meson_swpm_show_duty (struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", glob_duty);
}

static ssize_t meson_swpm_set_duty (struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{

	uint val;
	
	if(!(sscanf(buf, "%u", &val)))
		return	-EINVAL;

	glob_duty = val;
	return count;
}

static ssize_t meson_swpm_show_enable (struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", glob_enable);
}

static ssize_t meson_swpm_set_enable (struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	uint val;
	
	if(!(sscanf(buf, "%u", &val)))
		return	-EINVAL;
	
	glob_enable = val;

	if (glob_enable)
		meson_swpm_start_cycle();
	else
		meson_swpm_stop_cycle();
	
	return count;
}

static ssize_t meson_swpm_set_gpio (struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{

	uint val;
	gpio * pin;
	
	if(!(sscanf(buf, "%u", &val)))
		return	-EINVAL;

	pin = meson_swpm_get_gpio ( val );

	if (!pin) {

		pin = kzalloc(sizeof(gpio), GFP_KERNEL);
		if (!pin)
			return 0;

		pin->num = val;
		gpio_add(pin);
	}

	pin->duty = glob_duty;
	meson_swpm_set_gpio_duty(pin);
	
	return count;
}

static void meson_swpm_start_cycle (void) {

	uint limit = spwm_mgr->cycle->len / MESON8B_GPIO_CTRL_SIZE;
	struct dma_slave_config config;
	dma_cookie_t tx_cookie;
	void * iomem;
	uint i;
	dma_addr_t phys;

	/* Testing, to read registers. */
	struct scatterlist * src = kzalloc(sizeof(struct scatterlist), GFP_KERNEL);
	
	sg_set_buf(src, spwm_mgr->cycle->buf, spwm_mgr->cycle->len/* 8 */);
	sg_dma_address(src) = spwm_mgr->cycle->paddr;
	sg_mark_end(src);
		
	spwm_mgr->busy = true;
	
	if (!spwm_mgr->cycle)
	    goto out;

	//iomem = ioremap(MESON8B_GPIO_STR, MESON8B_GPIO_CTRL_SIZE); /* Next line will deliver the same address. */
	iomem = __arm_ioremap(MESON8B_GPIO_STR, MESON8B_GPIO_CTRL_SIZE, MT_DEVICE);
	
	//phys = virt_to_dma(spwm_mgr->chan->vc.chan.device->dev, iomem); /* Next line will deliver the same address. */
	phys = dma_map_single(spwm_mgr->chan->vc.chan.device->dev, iomem, MESON8B_GPIO_CTRL_SIZE, DMA_FROM_DEVICE);
	
    config.direction = DMA_DEV_TO_MEM;
	config.src_addr_width = DMA_SLAVE_BUSWIDTH_8_BYTES;
	config.src_addr = MESON8B_GPIO_STR;//MESON8B_GPIO_VIRT_STR;//phys;  /* None of those achieve the expected */
	
	dev_warn(spwm_mgr->dev, "dev: 0x%08x, dev_virt: %p, iomem: (%p, 0x%08x).\n",
			 MESON8B_GPIO_STR,
			 (void *) MESON8B_GPIO_VIRT_STR,
			 iomem, phys);
	
	dmaengine_slave_config(&spwm_mgr->chan->vc.chan, &config);

	/* Legitimate code, must generate a pulse on the selected gpios */
	
	/* spwm_mgr->cycle->tx_desc = dmaengine_prep_dma_cyclic(&spwm_mgr->chan->vc.chan, */
	/* 													 spwm_mgr->cycle->paddr,  */
	/* 													 spwm_mgr->cycle->len, */
	/* 													 spwm_mgr->cycle->len, */
	/* 													 DMA_MEM_TO_DEV, */
	/* 													 0); */
	
	/* Testing code, must read registers CBUS 0x200F and CBUS 0x2010 to src. */
    spwm_mgr->cycle->tx_desc = dmaengine_prep_slave_sg(&spwm_mgr->chan->vc.chan,
													   src,
													   /* 8, */spwm_mgr->cycle->len,
													   DMA_DEV_TO_MEM,
													   0);

	kfree(src);

	if (!spwm_mgr->cycle->tx_desc)
		goto out;
	
    tx_cookie = dmaengine_submit ( spwm_mgr->cycle->tx_desc );

	if (tx_cookie < 0)
		goto out;

	dma_async_issue_pending ( &spwm_mgr->chan->vc.chan );

	/* Testing ---> Debug. */
	dma_wait_for_async_tx(spwm_mgr->cycle->tx_desc);

	for (i = 0; i < limit; i++) 
		dev_warn(spwm_mgr->dev, "result[%d]: 0x%016llx.\n", i, spwm_mgr->cycle->buf[i]);
	
    //WR(~0U & ~(BIT(8) | BIT(7)) , iomem); ---> This will actually write the register ...
	//u32 reg = RD(iomem) ---> This will actually read the register ...
	
 out:
	spwm_mgr->busy = false;
	return;
}

static void meson_swpm_stop_cycle (void) {

	dmaengine_terminate_all ( &spwm_mgr->chan->vc.chan );
	spwm_mgr->busy = false;	
}

static bool meson_swpm_setup_cycle ( void ) {
	
	spwm_mgr->cycle = kzalloc (sizeof(cycle), GFP_KERNEL);
	
	if (!spwm_mgr->cycle) {
		
		dev_err(spwm_mgr->dev, "soft-pwm: No cycle.\n");
		return false;

	}
	
	spwm_mgr->cycle->len = ALIGN(spwm_mgr->freq * MESON8B_GPIO_CTRL_SIZE, MESON8B_GPIO_ALIGN_SIZE);
	
	spwm_mgr->cycle->buf = dma_alloc_coherent(spwm_mgr->chan->vc.chan.device->dev,
											  spwm_mgr->cycle->len,
											  &spwm_mgr->cycle->paddr,
											  GFP_KERNEL);

	if (dma_mapping_error(spwm_mgr->chan->vc.chan.device->dev, spwm_mgr->cycle->paddr))
		goto err_cycle;
	else
		memset(spwm_mgr->cycle->buf, 0, spwm_mgr->cycle->len);

	return true;
	
 err_cycle:

	dev_err(spwm_mgr->dev, "soft-pwm: No DMA, cycle, (%p, 0x%08x).\n", spwm_mgr->cycle->buf, spwm_mgr->cycle->paddr);
	kfree (spwm_mgr->cycle);
	
	return false;
}

static void meson_swpm_free_cycle ( void ) {

	dma_free_coherent(spwm_mgr->chan->vc.chan.device->dev, spwm_mgr->cycle->len, spwm_mgr->cycle->buf, spwm_mgr->cycle->paddr);
	kfree(spwm_mgr->cycle);
	
}

static	DEVICE_ATTR(gpio, S_IWUGO | S_IXUGO, NULL, meson_swpm_set_gpio);
static	DEVICE_ATTR(duty, S_IRWXUGO, meson_swpm_show_duty, meson_swpm_set_duty);
static	DEVICE_ATTR(enable, S_IRWXUGO, meson_swpm_show_enable, meson_swpm_set_enable);

static struct attribute *meson_spwm_sysfs_entries[] = {
	&dev_attr_gpio.attr,
	&dev_attr_duty.attr,
	&dev_attr_enable.attr,
	NULL
};
static struct attribute_group meson_spwm_attr_group = {
	.name   = "soft-pwm",
	.attrs  = meson_spwm_sysfs_entries,
};

static int get_def_freq_cmdline (char *str)
{
	
	get_option(&str, &glob_freq);
    return 1;
	
}
__setup("spwm_freq=", get_def_freq_cmdline);

static int meson_spwm_probe(struct platform_device *pdev)
{

	int ret;
	static dma_cap_mask_t mask;
	struct dma_chan * chan;
	
    spwm_mgr = kzalloc(sizeof(struct meson_soft_pwm), GFP_KERNEL);
	if (!spwm_mgr) {
		dev_err(&pdev->dev, "Meson-8b soft-pwm mgr device failed to allocate.\n");
		return -ENOMEM;
	}

    spwm_mgr->dev = &pdev->dev;

	INIT_LIST_HEAD(&spwm_mgr->gpios);
	spwm_mgr->freq = glob_freq;

	/* Between 100 (800 Bytes) and 100000 (~781K) seems fine. */
	if (!spwm_mgr->freq) {
		
		/* If no cmd line param present request default frequency from device tree */ 
		if (of_property_read_u32(pdev->dev.of_node,
								 "meson8b,spwm-freq",
								 &spwm_mgr->freq)) {
			
			dev_err(&pdev->dev, "Failed to get default freq.\n");
			ret = -EINVAL;
			
			goto err_no_spwm;
		}	
	}
	
	dma_cap_zero(mask);
	dma_cap_set(DMA_CYCLIC, mask);
	
    chan = dma_request_channel ( mask, NULL, NULL );

	if (!chan) {

		dev_err(spwm_mgr->dev, "soft-pwm: failed to get dma channel.\n");
		ret = -ENOSYS;

		goto err_no_spwm;
		
	} else {
		
		dev_info(spwm_mgr->dev, "soft-pwm: grabbed dma channel (%s).\n", dma_chan_name(chan));
		spwm_mgr->chan = to_s805_dma_chan(chan);
	}
	
	platform_set_drvdata(pdev, spwm_mgr);

	ret = sysfs_create_group(&spwm_mgr->dev->kobj, &meson_spwm_attr_group);

	if (ret < 0)	{
		
		dev_err(spwm_mgr->dev, "soft-pwm: failed to create sysfs group. \n");
		goto err_no_spwm;
		
	}
		
	if(!meson_swpm_setup_cycle()) {

		dma_release_channel ( &spwm_mgr->chan->vc.chan );
		ret = -ENOMEM;
		goto err_no_spwm;

	}
	
    dev_info(spwm_mgr->dev, "Loaded Meson-8b soft-pwm driver\n");
	
	return 0;
	
 err_no_spwm:

	kfree(spwm_mgr);
	return ret;
}

static int meson_spwm_remove(struct platform_device *pdev)
{
	int ret = 0;
	gpio * pin;
	
    struct meson_soft_pwm * m = platform_get_drvdata(pdev);

	if (m->busy)
		meson_swpm_stop_cycle();
	
	meson_swpm_free_cycle();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
	
	gpio_for_each_safe(pin) {

		list_del(&pin->elem);
		kfree(pin);
	}
	
#pragma GCC diagnostic pop

	dma_release_channel ( &m->chan->vc.chan );
	sysfs_remove_group(&spwm_mgr->dev->kobj, &meson_spwm_attr_group);
	kfree(m);
	
	return ret;
}

static struct platform_driver meson_spwm_driver = {
	.probe		= meson_spwm_probe,
	.remove		= meson_spwm_remove,
	.driver		= {
		.name = "meson_spwm",
		.owner	= THIS_MODULE,
		.of_match_table = meson_spwm_of_match
	},
};

static int __init meson_spwm_init(void)
{
	return platform_driver_register(&meson_spwm_driver);
}

static void __exit meson_spwm_exit(void)
{
	platform_driver_unregister(&meson_spwm_driver);
}

module_init(meson_spwm_init);
module_exit(meson_spwm_exit);

MODULE_ALIAS("platform:meson-spwm");
MODULE_DESCRIPTION("Meson-8b soft-pwm.");
MODULE_AUTHOR("szz-dvl");
MODULE_LICENSE("GPL v2");

/* This code has been tested for both VMSPLIT_2G and VMSPLIT_3G, always with no success. */
