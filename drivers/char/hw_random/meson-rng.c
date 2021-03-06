/*
 * meson-rng.c - Random Number Generator driver for the Amlogic Meson
 *
 * Copyright (C) 2014 Amlogic, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/hw_random.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/err.h>

#include <mach/am_regs.h>
#if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON6
#include <mach/mod_gate.h>
#endif

#ifdef CONFIG_ARCH_RANDOM
#include <mach/hwrng.h>

spinlock_t hwrng_lock;
bool hwrng_skip;
#endif

#define MESON_RNG_AUTOSUSPEND_DELAY    100

//#define DEBUG
#ifdef DEBUG
#define print_state(desc) printk(KERN_INFO "%s L%d " desc " on=%x,%x\n", __func__, __LINE__, \
		IS_CLK_GATE_ON(RANDOM_NUM_GEN), IS_CLK_GATE_ON(RANDOM_NUM_GEN1));
#else
#define print_state(desc)
#endif

struct meson_rng {
	struct device *dev;
	struct hwrng rng;
};

#ifdef CONFIG_ARCH_RANDOM
int arch_get_random_long(unsigned long *v) {
	
	/* Portability */
	if (BITS_PER_LONG == 32) 
		return arch_get_random_int ((unsigned int *) v);
	
	spin_lock(&hwrng_lock);
	
	aml_read_reg32(P_VDIN_ASFIFO_CTRL2);
	aml_read_reg32(P_VDIN_MATRIX_CTRL);
	aml_read_reg32(P_PAD_PULL_UP_REG5);
	
	*v = aml_read_reg32(P_RAND64_ADDR0);
	
	*v <<= (sizeof(int) * 8);
	*v |= (aml_read_reg32(P_RAND64_ADDR1) & ~0U);
	
	spin_unlock(&hwrng_lock);
	
	return 8;
}

int arch_get_random_int(unsigned int *v) {

	spin_lock(&hwrng_lock);

	aml_read_reg32(P_VDIN_ASFIFO_CTRL2);
	aml_read_reg32(P_VDIN_MATRIX_CTRL);
	aml_read_reg32(P_PAD_PULL_UP_REG5);
	
	if (!hwrng_skip)
		*v = aml_read_reg32(P_RAND64_ADDR0);
	else
		*v = aml_read_reg32(P_RAND64_ADDR1);
	
	spin_unlock(&hwrng_lock);
	
	hwrng_skip = !hwrng_skip;
		
	return 4;

}
#endif

static int meson_read(struct hwrng *rng, void *buf,
                      size_t max, bool wait)
{
	struct meson_rng *meson_rng = container_of(rng,
	                                  struct meson_rng, rng);
	u32 *data = buf;
	
	pm_runtime_get_sync(meson_rng->dev);

#ifdef CONFIG_ARCH_RANDOM
	spin_lock(&hwrng_lock);
#endif
	
	// this will cause additional disturbances
	aml_read_reg32(P_VDIN_ASFIFO_CTRL2);
	aml_read_reg32(P_VDIN_MATRIX_CTRL);
	aml_read_reg32(P_PAD_PULL_UP_REG5);

	// 'max' minimum is 32, so this is safe
	data[0] = aml_read_reg32(P_RAND64_ADDR0);
	data[1] = aml_read_reg32(P_RAND64_ADDR1);

#ifdef CONFIG_ARCH_RANDOM
	spin_unlock(&hwrng_lock);
#endif
	
	pm_runtime_mark_last_busy(meson_rng->dev);
	pm_runtime_put_autosuspend(meson_rng->dev);

	return 8;
}

static int meson_rng_init(struct hwrng *rng)
{
	print_state("b resu");
	switch_mod_gate_by_type(MOD_RANDOM_NUM_GEN, 1);
	print_state("a resu");

	// Enable the ring oscillator
	// NOTE:  CBUS 0x207f bit[0] = enable
	// NOTE:  CBUS 0x207f bit[1] = high-frequency mode.
	//      Setting bit[1]=1 may change the randomness even more
	aml_set_reg32_mask(P_AM_RING_OSC_REG0, (1 << 0) | (1 << 1));

	return 0;
}

static void meson_rng_cleanup(struct hwrng *rng)
{
	print_state("b susp");
	switch_mod_gate_by_type(MOD_RANDOM_NUM_GEN, 0);
	print_state("a susp");
}

static int meson_rng_probe(struct platform_device *pdev)
{
	struct meson_rng *meson_rng;

	meson_rng = devm_kzalloc(&pdev->dev, sizeof(struct meson_rng),
	                         GFP_KERNEL);
	if (!meson_rng)
		return -ENOMEM;

	meson_rng->dev = &pdev->dev;
	meson_rng->rng.name = "meson";
	meson_rng->rng.init = meson_rng_init;
	meson_rng->rng.cleanup = meson_rng_cleanup;
	meson_rng->rng.read = meson_read;

	platform_set_drvdata(pdev, meson_rng);

	pm_runtime_set_autosuspend_delay(&pdev->dev, MESON_RNG_AUTOSUSPEND_DELAY);
	pm_runtime_use_autosuspend(&pdev->dev);
	print_state("b set_active");
	pm_runtime_set_active(&pdev->dev);
	print_state("a set_active");
	pm_runtime_enable(&pdev->dev);

#ifdef CONFIG_ARCH_RANDOM
	spin_lock_init(&hwrng_lock);
	hwrng_skip = false;
#endif
	
	return hwrng_register(&meson_rng->rng);
}

static int meson_rng_remove(struct platform_device *pdev)
{
	struct meson_rng *meson_rng = platform_get_drvdata(pdev);
	hwrng_unregister(&meson_rng->rng);
	return 0;
}

#if defined(CONFIG_PM_SLEEP) || defined(CONFIG_PM_RUNTIME)
static int meson_rng_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct meson_rng *meson_rng = platform_get_drvdata(pdev);
	meson_rng_cleanup(&meson_rng->rng);
	return 0;
}

static int meson_rng_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct meson_rng *meson_rng = platform_get_drvdata(pdev);
	meson_rng_init(&meson_rng->rng);
	return 0;
}
#endif

static UNIVERSAL_DEV_PM_OPS(meson_rng_pm_ops, meson_rng_runtime_suspend,
                            meson_rng_runtime_resume, NULL);

static const struct of_device_id meson_rng_of_match[]={
	{ .compatible = "amlogic,meson-rng", },
	{},
};

static struct platform_driver meson_rng_driver = {
	.driver		= {
		.name	= "meson-rng",
		.owner	= THIS_MODULE,
		.of_match_table = meson_rng_of_match,
		.pm	= &meson_rng_pm_ops,
	},
	.probe		= meson_rng_probe,
	.remove		= meson_rng_remove,
};

module_platform_driver(meson_rng_driver);

MODULE_DESCRIPTION("Meson H/W Random Number Generator driver");
MODULE_AUTHOR("Lawrence Mok <lawrence.mok@amlogic.com");
MODULE_LICENSE("GPL");
