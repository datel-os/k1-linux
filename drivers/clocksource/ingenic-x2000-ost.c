// SPDX-License-Identifier: GPL-2.0
/*
 * The operating system timers of the Ingenic X2000
 *
 * One 64 bit counter for the whole SoC, which is the clock source and the
 * scheduler's clock, and for each CPU a block of registers with a 32 bit
 * counter that is compared with a value: that CPU's event timer. An event
 * timer's interrupt is one of its CPU's own interrupt lines.
 *
 * The registers are those of the timer driver in Ingenic's SDK kernel
 * (drivers/clocksource/ingenic_core_ost.c).
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/cpuhotplug.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/percpu.h>
#include <linux/sched_clock.h>

/* the counter of the whole SoC */
#define OST_G_CCR	0x00	/* prescaler: 0 for the clock as it comes */
#define OST_G_ER	0x04	/* enable */
#define OST_G_CR	0x08	/* write 1 to clear the counter */
#define OST_G_CNTH	0x0c
#define OST_G_CNTL	0x10

/* a CPU's event timer */
#define OST_CCR		0x00	/* prescaler */
#define OST_ER		0x04	/* enable */
#define OST_CR		0x08	/* write 1 to clear the counter */
#define OST_FR		0x0c	/* flag: the counter has reached the value */
#define OST_MR		0x10	/* mask of the flag's interrupt */
#define OST_DFR		0x14	/* the value */

#define OST_MIN_DELTA	0xf

struct x2000_ost_event {
	struct clock_event_device cevt;
	void __iomem *base;
};

static DEFINE_PER_CPU(struct x2000_ost_event, x2000_ost_events);
static void __iomem *x2000_ost_global;
static unsigned long x2000_ost_rate;

static u64 notrace x2000_ost_read(void)
{
	u32 hi, lo;

	/* The halves are read apart: once more if the upper one has moved */
	do {
		hi = readl(x2000_ost_global + OST_G_CNTH);
		lo = readl(x2000_ost_global + OST_G_CNTL);
	} while (hi != readl(x2000_ost_global + OST_G_CNTH));

	return (u64)hi << 32 | lo;
}

static u64 x2000_ost_clocksource_read(struct clocksource *cs)
{
	return x2000_ost_read();
}

static struct clocksource x2000_ost_clocksource = {
	.name	= "ingenic-x2000-ost",
	.rating	= 400,
	.read	= x2000_ost_clocksource_read,
	.mask	= CLOCKSOURCE_MASK(64),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int x2000_ost_set_next_event(unsigned long delta,
				    struct clock_event_device *cevt)
{
	struct x2000_ost_event *evt =
		container_of(cevt, struct x2000_ost_event, cevt);

	/*
	 * The timer is stopped first and its flag cleared: the flag of the
	 * event this one replaces would stop the new one in the handler.
	 */
	writel(0, evt->base + OST_ER);
	writel(0, evt->base + OST_FR);
	writel(delta, evt->base + OST_DFR);
	writel(1, evt->base + OST_CR);
	writel(1, evt->base + OST_ER);

