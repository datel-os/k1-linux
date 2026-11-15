// SPDX-License-Identifier: GPL-2.0
/*
 * SMP on the Ingenic XBurst2 (X2000)
 *
 * The CPUs are started and sent interrupts through the SoC's core control
 * unit: for each CPU a reset bit and a mailbox, and three masks that say
 * which CPUs the devices', the mailboxes' and the timers' interrupts reach.
 *
 * From the SMP code of Ingenic's SDK kernel (arch/mips/xburst2/core/smp.c),
 * without its operations for taking a CPU offline: on the Creality K1 that
 * code stops a CPU, but the machine freezes when the CPU's reset is released
 * again, so a CPU that went offline could not come back. How a thread of this
 * core is restarted while the other runs is not known, and the X2000 does not
 * offer CPU hotplug until it is.
 */

#include <linux/bits.h>
#include <linux/cpumask.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/printk.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/barrier.h>
#include <asm/mipsregs.h>
#include <asm/processor.h>
#include <asm/smp-ops.h>
#include <asm/uasm.h>

/* the core control unit; a bit for each CPU in every register but the last */
#define CCU_CSRR	0x0040	/* set: the CPU is held in reset */
#define CCU_PIMR	0x0120	/* set: the devices' interrupts reach the CPU */
#define CCU_MIMR	0x0160	/* set: its mailbox's interrupt reaches the CPU */
#define CCU_OIMR	0x01a0	/* set: its timer's interrupt reaches the CPU */
#define CCU_RER		0x0f00	/* where a CPU starts when its reset ends */
#define CCU_MBR(cpu)	(0x1000 + 4 * (cpu))

/* in the start code */
#define REG_V0		2
#define REG_V1		3
#define REG_GP		28
#define REG_SP		29
#define REG_RA		31

static struct {
	struct device_node *np;
	void __iomem *ccu;
	raw_spinlock_t mbox_lock[NR_CPUS];
	unsigned long entry;	/* the start code */
	unsigned long sp, gp;	/* of the CPU that is being started */
} xburst2_smp;

static void ccu_set(unsigned int reg, unsigned int cpu)
{
	writel(readl(xburst2_smp.ccu + reg) | BIT(cpu), xburst2_smp.ccu + reg);
}

/*
 * An interrupt for another CPU is the bits of its actions in that CPU's
 * mailbox; the CPU takes them out all at once.
 */
static void xburst2_send_ipi_single(int cpu, unsigned int action)
{
	void __iomem *mbox = xburst2_smp.ccu + CCU_MBR(cpu);
	unsigned long flags;

	raw_spin_lock_irqsave(&xburst2_smp.mbox_lock[cpu], flags);
	writel(readl(mbox) | action, mbox);
	raw_spin_unlock_irqrestore(&xburst2_smp.mbox_lock[cpu], flags);
}

static void xburst2_send_ipi_mask(const struct cpumask *mask,
				  unsigned int action)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		xburst2_send_ipi_single(cpu, action);
}

static irqreturn_t xburst2_mbox_interrupt(int irq, void *dev_id)
{
	unsigned int cpu = smp_processor_id();
	void __iomem *mbox = xburst2_smp.ccu + CCU_MBR(cpu);
	unsigned int action;

	raw_spin_lock(&xburst2_smp.mbox_lock[cpu]);
	action = readl(mbox);
	writel(0, mbox);
	raw_spin_unlock(&xburst2_smp.mbox_lock[cpu]);

	/*
	 * No action is not an error. irq-mips-cpu dispatches every line of one
	 * reading of what is pending; a mailbox interrupt that is served from
	 * inside an earlier line's softirqs has its handler called once more
	 * afterwards, with nothing left.
	 */
	if (action & SMP_CALL_FUNCTION)
		generic_smp_call_function_interrupt();

	if (action & SMP_RESCHEDULE_YOURSELF)
		scheduler_ipi();

	return IRQ_HANDLED;
}

/*
 * What a CPU runs when its reset ends: the cache attribute of kseg0, the
 * count register stopped, its stack and thread pointers, and on to
 * smp_bootstrap.
 */
static int __init xburst2_build_start_code(void)
{
	u32 *p = (u32 *)__get_free_page(GFP_KERNEL);

	if (!p)
		return -ENOMEM;

	xburst2_smp.entry = (unsigned long)p;

	/*
	 * Config7, register 16 select 7: Ingenic's code sets these three bits
	 * ("IFU small buffer off").
	 */
	UASM_i_MFC0(&p, REG_V0, 16, 7);
	uasm_i_ori(&p, REG_V0, REG_V0, 7 << 3);
	UASM_i_MTC0(&p, REG_V0, 16, 7);

	/* kseg0 cached */
	UASM_i_MFC0(&p, REG_V0, C0_CONFIG);
	UASM_i_LA(&p, REG_V1, ~CONF_CM_CMASK);
	uasm_i_and(&p, REG_V0, REG_V0, REG_V1);
	uasm_i_ori(&p, REG_V0, REG_V0, CONF_CM_CACHABLE_NONCOHERENT);
	UASM_i_MTC0(&p, REG_V0, C0_CONFIG);

	/* the count register stopped and at zero, no interrupt pending */
	UASM_i_LA(&p, REG_V0, CAUSEF_DC);
	UASM_i_MTC0(&p, REG_V0, C0_CAUSE);
	UASM_i_MTC0(&p, 0, C0_COUNT);

	UASM_i_LA(&p, REG_V0, ST0_CU0);
	UASM_i_MTC0(&p, REG_V0, C0_STATUS);

	UASM_i_LA(&p, REG_SP, (unsigned long)&xburst2_smp.sp);
	UASM_i_LW(&p, REG_SP, 0, REG_SP);
	UASM_i_LA(&p, REG_GP, (unsigned long)&xburst2_smp.gp);
	UASM_i_LW(&p, REG_GP, 0, REG_GP);
	UASM_i_LA(&p, REG_RA, (unsigned long)smp_bootstrap);
	uasm_i_jr(&p, REG_RA);
	uasm_i_nop(&p);

	/* The other CPU starts with its caches as this one leaves them */
	dma_cache_wback_inv(xburst2_smp.entry, 128);

	return 0;
}

