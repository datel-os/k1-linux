// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic True Random Number Generator driver
 * Copyright (c) 2019 漆鹏振 (Qi Pengzhen) <aric.pzqi@ingenic.com>
 * Copyright (c) 2020 周琰杰 (Zhou Yanjie) <zhouyanjie@wanyeetech.com>
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

/* DTRNG register offsets */
#define TRNG_REG_CFG_OFFSET			0x00
#define TRNG_REG_RANDOMNUM_OFFSET	0x04
#define TRNG_REG_STATUS_OFFSET		0x08

/* bits within the CFG register */
#define CFG_GEN_EN					BIT(0)
#define CFG_RDY_CLR					BIT(12)

/* bits within the STATUS register */
#define STATUS_RANDOM_RDY			BIT(0)

/*
 * The X2000's bits are not independent of each other: over 16 million of them
 * a bit equalled the one before it 48.2 % of the time, and Markov models up to
 * order 12 put the min-entropy at 0.94 bit per bit. Claim less than that.
 */
#define X2000_QUALITY				900

enum ingenic_trng_version {
	ID_X1830,
	ID_X2000,
};

struct ingenic_trng {
	void __iomem *base;
	struct hwrng rng;
	enum ingenic_trng_version version;
};

/*
 * On the X2000 the ready flag stays set once 32 bits have been collected,
 * while the number register goes on shifting new bits in. A read that follows
 * another within about a microsecond would return some of the same bits.
 * CFG_RDY_CLR is a level: it holds the flag and the register at zero. After
 * its release the flag says that 32 bits have come in which nobody has read.
 */
static void ingenic_trng_restart(struct ingenic_trng *trng)
{
	unsigned int ctrl;

	ctrl = readl(trng->base + TRNG_REG_CFG_OFFSET);
	writel(ctrl | CFG_RDY_CLR, trng->base + TRNG_REG_CFG_OFFSET);
	writel(ctrl & ~CFG_RDY_CLR, trng->base + TRNG_REG_CFG_OFFSET);
}

static int ingenic_trng_init(struct hwrng *rng)
{
	struct ingenic_trng *trng = container_of(rng, struct ingenic_trng, rng);
	unsigned int ctrl;

	ctrl = readl(trng->base + TRNG_REG_CFG_OFFSET);
	ctrl |= CFG_GEN_EN;
	writel(ctrl, trng->base + TRNG_REG_CFG_OFFSET);

	if (trng->version >= ID_X2000)
		ingenic_trng_restart(trng);

	return 0;
}

static void ingenic_trng_cleanup(struct hwrng *rng)
{
	struct ingenic_trng *trng = container_of(rng, struct ingenic_trng, rng);
	unsigned int ctrl;

	ctrl = readl(trng->base + TRNG_REG_CFG_OFFSET);
	ctrl &= ~CFG_GEN_EN;
	writel(ctrl, trng->base + TRNG_REG_CFG_OFFSET);
}

static int ingenic_trng_read(struct hwrng *rng, void *buf, size_t max, bool wait)
{
	struct ingenic_trng *trng = container_of(rng, struct ingenic_trng, rng);
	u32 *data = buf;
	u32 status;
	int ret;

	ret = readl_poll_timeout(trng->base + TRNG_REG_STATUS_OFFSET, status,
				 status & STATUS_RANDOM_RDY, 10, 1000);
	if (ret == -ETIMEDOUT) {
		pr_err("%s: Wait for DTRNG data ready timeout\n", __func__);
		return ret;
	}

	*data = readl(trng->base + TRNG_REG_RANDOMNUM_OFFSET);

	if (trng->version >= ID_X2000)
		ingenic_trng_restart(trng);

	return 4;
}

static int ingenic_trng_probe(struct platform_device *pdev)
{
	struct ingenic_trng *trng;
	struct clk *clk;
	int ret;

	trng = devm_kzalloc(&pdev->dev, sizeof(*trng), GFP_KERNEL);
	if (!trng)
		return -ENOMEM;

	trng->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(trng->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(trng->base),
				     "%s: Failed to map DTRNG registers\n", __func__);

	clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk),
				     "%s: Cannot get and enable DTRNG clock\n", __func__);

	trng->version = (uintptr_t)device_get_match_data(&pdev->dev);

	trng->rng.name = pdev->name;
	trng->rng.init = ingenic_trng_init;
	trng->rng.cleanup = ingenic_trng_cleanup;
	trng->rng.read = ingenic_trng_read;
	if (trng->version >= ID_X2000)
		trng->rng.quality = X2000_QUALITY;

	ret = devm_hwrng_register(&pdev->dev, &trng->rng);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to register hwrng\n");

	platform_set_drvdata(pdev, trng);

	dev_info(&pdev->dev, "Ingenic DTRNG driver registered\n");
	return 0;
}

static const struct of_device_id ingenic_trng_of_match[] = {
	{ .compatible = "ingenic,x1830-dtrng", .data = (void *)ID_X1830 },
	{ .compatible = "ingenic,x2000-dtrng", .data = (void *)ID_X2000 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ingenic_trng_of_match);

static struct platform_driver ingenic_trng_driver = {
	.probe		= ingenic_trng_probe,
	.driver		= {
		.name	= "ingenic-trng",
		.of_match_table = ingenic_trng_of_match,
	},
};

module_platform_driver(ingenic_trng_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("漆鹏振 (Qi Pengzhen) <aric.pzqi@ingenic.com>");
MODULE_AUTHOR("周琰杰 (Zhou Yanjie) <zhouyanjie@wanyeetech.com>");
MODULE_DESCRIPTION("Ingenic True Random Number Generator driver");
