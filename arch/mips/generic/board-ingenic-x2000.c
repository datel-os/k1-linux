// SPDX-License-Identifier: GPL-2.0
/*
 * What a board with an Ingenic X2000 needs of the SoC's clock and power
 * block besides its clocks, which have a driver:
 *
 * - the supply of two pin banks, 1.8 V or 3.3 V, which the board's device
 *   tree states in the pin controller's node;
 * - a word that outlives a reset. It is written before a restart, so that
 *   the next boot (and the boot loader, for "recovery") can tell a restart
 *   from the watchdog's reset; the reset itself is the watchdog driver's.
 *
 * And the power-off, which is the RTC block's hibernation with the times
 * Ingenic's SDK kernel sets for it. rtc-jz4740 can power a JZ4780 off, but it
 * computes those two times for that SoC's registers, and the X2000's count
 * in other units.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irqflags.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/string.h>

#define CPM_RSR			0x08	/* what has reset the SoC */
#define CPM_CPPSR		0x34	/* the word that outlives a reset */
#define CPM_CPSPPR		0x38	/* opens CPPSR for a write */
#define CPM_EXCLK_DS		0xe0

#define RSR_POWER_ON		BIT(0)
#define RSR_WATCHDOG		BIT(1)
#define RSR_P0			BIT(2)

#define CPSPPR_OPEN		0x5a5a
#define CPPSR_RESTART		0x003535
#define CPPSR_RECOVERY		0x001a1a

#define EXCLK_DS_PA_1V8		BIT(30)
#define EXCLK_DS_PE_MSC_1V8	BIT(31)

/* the RTC block: every write is opened first, and waited for */
#define RTC_RTCCR		0x00
#define RTC_HCR			0x20	/* 1: hibernate */
#define RTC_HWFCR		0x24	/* how long the wake-up pin has to be low */
#define RTC_HRCR		0x28	/* how long reset is held after a wake-up */
#define RTC_HWCR		0x2c
#define RTC_HWRSR		0x30
#define RTC_WENR		0x3c

#define RTCCR_WRDY		BIT(7)
#define RTCCR_RTCE		BIT(0)
#define WENR_OPEN		0xa55a
#define WENR_WEN		BIT(31)

/* 1000 ms and 125 ms, in the registers' units */
#define HWFCR_1000_MS		(((0x7ff * 1000) / 2000) << 5)
#define HRCR_125_MS		((125 / 125) << 11)

static void __iomem *x2000_cpm;
static void __iomem *x2000_rtc;

static int x2000_rtc_wait(unsigned int reg, u32 bit)
{
	u32 val;

	return readl_poll_timeout_atomic(x2000_rtc + reg, val, val & bit, 10,
					 100 * USEC_PER_MSEC);
}

static void x2000_rtc_write(unsigned int reg, u32 val)
{
	if (x2000_rtc_wait(RTC_RTCCR, RTCCR_WRDY))
		return;

	writel(WENR_OPEN, x2000_rtc + RTC_WENR);
	x2000_rtc_wait(RTC_RTCCR, RTCCR_WRDY);
	x2000_rtc_wait(RTC_WENR, WENR_WEN);
	x2000_rtc_wait(RTC_RTCCR, RTCCR_WRDY);

	writel(val, x2000_rtc + reg);
	x2000_rtc_wait(RTC_RTCCR, RTCCR_WRDY);
}

static int x2000_power_off(struct sys_off_data *data)
{
	local_irq_disable();

	x2000_rtc_write(RTC_HWFCR, HWFCR_1000_MS);
	x2000_rtc_write(RTC_HRCR, HRCR_125_MS);
	x2000_rtc_write(RTC_HWRSR, 0);
	x2000_rtc_write(RTC_HWCR, 0);
	x2000_rtc_write(RTC_RTCCR, readl(x2000_rtc + RTC_RTCCR) | RTCCR_RTCE);
	x2000_rtc_write(RTC_HCR, 1);

	mdelay(200);
	pr_emerg("x2000: still running after the power-off\n");

	return NOTIFY_DONE;
}