static void __init xburst2_smp_setup(void)
{
	unsigned int cpu, cpus = 0;
	struct device_node *np;

	for_each_of_cpu_node(np)
		cpus++;
	cpus = clamp_t(unsigned int, cpus, 1, NR_CPUS);

	/* No mailbox interrupt yet, and every CPU but this one in reset */
	writel(0, xburst2_smp.ccu + CCU_MIMR);
	writel(0xfffe, xburst2_smp.ccu + CCU_CSRR);

	for (cpu = 0; cpu < cpus; cpu++) {
		set_cpu_possible(cpu, true);
		set_cpu_present(cpu, true);
		__cpu_number_map[cpu] = cpu;
		__cpu_logical_map[cpu] = cpu;
		raw_spin_lock_init(&xburst2_smp.mbox_lock[cpu]);
	}

	pr_info("XBurst2: %u CPUs\n", cpus);
}

static void __init xburst2_prepare_cpus(unsigned int max_cpus)
{
	int irq;

	if (max_cpus <= 1)
		return;

	irq = of_irq_get(xburst2_smp.np, 0);
	if (irq <= 0 ||
	    request_irq(irq, xburst2_mbox_interrupt,
			IRQF_PERCPU | IRQF_NO_THREAD | IRQF_NO_SUSPEND,
			"xburst2-mailbox", &xburst2_smp)) {
		pr_err("XBurst2: no mailbox interrupt, the other CPUs stay off\n");
		init_cpu_present(cpumask_of(0));
		return;
	}

	writel(0, xburst2_smp.ccu + CCU_MBR(0));
	writel(BIT(0), xburst2_smp.ccu + CCU_MIMR);

	if (xburst2_build_start_code()) {
		pr_err("XBurst2: no memory for the start code, the other CPUs stay off\n");
		init_cpu_present(cpumask_of(0));
		return;
	}

	writel(xburst2_smp.entry, xburst2_smp.ccu + CCU_RER);
}

static int xburst2_boot_secondary(int cpu, struct task_struct *idle)
{
	u32 reset;

	writel(xburst2_smp.entry, xburst2_smp.ccu + CCU_RER);

	reset = readl(xburst2_smp.ccu + CCU_CSRR) | BIT(cpu);
	writel(reset, xburst2_smp.ccu + CCU_CSRR);

	xburst2_smp.sp = __KSTK_TOS(idle);
	xburst2_smp.gp = (unsigned long)task_thread_info(idle);
	/* The start code reads the two as soon as the reset ends */
	wmb();

	writel(reset & ~BIT(cpu), xburst2_smp.ccu + CCU_CSRR);

	return 0;
}

static void xburst2_init_secondary(void)
{
	unsigned int cpu = smp_processor_id();

	clear_c0_cause(CAUSEF_IP);
	clear_c0_status(ST0_IM);

	ccu_set(CCU_MIMR, cpu);
	writel(0, xburst2_smp.ccu + CCU_MBR(cpu));
	ccu_set(CCU_PIMR, cpu);
	ccu_set(CCU_OIMR, cpu);

	/*
	 * This CPU's lines for the interrupt controller, the mailbox and the
	 * timer. Each is one interrupt for all CPUs, requested once, and a
	 * CPU's own mask bit lets it in.
	 */
	change_c0_status(ST0_IM, STATUSF_IP2 | STATUSF_IP3 | STATUSF_IP4);
}

static void xburst2_smp_finish(void)
{
	local_irq_enable();
}

static const struct plat_smp_ops xburst2_smp_ops = {
	.send_ipi_single	= xburst2_send_ipi_single,
	.send_ipi_mask		= xburst2_send_ipi_mask,
	.init_secondary		= xburst2_init_secondary,
	.smp_finish		= xburst2_smp_finish,
	.boot_secondary		= xburst2_boot_secondary,
	.smp_setup		= xburst2_smp_setup,
	.prepare_cpus		= xburst2_prepare_cpus,
};

int __init register_xburst2_smp_ops(void)
{
	if (boot_cpu_type() != CPU_XBURST2)
		return -ENODEV;

	xburst2_smp.np = of_find_compatible_node(NULL, NULL, "ingenic,x2000-ccu");
	if (!xburst2_smp.np)
		return -ENODEV;

	xburst2_smp.ccu = of_iomap(xburst2_smp.np, 0);
	if (!xburst2_smp.ccu) {
		of_node_put(xburst2_smp.np);
		return -ENOMEM;
	}

	register_smp_ops(&xburst2_smp_ops);

	return 0;
}
