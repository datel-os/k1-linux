// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (C) 2010, Paul Cercueil <paul@crapouillou.net>
 *  JZ4740 Watchdog driver
 */

#include <linux/mfd/ingenic-tcu.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/watchdog.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define DEFAULT_HEARTBEAT 5
#define MAX_HEARTBEAT     2048

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static unsigned int heartbeat = DEFAULT_HEARTBEAT;
module_param(heartbeat, uint, 0);
MODULE_PARM_DESC(heartbeat,
		"Watchdog heartbeat period in seconds from 1 to "
		__MODULE_STRING(MAX_HEARTBEAT) ", default "
		__MODULE_STRING(DEFAULT_HEARTBEAT));

/*
 * The X2000's watchdog differs in two things. Its counter cannot be written:
 * a bit of the control register clears it. And its data register holds a
 * second compare value in the upper half; the counter reaching that one sets
 * a flag in the TCU's flag register and, unless masked, raises the TCU's
 * interrupt. That is a pre-timeout.
 */
#define X2000_WDT_TCSR_CLRZ	BIT(10)
#define X2000_WDT_TDR_EARLY_LSB	16
#define X2000_TCU_WDT_FLAG	BIT(24)

enum jz4740_wdt_version {
	ID_JZ4740,
	ID_X2000,
};

struct jz4740_wdt_drvdata {
	struct watchdog_device wdt;
	struct watchdog_info info;
	struct regmap *map;
	struct clk *clk;
	unsigned long clk_rate;
	enum jz4740_wdt_version version;
};

static void jz4740_wdt_clear_counter(struct jz4740_wdt_drvdata *drvdata)
{
	if (drvdata->version >= ID_X2000)
		regmap_set_bits(drvdata->map, TCU_REG_WDT_TCSR,
				X2000_WDT_TCSR_CLRZ);
	else
		regmap_write(drvdata->map, TCU_REG_WDT_TCNT, 0);
}

/* With the counter at zero: let the early compare interrupt again, or not. */
static void jz4740_wdt_x2000_arm_pretimeout(struct jz4740_wdt_drvdata *drvdata,
					    bool on)
{
	regmap_write(drvdata->map, TCU_REG_TMSR, X2000_TCU_WDT_FLAG);
	regmap_write(drvdata->map, TCU_REG_TFCR, X2000_TCU_WDT_FLAG);
	if (on)
		regmap_write(drvdata->map, TCU_REG_TMCR, X2000_TCU_WDT_FLAG);
}

static bool jz4740_wdt_has_pretimeout(struct watchdog_device *wdt_dev)
{
	return (wdt_dev->info->options & WDIOF_PRETIMEOUT) &&
	       wdt_dev->pretimeout && wdt_dev->pretimeout < wdt_dev->timeout;
}

static int jz4740_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct jz4740_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);

	jz4740_wdt_clear_counter(drvdata);

	/* A pre-timeout that a governor let pass masked itself. */
	if (drvdata->version >= ID_X2000 && jz4740_wdt_has_pretimeout(wdt_dev))
		jz4740_wdt_x2000_arm_pretimeout(drvdata, true);

	return 0;
}

static void jz4740_wdt_program(struct watchdog_device *wdt_dev,
			       unsigned int new_timeout)
{
	struct jz4740_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);
	u16 timeout_value = (u16)(drvdata->clk_rate * new_timeout);
	u32 tdr = timeout_value;
	unsigned int tcer;

	regmap_read(drvdata->map, TCU_REG_WDT_TCER, &tcer);
	regmap_write(drvdata->map, TCU_REG_WDT_TCER, 0);

	wdt_dev->timeout = new_timeout;

	if (drvdata->version >= ID_X2000) {
		bool pretimeout = jz4740_wdt_has_pretimeout(wdt_dev);
		u16 early = timeout_value;

		if (pretimeout)
			early = (u16)(drvdata->clk_rate *
				      (new_timeout - wdt_dev->pretimeout));
		tdr |= (u32)early << X2000_WDT_TDR_EARLY_LSB;

		regmap_write(drvdata->map, TCU_REG_WDT_TDR, tdr);
		jz4740_wdt_clear_counter(drvdata);
		jz4740_wdt_x2000_arm_pretimeout(drvdata, pretimeout);
	} else {
		regmap_write(drvdata->map, TCU_REG_WDT_TDR, tdr);
		jz4740_wdt_clear_counter(drvdata);
	}

	if (tcer & TCU_WDT_TCER_TCEN)
		regmap_write(drvdata->map, TCU_REG_WDT_TCER, TCU_WDT_TCER_TCEN);
}

static int jz4740_wdt_set_timeout(struct watchdog_device *wdt_dev,
				  unsigned int new_timeout)
{
	struct jz4740_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);

	/*
	 * The X2000's counter is cleared through the control register, which
	 * also holds the clock's settings and is not to be touched while that
	 * clock is stopped. start() programs the timeout anyway.
	 */
	if (drvdata->version >= ID_X2000 && !watchdog_active(wdt_dev)) {
		wdt_dev->timeout = new_timeout;
		return 0;
	}

	jz4740_wdt_program(wdt_dev, new_timeout);

	return 0;
}

static int jz4740_wdt_set_pretimeout(struct watchdog_device *wdt_dev,
				     unsigned int new_pretimeout)
{
	wdt_dev->pretimeout = new_pretimeout;

	if (watchdog_active(wdt_dev))
		jz4740_wdt_program(wdt_dev, wdt_dev->timeout);

	return 0;
}