static void x2000_cppsr_write(u32 val)
{
	writel(CPSPPR_OPEN, x2000_cpm + CPM_CPSPPR);
	writel(val, x2000_cpm + CPM_CPPSR);
	writel(0, x2000_cpm + CPM_CPSPPR);
}

static int x2000_restart_mark(struct sys_off_data *data)
{
	if (data->cmd && !strcmp(data->cmd, "recovery")) {
		/* the boot loader acts on this one: until it reads back */
		do {
			x2000_cppsr_write(CPPSR_RECOVERY);
			udelay(100);
		} while (readl(x2000_cpm + CPM_CPPSR) != CPPSR_RECOVERY);
	} else {
		x2000_cppsr_write(CPPSR_RESTART);
	}

	/* the watchdog's handler, next in the chain, resets the SoC */
	return NOTIFY_DONE;
}

static void __init x2000_reset_cause(void)
{
	u32 rsr = readl(x2000_cpm + CPM_RSR);
	u32 cppsr = readl(x2000_cpm + CPM_CPPSR);

	writel(0, x2000_cpm + CPM_RSR);
	x2000_cppsr_write(0);

	pr_info("x2000: reset status RSR=0x%08x%s%s%s CPPSR=0x%08x\n", rsr,
		(rsr & RSR_POWER_ON) ? " power-on" : "",
		!(rsr & RSR_WATCHDOG) ? "" :
			cppsr == CPPSR_RESTART ? " restart" : " watchdog",
		(rsr & RSR_P0) ? " p0-reset" : "",
		cppsr);
}

static u32 __init x2000_bank_supply(struct device_node *np, const char *prop,
				    u32 bit, u32 val)
{
	u32 uv;

	if (of_property_read_u32(np, prop, &uv))
		return val;
	if (uv == 1800000)
		return val | bit;
	if (uv == 3300000)
		return val & ~bit;

	pr_warn("x2000: %s is %u: neither 1.8 V nor 3.3 V, the bank is left as it is\n",
		prop, uv);
	return val;
}

static void __init x2000_bank_supplies(void)
{
	struct device_node *np;
	u32 was, val;

	np = of_find_compatible_node(NULL, NULL, "ingenic,x2000-pinctrl");
	if (!np)
		return;

	was = readl(x2000_cpm + CPM_EXCLK_DS);
	val = x2000_bank_supply(np, "ingenic,gpa-microvolt", EXCLK_DS_PA_1V8, was);
	val = x2000_bank_supply(np, "ingenic,gpe-msc-microvolt",
				EXCLK_DS_PE_MSC_1V8, val);
	of_node_put(np);

	if (val != was)
		writel(val, x2000_cpm + CPM_EXCLK_DS);

	pr_info("x2000: pin bank supplies: EXCLK_DS 0x%08x, was 0x%08x\n", val, was);
}

static int __init x2000_board_init(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "ingenic,x2000-cgu");
	if (!np)
		return 0;

	x2000_cpm = of_iomap(np, 0);
	of_node_put(np);
	if (!x2000_cpm)
		return -ENOMEM;

	x2000_bank_supplies();
	x2000_reset_cause();

	if (IS_ERR(register_sys_off_handler(SYS_OFF_MODE_RESTART,
					    SYS_OFF_PRIO_HIGH,
					    x2000_restart_mark, NULL)))
		pr_warn("x2000: a restart will read as a watchdog reset\n");

	np = of_find_compatible_node(NULL, NULL, "ingenic,x2000-rtc");
	if (np) {
		x2000_rtc = of_iomap(np, 0);
		of_node_put(np);
	}
	if (x2000_rtc)
		register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
					 SYS_OFF_PRIO_DEFAULT,
					 x2000_power_off, NULL);

	return 0;
}
arch_initcall(x2000_board_init);
