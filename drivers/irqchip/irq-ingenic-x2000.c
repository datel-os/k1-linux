// SPDX-License-Identifier: GPL-2.0
/*
 * The interrupt controller of the Ingenic X2000
 *
 * Each CPU has a block of registers of its own, with a mask bit for every one
 * of the SoC's 64 interrupt sources, and raises its interrupt line while a
 * source it has not masked is active. An interrupt is given to a CPU by
 * unmasking it there and nowhere else.
 *
 * A CPU's block is written only while that CPU is online. The block of a CPU
 * that has not started yet is left as the CPU's reset leaves it; a source it
 * should have unmasked then is masked the first time that CPU sees it.
 */

#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#define X2000_INTC_WORDS	2
#define X2000_INTC_IRQS		(X2000_INTC_WORDS * 32)

/* A CPU's registers for the 32 sources of a word */
#define X2000_INTC_WORD(w)	((w) * 0x20)
#define X2000_INTC_ICMR		0x04	/* the sources that are masked */
#define X2000_INTC_ICMSR	0x08	/* write 1 to mask */
#define X2000_INTC_ICMCR	0x0c	/* write 1 to unmask */
#define X2000_INTC_ICPR		0x10	/* active and not masked */

struct x2000_intc {
	raw_spinlock_t lock;
	struct irq_domain *domain;
	void __iomem *base[NR_CPUS];
	/* the CPU each interrupt is given to */
	u8 cpu[X2000_INTC_IRQS];
};

static void x2000_intc_handle(struct irq_desc *desc)
{
	struct x2000_intc *intc = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int cpu = smp_processor_id();
	void __iomem *base = intc->base[cpu];
	unsigned int word, bit;

	chained_irq_enter(chip, desc);

	for (word = 0; word < X2000_INTC_WORDS; word++) {
		void __iomem *regs = base + X2000_INTC_WORD(word);
		unsigned long pending = readl(regs + X2000_INTC_ICPR);

		for_each_set_bit(bit, &pending, 32) {
			/*
			 * Not this CPU's: it has just been given to another,
			 * or this CPU's block had it unmasked from the start.
			 * It is the other CPU's to handle; here it is masked.
			 */
			if (unlikely(intc->cpu[word * 32 + bit] != cpu)) {
				writel(BIT(bit), regs + X2000_INTC_ICMSR);
				continue;
			}

			generic_handle_domain_irq(intc->domain, word * 32 + bit);
		}
	}

	chained_irq_exit(chip, desc);
}

static void __iomem *x2000_intc_reg(struct x2000_intc *intc,
				    irq_hw_number_t hwirq, unsigned int reg)
{
	return intc->base[intc->cpu[hwirq]] + X2000_INTC_WORD(hwirq / 32) + reg;
}

static void x2000_intc_mask(struct irq_data *d)
{
	struct x2000_intc *intc = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&intc->lock, flags);
	writel(BIT(d->hwirq % 32), x2000_intc_reg(intc, d->hwirq, X2000_INTC_ICMSR));
	raw_spin_unlock_irqrestore(&intc->lock, flags);
}

static void x2000_intc_unmask(struct irq_data *d)
{
	struct x2000_intc *intc = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&intc->lock, flags);
	writel(BIT(d->hwirq % 32), x2000_intc_reg(intc, d->hwirq, X2000_INTC_ICMCR));
	raw_spin_unlock_irqrestore(&intc->lock, flags);
}

#ifdef CONFIG_SMP
static int x2000_intc_set_affinity(struct irq_data *d,
				   const struct cpumask *dest, bool force)
{
	struct x2000_intc *intc = irq_data_get_irq_chip_data(d);
	unsigned int cpu = cpumask_any_and(dest, cpu_online_mask);
	u32 bit = BIT(d->hwirq % 32);
	unsigned long flags;
	bool masked;

	if (cpu >= nr_cpu_ids || !intc->base[cpu])
		return -EINVAL;

	raw_spin_lock_irqsave(&intc->lock, flags);
	masked = readl(x2000_intc_reg(intc, d->hwirq, X2000_INTC_ICMR)) & bit;
	writel(bit, x2000_intc_reg(intc, d->hwirq, X2000_INTC_ICMSR));
	intc->cpu[d->hwirq] = cpu;
	if (!masked)
		writel(bit, x2000_intc_reg(intc, d->hwirq, X2000_INTC_ICMCR));
	raw_spin_unlock_irqrestore(&intc->lock, flags);

	irq_data_update_effective_affinity(d, cpumask_of(cpu));

	return IRQ_SET_MASK_OK;
}
#endif

static struct irq_chip x2000_intc_chip = {
	.name			= "x2000-intc",
	.irq_mask		= x2000_intc_mask,
	.irq_unmask		= x2000_intc_unmask,
#ifdef CONFIG_SMP
	.irq_set_affinity	= x2000_intc_set_affinity,
#endif
	.flags			= IRQCHIP_SKIP_SET_WAKE,
};

static int x2000_intc_map(struct irq_domain *domain, unsigned int virq,
			  irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(virq, &x2000_intc_chip, handle_level_irq);
	irq_set_chip_data(virq, domain->host_data);
	irqd_set_single_target(irq_get_irq_data(virq));

	return 0;
}

static const struct irq_domain_ops x2000_intc_domain_ops = {
	.map	= x2000_intc_map,
	.xlate	= irq_domain_xlate_onecell,
};

static int __init x2000_intc_of_init(struct device_node *node,
				     struct device_node *parent)
{
	struct x2000_intc *intc;
	unsigned int cpu, word, parent_irq;
	int ret;

	intc = kzalloc(sizeof(*intc), GFP_KERNEL);
	if (!intc)
		return -ENOMEM;

	raw_spin_lock_init(&intc->lock);

	/* one block of registers for each CPU, in the CPUs' order */
	for_each_possible_cpu(cpu) {
		intc->base[cpu] = of_iomap(node, cpu);
		if (!intc->base[cpu])
			break;
	}

	if (!intc->base[0]) {
		pr_err("%pOF: failed to map the boot CPU's registers\n", node);
		ret = -ENOMEM;
		goto err_free;
	}

	parent_irq = irq_of_parse_and_map(node, 0);
	if (!parent_irq) {
		pr_err("%pOF: failed to map the CPU's interrupt\n", node);
		ret = -EINVAL;
		goto err_unmap;
	}

	/* Every interrupt is the boot CPU's, and masked, until it is asked for */
	for (word = 0; word < X2000_INTC_WORDS; word++)
		writel(~0U, intc->base[0] + X2000_INTC_WORD(word) + X2000_INTC_ICMSR);

	intc->domain = irq_domain_create_linear(of_fwnode_handle(node),
						X2000_INTC_IRQS,
						&x2000_intc_domain_ops, intc);
	if (!intc->domain) {
		pr_err("%pOF: failed to add the interrupt domain\n", node);
		ret = -ENOMEM;
		goto err_unmap;
	}

	irq_set_chained_handler_and_data(parent_irq, x2000_intc_handle, intc);

	return 0;

err_unmap:
	for_each_possible_cpu(cpu)
		if (intc->base[cpu])
			iounmap(intc->base[cpu]);
err_free:
	kfree(intc);
	return ret;
}

IRQCHIP_DECLARE(x2000_intc, "ingenic,x2000-intc", x2000_intc_of_init);