	return 0;
}

static int x2000_ost_shutdown(struct clock_event_device *cevt)
{
	struct x2000_ost_event *evt =
		container_of(cevt, struct x2000_ost_event, cevt);

	writel(0, evt->base + OST_ER);

	return 0;
}

static irqreturn_t x2000_ost_interrupt(int irq, void *dev_id)
{
	struct x2000_ost_event *evt = this_cpu_ptr(&x2000_ost_events);

	if (!readl(evt->base + OST_FR))
		return IRQ_NONE;

	/* One event: left running, the counter would reach the value again */
	writel(0, evt->base + OST_ER);
	writel(0, evt->base + OST_FR);

	evt->cevt.event_handler(&evt->cevt);

	return IRQ_HANDLED;
}

static int x2000_ost_starting_cpu(unsigned int cpu)
{
	struct x2000_ost_event *evt = this_cpu_ptr(&x2000_ost_events);

	if (!evt->base)
		return -ENODEV;

	writel(0, evt->base + OST_ER);
	writel(0, evt->base + OST_CCR);
	writel(1, evt->base + OST_CR);
	writel(0, evt->base + OST_FR);
	writel(0, evt->base + OST_MR);

	evt->cevt.name = "ingenic-x2000-ost";
	evt->cevt.features = CLOCK_EVT_FEAT_ONESHOT;
	evt->cevt.rating = 400;
	evt->cevt.cpumask = cpumask_of(cpu);
	evt->cevt.set_next_event = x2000_ost_set_next_event;
	evt->cevt.set_state_shutdown = x2000_ost_shutdown;
	evt->cevt.set_state_oneshot_stopped = x2000_ost_shutdown;

	clockevents_config_and_register(&evt->cevt, x2000_ost_rate,
					OST_MIN_DELTA, 0xffffffff);

	return 0;
}

static int x2000_ost_dying_cpu(unsigned int cpu)
{
	struct x2000_ost_event *evt = this_cpu_ptr(&x2000_ost_events);

	writel(0, evt->base + OST_ER);
	writel(1, evt->base + OST_MR);

	return 0;
}

static int __init x2000_ost_init(struct device_node *np)
{
	unsigned int cpu;
	struct clk *clk;
	int irq, ret;

	/* the SoC's counter, then one block for each CPU, in the CPUs' order */
	x2000_ost_global = of_iomap(np, 0);
	if (!x2000_ost_global) {
		pr_err("%pOF: failed to map the counter's registers\n", np);
		return -ENOMEM;
	}

	for_each_possible_cpu(cpu) {
		per_cpu(x2000_ost_events, cpu).base = of_iomap(np, 1 + cpu);
		if (!per_cpu(x2000_ost_events, cpu).base)
			pr_warn("%pOF: no event timer for CPU %u\n", np, cpu);
	}

	if (!per_cpu(x2000_ost_events, 0).base) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	clk = of_clk_get(np, 0);
	if (IS_ERR(clk)) {
		pr_err("%pOF: failed to get the clock\n", np);
		ret = PTR_ERR(clk);
		goto err_unmap;
	}

	ret = clk_prepare_enable(clk);
	if (ret)
		goto err_put_clk;

	x2000_ost_rate = clk_get_rate(clk);
	if (!x2000_ost_rate) {
		ret = -EINVAL;
		goto err_disable_clk;
	}

	irq = of_irq_get(np, 0);
	if (irq <= 0) {
		pr_err("%pOF: failed to get the interrupt\n", np);
		ret = irq ? irq : -EINVAL;
		goto err_disable_clk;
	}

	/* the counter: from zero, at the clock's own rate */
	writel(0, x2000_ost_global + OST_G_ER);
	writel(0, x2000_ost_global + OST_G_CCR);
	writel(1, x2000_ost_global + OST_G_CR);
	writel(1, x2000_ost_global + OST_G_ER);

	ret = clocksource_register_hz(&x2000_ost_clocksource, x2000_ost_rate);
	if (ret)
		goto err_disable_clk;

	/*
	 * The line is each CPU's own: one request serves them all, and the
	 * handler finds its CPU's timer by itself.
	 */
	ret = request_irq(irq, x2000_ost_interrupt, IRQF_PERCPU | IRQF_TIMER,
			  "ingenic-x2000-ost", &x2000_ost_events);
	if (ret) {
		pr_err("%pOF: failed to request the interrupt\n", np);
		goto err_clocksource;
	}

	ret = cpuhp_setup_state(CPUHP_AP_INGENIC_X2000_OST_STARTING,
				"clockevents/ingenic/x2000-ost:starting",
				x2000_ost_starting_cpu, x2000_ost_dying_cpu);
	if (ret)
		goto err_free_irq;

	/* Last: there is no way to undo it */
	sched_clock_register(x2000_ost_read, 64, x2000_ost_rate);

	return 0;

err_free_irq:
	free_irq(irq, &x2000_ost_events);
err_clocksource:
	clocksource_unregister(&x2000_ost_clocksource);
err_disable_clk:
	clk_disable_unprepare(clk);
err_put_clk:
	clk_put(clk);
err_unmap:
	for_each_possible_cpu(cpu)
		if (per_cpu(x2000_ost_events, cpu).base)
			iounmap(per_cpu(x2000_ost_events, cpu).base);
	iounmap(x2000_ost_global);

	return ret;
}

TIMER_OF_DECLARE(x2000_ost, "ingenic,x2000-ost", x2000_ost_init);
