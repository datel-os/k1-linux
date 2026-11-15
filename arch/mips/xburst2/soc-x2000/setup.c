/*
 * Ingenic Soc Setup
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2006 Ingenic Semiconductor Inc.
 */
#include <linux/bits.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/proc_fs.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/irqchip.h>
#include <linux/clocksource.h>

#include <asm/prom.h>
#include <asm/time.h>
#include <core_base.h>
#include <soc/base.h>
#include <soc/cpm.h>
#include <soc/ddr.h>

static int ddr_size = 0;
module_param(ddr_size, int, S_IRUGO);
MODULE_PARM_DESC(ddr_size, "ddr size");


static void __init init_ddr_size(void)
{
	unsigned int ddr_cfg = 0;
	unsigned int col0 = 0;
	unsigned int row0 = 0;
	unsigned int ba0 = 0;
	unsigned int dq_width_bytes = 16 / 8;   // 固定16bit 位宽.
	ddr_cfg = inl(DDRC_H2_IOBASE + DDRC_CFG);

	col0 = ((ddr_cfg & DDRC_CFG_COL0_MASK) >> DDRC_CFG_COL0_BIT) + 8;
	row0 = ((ddr_cfg & DDRC_CFG_ROW0_MASK) >> DDRC_CFG_ROW0_BIT) + 12;
	ba0  = ((ddr_cfg & (1 << DDRC_CFG_BA0)) >> DDRC_CFG_BA0) == 1 ? 3 : 2; // 3: 2**3 = 8 bank, 2: 2**2 = 4 bank.

	ddr_size = (1 << (col0 + row0 + ba0)) / 1024 / 1024 * dq_width_bytes;   // ddr_size in MBytes.

	pr_info("ddr_size: %d MBytes\n", ddr_size);
}

void __init plat_mem_setup(void)
{
	/* use IO_BASE, so that we can use phy addr on hard manual
	 * directly with in(bwlq)/out(bwlq) in io.h.
	 */
	set_io_port_base(IO_BASE);
	ioport_resource.start   = 0x00000000;
	ioport_resource.end = 0xffffffff;
	iomem_resource.start    = 0x00000000;
	iomem_resource.end  = 0xffffffff;
	init_ddr_size();

	__dt_setup_arch(get_fdt_addr());
	return;
}

void __init device_tree_init(void)
{
	unflatten_and_copy_device_tree();
}

static int __init plat_of_populate(void)
{
	of_platform_default_populate(NULL, NULL, NULL);
	return 0;
}
arch_initcall(plat_of_populate);

/*
 * Which supply two of the pin banks run on is chosen by two bits of a clock
 * block register, EXCLK_DS: bit 30 for port A, bit 31 for port E and the MSC
 * pins that go with it. A set bit is 1.8 V, a clear one 3.3 V. Upstream's pin
 * control driver has nothing for them, so a board states the two supplies in
 * two properties of the pin controller's node and they are set here, before
 * any driver uses a pin. A bank without its property keeps what the boot
 * loader left.
 */
#define EXCLK_DS_PA_1V8		BIT(30)
#define EXCLK_DS_PE_MSC_1V8	BIT(31)

static u32 __init bank_supply(struct device_node *np, const char *prop,
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

static int __init bank_supplies_init(void)
{
	struct device_node *np;
	u32 was, val;

	np = of_find_compatible_node(NULL, NULL, "ingenic,x2000-pinctrl");
	if (!np)
		return 0;
	was = cpm_inl(CPM_EXCLK_DS);
	val = bank_supply(np, "ingenic,gpa-microvolt", EXCLK_DS_PA_1V8, was);
	val = bank_supply(np, "ingenic,gpe-msc-microvolt", EXCLK_DS_PE_MSC_1V8, val);
	of_node_put(np);
	if (val != was)
		cpm_outl(val, CPM_EXCLK_DS);
	pr_info("x2000: pin bank supplies: EXCLK_DS 0x%08x, was 0x%08x\n", val, was);
	return 0;
}
arch_initcall(bank_supplies_init);

void __init plat_time_init(void)
{
	of_clk_init(NULL);

	timer_probe();
}

void __init arch_init_irq(void)
{
	irqchip_init();
}