static irqreturn_t jz4740_wdt_x2000_irq(int irq, void *dev_id)
{
	struct jz4740_wdt_drvdata *drvdata = dev_id;
	unsigned int flags;

	regmap_read(drvdata->map, TCU_REG_TFR, &flags);
	if (!(flags & X2000_TCU_WDT_FLAG))
		return IRQ_NONE;

	/* Once per count: the flag stays up until the counter is cleared. */
	jz4740_wdt_x2000_arm_pretimeout(drvdata, false);

	/*
	 * A restart through the watchdog sets a timeout of zero, which the
	 * early compare matches at once. That is the restart, not a hang.
	 */
	if (system_state > SYSTEM_RUNNING)
		return IRQ_HANDLED;

	watchdog_notify_pretimeout(&drvdata->wdt);

	return IRQ_HANDLED;
}

static int jz4740_wdt_start(struct watchdog_device *wdt_dev)
{
	struct jz4740_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);
	unsigned int tcer;
	int ret;

	ret = clk_prepare_enable(drvdata->clk);
	if (ret)
		return ret;

	regmap_read(drvdata->map, TCU_REG_WDT_TCER, &tcer);

	jz4740_wdt_program(wdt_dev, wdt_dev->timeout);

	/* Start watchdog if it wasn't started already */
	if (!(tcer & TCU_WDT_TCER_TCEN))
		regmap_write(drvdata->map, TCU_REG_WDT_TCER, TCU_WDT_TCER_TCEN);

	return 0;
}

static int jz4740_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct jz4740_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);

	regmap_write(drvdata->map, TCU_REG_WDT_TCER, 0);
	if (drvdata->version >= ID_X2000)
		jz4740_wdt_x2000_arm_pretimeout(drvdata, false);
	clk_disable_unprepare(drvdata->clk);

	return 0;
}

static int jz4740_wdt_restart(struct watchdog_device *wdt_dev,
			      unsigned long action, void *data)
{
	wdt_dev->timeout = 0;
	jz4740_wdt_start(wdt_dev);
	return 0;
}

static const struct watchdog_info jz4740_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "jz4740 Watchdog",
};

static const struct watchdog_ops jz4740_wdt_ops = {
	.owner = THIS_MODULE,
	.start = jz4740_wdt_start,
	.stop = jz4740_wdt_stop,
	.ping = jz4740_wdt_ping,
	.set_timeout = jz4740_wdt_set_timeout,
	.set_pretimeout = jz4740_wdt_set_pretimeout,
	.restart = jz4740_wdt_restart,
};

#ifdef CONFIG_OF
static const struct of_device_id jz4740_wdt_of_matches[] = {
	{ .compatible = "ingenic,jz4740-watchdog", },
	{ .compatible = "ingenic,jz4780-watchdog", },
	{ .compatible = "ingenic,x2000-watchdog", .data = (void *)ID_X2000 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, jz4740_wdt_of_matches);
#endif

static int jz4740_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct jz4740_wdt_drvdata *drvdata;
	struct watchdog_device *jz4740_wdt;
	long rate;
	int ret;

	drvdata = devm_kzalloc(dev, sizeof(struct jz4740_wdt_drvdata),
			       GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->clk = devm_clk_get(&pdev->dev, "wdt");
	if (IS_ERR(drvdata->clk)) {
		dev_err(&pdev->dev, "cannot find WDT clock\n");
		return PTR_ERR(drvdata->clk);
	}

	/* Set smallest clock possible */
	rate = clk_round_rate(drvdata->clk, 1);
	if (rate < 0)
		return rate;

	ret = clk_set_rate(drvdata->clk, rate);
	if (ret)
		return ret;

	drvdata->clk_rate = rate;
	drvdata->version = (uintptr_t)device_get_match_data(dev);
	drvdata->info = jz4740_wdt_info;
	jz4740_wdt = &drvdata->wdt;
	jz4740_wdt->info = &drvdata->info;
	jz4740_wdt->ops = &jz4740_wdt_ops;
	jz4740_wdt->min_timeout = 1;
	jz4740_wdt->max_timeout = 0xffff / rate;
	jz4740_wdt->timeout = clamp(heartbeat,
				    jz4740_wdt->min_timeout,
				    jz4740_wdt->max_timeout);
	jz4740_wdt->parent = dev;
	watchdog_set_nowayout(jz4740_wdt, nowayout);
	watchdog_set_drvdata(jz4740_wdt, drvdata);

	drvdata->map = device_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(drvdata->map)) {
		dev_err(dev, "regmap not found\n");
		return PTR_ERR(drvdata->map);
	}

	if (drvdata->version >= ID_X2000) {
		int irq = platform_get_irq_optional(pdev, 0);

		if (irq == -EPROBE_DEFER)
			return irq;

		jz4740_wdt_x2000_arm_pretimeout(drvdata, false);

		if (irq > 0) {
			ret = devm_request_irq(dev, irq, jz4740_wdt_x2000_irq,
					       IRQF_SHARED, dev_name(dev),
					       drvdata);
			if (ret)
				return ret;

			drvdata->info.options |= WDIOF_PRETIMEOUT;
		}
	}

	return devm_watchdog_register_device(dev, &drvdata->wdt);
}

static struct platform_driver jz4740_wdt_driver = {
	.probe = jz4740_wdt_probe,
	.driver = {
		.name = "jz4740-wdt",
		.of_match_table = of_match_ptr(jz4740_wdt_of_matches),
	},
};

module_platform_driver(jz4740_wdt_driver);

MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_DESCRIPTION("jz4740 Watchdog Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:jz4740-wdt");
