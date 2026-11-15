// SPDX-License-Identifier: GPL-2.0
/*
 * The secondary cache of the Ingenic XBurst2
 *
 * Config2 describes it as on a MIPS32 core, but for 8 or 16 ways, where
 * sc-mips reads 1 to 8 out of the same field. It cannot be switched off.
 *
 * From the cache code of Ingenic's SDK kernel (arch/mips/xburst2/core/sc.c).
 */

#include <linux/kernel.h>
#include <linux/mm.h>

#include <asm/bcache.h>
#include <asm/cacheops.h>
#include <asm/cpu.h>
#include <asm/cpu-features.h>
#include <asm/mipsregs.h>
#include <asm/r4kcache.h>

static unsigned long xburst2_sc_size __read_mostly;

static void xburst2_sc_blast(void)
{
	if (cpu_scache_line_size() == 64)
		blast_scache64();
	else
		blast_scache32();
}

static void xburst2_sc_wback_inv(unsigned long addr, unsigned long size)
{
	__sync();

	if (size >= xburst2_sc_size)
		xburst2_sc_blast();
	else
		blast_scache_range(addr, addr + size);

	__fast_iob();
}

static void xburst2_sc_inv(unsigned long addr, unsigned long size)
{
	unsigned long almask = ~(cpu_scache_line_size() - 1);

	if (size >= xburst2_sc_size) {
		xburst2_sc_blast();
	} else {
		/*
		 * The first and the last line may hold other data too:
		 * those two are written back, not dropped.
		 */
		cache_op(Hit_Writeback_Inv_SD, addr & almask);
		cache_op(Hit_Writeback_Inv_SD, (addr + size - 1) & almask);
		blast_inv_scache_range(addr, addr + size);
	}

	__fast_iob();
}

static void xburst2_sc_enable(void)
{
	/* always on */
}

static void xburst2_sc_disable(void)
{
	/* always on */
}

static struct bcache_ops xburst2_sc_ops = {
	.bc_enable	= xburst2_sc_enable,
	.bc_disable	= xburst2_sc_disable,
	.bc_wback_inv	= xburst2_sc_wback_inv,
	.bc_inv		= xburst2_sc_inv,
};

static int xburst2_sc_probe(void)
{
	struct cpuinfo_mips *c = &current_cpu_data;
	unsigned int config2, tmp;

	/* Mark as not present until probe completed */
	c->scache.flags |= MIPS_CACHE_NOT_PRESENT;

	/* Is there a Config2 register? */
	if (!(read_c0_config1() & MIPS_CONF_M))
		return 0;

	config2 = read_c0_config2();

	tmp = (config2 >> 4) & 0x0f;
	if (tmp < 1 || tmp > 7)
		return 0;
	c->scache.linesz = 2 << tmp;

	tmp = (config2 >> 8) & 0x0f;
	if (tmp > 7)
		return 0;
	c->scache.sets = 64 << tmp;

	/* 8 or 16 ways, and nothing else */
	tmp = (config2 >> 0) & 0x0f;
	if (tmp != 7 && tmp != 15)
		return 0;
	c->scache.ways = tmp + 1;

	c->scache.waysize = c->scache.sets * c->scache.linesz;
	c->scache.waybit = __ffs(c->scache.waysize);
	c->scache.flags &= ~MIPS_CACHE_NOT_PRESENT;

	xburst2_sc_size = c->scache.ways * c->scache.sets * c->scache.linesz;

	return 1;
}

/* For each CPU as it comes up, as the primary caches are */
int xburst2_sc_init(void)
{
	struct cpuinfo_mips *c = &current_cpu_data;
	int found = xburst2_sc_probe();

	if (found) {
		bcops = &xburst2_sc_ops;
		pr_info("Unified secondary cache %ldkB %u-way, linesize %d bytes.\n",
			xburst2_sc_size >> 10, c->scache.ways, c->scache.linesz);
	}

	c->options |= MIPS_CPU_INCLUSIVE_CACHES;

	return found;
}
