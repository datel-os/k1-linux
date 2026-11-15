// SPDX-License-Identifier: GPL-2.0
/*
 * X2000 SoC CGU driver
 *
 * The clocks are those of the clock driver in Ingenic's SDK kernel
 * (drivers/clk/ingenic-v2/clk-x2000.c), in the form of the X1830's driver
 * here: the two SoCs have the same CPCCR register and the same layout of the
 * PLLs' registers. The PLLs themselves are clocks of this driver's own.
 *
 * Not described: the audio clocks (I2S, PCM, DMIC, S/PDIF), the gates of the
 * buses themselves and the power-down bits of the blocks' memories. They stay
 * as the boot loader leaves them.
 */

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>

#include <dt-bindings/clock/ingenic,x2000-cgu.h>

#include "cgu.h"
#include "pm.h"

/* CGU register offsets */
#define CGU_REG_CPCCR		0x00
#define CGU_REG_LCR		0x04
#define CGU_REG_APLL		0x10
#define CGU_REG_MPLL		0x14
#define CGU_REG_EPLL		0x18
#define CGU_REG_CLKGR0		0x20
#define CGU_REG_OPCR		0x24
#define CGU_REG_CLKGR1		0x28
#define CGU_REG_DDRCDR		0x2c
#define CGU_REG_ISPCDR		0x30
#define CGU_REG_MACPTPCDR	0x4c
#define CGU_REG_RSACDR		0x50
#define CGU_REG_MACCDR		0x54
#define CGU_REG_MACTXCDR	0x58
#define CGU_REG_SSICDR		0x5c
#define CGU_REG_LPCDR		0x64
#define CGU_REG_MSC0CDR		0x68
#define CGU_REG_PWMCDR		0x6c
#define CGU_REG_SFCCDR		0x74
#define CGU_REG_CIMCDR		0x78
#define CGU_REG_MSC1CDR		0xa4
#define CGU_REG_MSC2CDR		0xa8
#define CGU_REG_CPCSR		0xd4
#define CGU_REG_MACTXCDR1	0xdc

/* bits within the CPCCR register */
#define CPCCR_SEL_SRC_MASK	GENMASK(31, 30)
#define CPCCR_SEL_SRC_APLL	FIELD_PREP_CONST(CPCCR_SEL_SRC_MASK, 2)
#define CPCCR_SEL_CPLL_MASK	GENMASK(29, 28)
#define CPCCR_SEL_CPLL_SCLKA	FIELD_PREP_CONST(CPCCR_SEL_CPLL_MASK, 1)
#define CPCCR_SEL_CPLL_MPLL	FIELD_PREP_CONST(CPCCR_SEL_CPLL_MASK, 2)
#define CPCCR_SEL_H0PLL_MASK	GENMASK(27, 26)
#define CPCCR_SEL_H0PLL_SCLKA	FIELD_PREP_CONST(CPCCR_SEL_H0PLL_MASK, 1)
#define CPCCR_SEL_H2PLL_MASK	GENMASK(25, 24)
#define CPCCR_SEL_H2PLL_SCLKA	FIELD_PREP_CONST(CPCCR_SEL_H2PLL_MASK, 1)
#define CPCCR_CE_CPU		BIT(22)
#define CPCCR_L2CDIV_MASK	GENMASK(7, 4)
#define CPCCR_CDIV_MASK		GENMASK(3, 0)

/* bits within the CPCSR register */
#define CPCSR_CDIV_BUSY		BIT(0)

/* bits within the DDRCDR register */
#define DDRCDR_SEL_MASK		GENMASK(31, 30)
#define DDRCDR_SEL_SCLKA	FIELD_PREP_CONST(DDRCDR_SEL_MASK, 1)

/* bits within the MSC0CDR register */
#define MSC0CDR_BIT21		BIT(21)

/* bits within the PLL control registers */
#define PLL_M_MASK		GENMASK(28, 20)
#define PLL_N_MASK		GENMASK(19, 14)
#define PLL_OD_MASK		GENMASK(13, 11)
#define PLL_STATUS_MASK		GENMASK(3, 2)
#define PLL_ON			BIT(3)
#define PLL_ENABLE		BIT(0)

/*
 * While the APLL is off the CPU runs on the MPLL divided by two, the L2
 * cache on the MPLL divided by four. For that the MPLL has to be in this
 * range.
 */
#define X2000_CPU_PARK_MPLL_MIN	1200000000UL
#define X2000_CPU_PARK_MPLL_MAX	2400000000UL
#define X2000_CPU_PARK_CDIV	(2 - 1)
#define X2000_CPU_PARK_L2CDIV	(4 - 1)

static struct ingenic_cgu *cgu;

/*
 * The PLLs
 *
 *	rate = crystal * 2 * (M + 1) / ((N + 1) << OD)
 *
 * They are clocks of this driver, not PLLs of the framework. The framework
 * sets a rate by writing the new multiplier into the running PLL and lets a
 * driver know afterwards. Nothing may run on a PLL while its setting changes,
 * and on the APLL the CPU does: it has to be moved away before the register
 * is touched.
 *
 * A rate is set by the multiplier alone. The reference divider and the output
 * divider stay as they are in the register, and with them the range of the
 * loop filter (bits 7:5), which goes with the reference divider.
 *
 * Bit 3 says that the PLL is on. It is set about 750 us after the enable bit
 * was, and a setting that changes meanwhile does not clear it: it stands for
 * the setting in the register only if that was written while the PLL was off.
 * So a new setting is written the way Ingenic's code does it: zero, then the
 * setting with the enable bit.
 */

static void __iomem *x2000_pll_reg(struct clk_hw *hw)
{
	switch (to_ingenic_clk(hw)->idx) {
	case X2000_CLK_APLL:
		return cgu->base + CGU_REG_APLL;
	case X2000_CLK_MPLL:
		return cgu->base + CGU_REG_MPLL;
	default:
		return cgu->base + CGU_REG_EPLL;
	}
}

static unsigned long x2000_pll_rate(u32 ctl, unsigned long parent_rate)
{
	u32 div = (FIELD_GET(PLL_N_MASK, ctl) + 1) << FIELD_GET(PLL_OD_MASK, ctl);

	return div_u64((u64)parent_rate * 2 * (FIELD_GET(PLL_M_MASK, ctl) + 1),
		       div);
}

/* The register's value for the rate that can be had nearest to the one asked for */
static u32 x2000_pll_ctl(u32 ctl, unsigned long rate, unsigned long parent_rate)
{
	u32 div = (FIELD_GET(PLL_N_MASK, ctl) + 1) << FIELD_GET(PLL_OD_MASK, ctl);
	u64 m;

	m = DIV_ROUND_CLOSEST_ULL((u64)rate * div, parent_rate * 2);
	m = clamp_t(u64, m, 1, FIELD_MAX(PLL_M_MASK) + 1);

	return (ctl & ~PLL_M_MASK) | FIELD_PREP(PLL_M_MASK, m - 1);
}

static unsigned long
x2000_pll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	return x2000_pll_rate(readl(x2000_pll_reg(hw)), parent_rate);
}

static int x2000_pll_determine_rate(struct clk_hw *hw,
				    struct clk_rate_request *req)
{
	u32 ctl = x2000_pll_ctl(readl(x2000_pll_reg(hw)), req->rate,
				req->best_parent_rate);

	req->rate = x2000_pll_rate(ctl, req->best_parent_rate);

	return 0;
}

static int x2000_pll_is_enabled(struct clk_hw *hw)
{
	return !!(readl(x2000_pll_reg(hw)) & PLL_ENABLE);
}

/*
 * Off, then the setting with the enable bit, then a wait for the PLL to be
 * on. The CPU's rate is assigned before the kernel has a clock source, so the
 * wait may not be one that reads the time: without a delay between its reads
 * this one counts them.
 */
static bool x2000_pll_start(void __iomem *reg, u32 ctl)
{
	u32 val;

	writel(0, reg);
	writel((ctl & ~PLL_STATUS_MASK) | PLL_ENABLE, reg);

	return !readl_poll_timeout_atomic(reg, val, val & PLL_ON, 0,
					  10 * USEC_PER_MSEC);
}

static void x2000_cpccr_write(u32 cpccr, bool dividers)
{
	unsigned int i;

	if (dividers) {
		writel(cpccr | CPCCR_CE_CPU, cgu->base + CGU_REG_CPCCR);
		for (i = 0; i < 1000000; i++)
			if (!(readl(cgu->base + CGU_REG_CPCSR) & CPCSR_CDIV_BUSY))
				break;
	}
	writel(cpccr, cgu->base + CGU_REG_CPCCR);
}

/*
 * A PLL is off for a moment when its rate is set, so nothing may run on it
 * then. Of what can run on the APLL, the CPU and the L2 cache are moved to
 * the MPLL for that time; if a bus or the memory runs on it the change is
 * refused. The rate the clock core finds afterwards is the one in the
 * register, whatever this returns.
 */
static int x2000_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long parent_rate)
{
	bool apll = to_ingenic_clk(hw)->idx == X2000_CLK_APLL;
	void __iomem *reg = x2000_pll_reg(hw);
	u32 old, new, cpccr = 0, dividers = 0;
	bool parked = false, back = false;
	unsigned long mpll = 0, flags;
	int ret = 0;

	if (apll)
		mpll = clk_hw_get_rate(__clk_get_hw(cgu->clocks.clks[X2000_CLK_MPLL]));

	spin_lock_irqsave(&cgu->lock, flags);

	old = readl(reg);
	new = x2000_pll_ctl(old, rate, parent_rate);

	if (!(old & PLL_ENABLE)) {
		/* It is off: the setting is for when it is switched on */
		writel(new & ~PLL_STATUS_MASK, reg);
		goto unlock;
	}

	if (apll)
		cpccr = readl(cgu->base + CGU_REG_CPCCR);

	if ((cpccr & CPCCR_SEL_SRC_MASK) == CPCCR_SEL_SRC_APLL) {
		bool cpu = (cpccr & CPCCR_SEL_CPLL_MASK) == CPCCR_SEL_CPLL_SCLKA;

		if ((cpccr & CPCCR_SEL_H0PLL_MASK) == CPCCR_SEL_H0PLL_SCLKA ||
		    (cpccr & CPCCR_SEL_H2PLL_MASK) == CPCCR_SEL_H2PLL_SCLKA ||
		    (readl(cgu->base + CGU_REG_DDRCDR) & DDRCDR_SEL_MASK) ==
							DDRCDR_SEL_SCLKA ||
		    (cpu && (mpll < X2000_CPU_PARK_MPLL_MIN ||
			     mpll > X2000_CPU_PARK_MPLL_MAX))) {
			ret = -EBUSY;
			goto unlock;
		}

		if (cpu) {
			/* to the MPLL: the dividers first, then the source */
			dividers = cpccr & (CPCCR_L2CDIV_MASK | CPCCR_CDIV_MASK);
			cpccr &= ~(CPCCR_L2CDIV_MASK | CPCCR_CDIV_MASK);
			cpccr |= FIELD_PREP(CPCCR_L2CDIV_MASK,
					    X2000_CPU_PARK_L2CDIV) |
				 FIELD_PREP(CPCCR_CDIV_MASK, X2000_CPU_PARK_CDIV);
			x2000_cpccr_write(cpccr, true);
			cpccr &= ~CPCCR_SEL_CPLL_MASK;
			cpccr |= CPCCR_SEL_CPLL_MPLL;
			x2000_cpccr_write(cpccr, false);
			parked = true;
		}
	}

	if (!x2000_pll_start(reg, new)) {
		/* The setting it had, then: it came on with that one */
		ret = -EIO;
		x2000_pll_start(reg, old);
	}

	if (parked && (readl(reg) & PLL_ON)) {
		/* back to the APLL: the source first, then the dividers */
		cpccr &= ~CPCCR_SEL_CPLL_MASK;
		cpccr |= CPCCR_SEL_CPLL_SCLKA;
		x2000_cpccr_write(cpccr, false);
		cpccr &= ~(CPCCR_L2CDIV_MASK | CPCCR_CDIV_MASK);
		x2000_cpccr_write(cpccr | dividers, true);
		back = true;
	}

unlock:
	spin_unlock_irqrestore(&cgu->lock, flags);

	if (ret == -EBUSY)
		pr_warn("x2000-cgu: the APLL stays at %lu Hz: something that cannot be moved runs on it (CPCCR 0x%08x, MPLL at %lu Hz)\n",
			x2000_pll_rate(old, parent_rate), cpccr, mpll);
	if (ret == -EIO)
		pr_err("x2000-cgu: %s did not come on at %lu Hz\n",
		       clk_hw_get_name(hw), rate);
	if (parked && !back)
		pr_err("x2000-cgu: the CPU stays on the MPLL divided by %u\n",
		       X2000_CPU_PARK_CDIV + 1);
	if (back && !ret)
		pr_info("x2000-cgu: CPU clock set to %lu MHz\n",
			rate / (FIELD_GET(CPCCR_CDIV_MASK, dividers) + 1) / 1000000);

	return ret;
}

static int x2000_pll_enable(struct clk_hw *hw)
{
	void __iomem *reg = x2000_pll_reg(hw);
	unsigned long flags;
	bool on = true;
	u32 ctl;

	spin_lock_irqsave(&cgu->lock, flags);
	ctl = readl(reg);
	if (!(ctl & PLL_ENABLE))
		on = x2000_pll_start(reg, ctl);
	spin_unlock_irqrestore(&cgu->lock, flags);

	return on ? 0 : -ETIMEDOUT;
}

static void x2000_pll_disable(struct clk_hw *hw)
{
	void __iomem *reg = x2000_pll_reg(hw);
	unsigned long flags;

	spin_lock_irqsave(&cgu->lock, flags);
	writel(readl(reg) & ~(PLL_STATUS_MASK | PLL_ENABLE), reg);
	spin_unlock_irqrestore(&cgu->lock, flags);
}

/* The CPU runs on it: its rate can be set, and the clock core cannot switch it */
static const struct clk_ops x2000_apll_ops = {
	.recalc_rate	= x2000_pll_recalc_rate,
	.determine_rate	= x2000_pll_determine_rate,
	.set_rate	= x2000_pll_set_rate,
	.is_enabled	= x2000_pll_is_enabled,
};

/* The buses and the memory run on it: it is only read */
static const struct clk_ops x2000_mpll_ops = {
	.recalc_rate	= x2000_pll_recalc_rate,
	.is_enabled	= x2000_pll_is_enabled,
};

static const struct clk_ops x2000_epll_ops = {
	.recalc_rate	= x2000_pll_recalc_rate,
	.determine_rate	= x2000_pll_determine_rate,
	.set_rate	= x2000_pll_set_rate,
	.enable		= x2000_pll_enable,
	.disable	= x2000_pll_disable,
	.is_enabled	= x2000_pll_is_enabled,
};

/*
 * The dividers of the CPU, its L2 cache and the buses, all in CPCCR
 *
 * They are read here and not set. A new divider is taken over with a change
 * enable bit and the unit says in another register, CPCSR, when it has done
 * so; the framework's dividers look for a busy bit in the divider's own
 * register, and they are written whenever the rate above them changes. The
 * APLL's rate setting is the one place that changes two of them.
 */

static unsigned long
x2000_cpccr_div_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	unsigned int shift;

	switch (to_ingenic_clk(hw)->idx) {
	case X2000_CLK_CPU:
		shift = 0;
		break;
	case X2000_CLK_L2CACHE:
		shift = 4;
		break;
	case X2000_CLK_AHB0:
		shift = 8;
		break;
	case X2000_CLK_AHB2:
		shift = 12;
		break;
	default:	/* X2000_CLK_PCLK */
		shift = 16;
		break;
	}

	return parent_rate /
	       (((readl(cgu->base + CGU_REG_CPCCR) >> shift) & 0xf) + 1);
}

static const struct clk_ops x2000_cpccr_div_ops = {
	.recalc_rate	= x2000_cpccr_div_recalc_rate,
};

/*
 * The power switches of four blocks, in the LCR register: a set control bit
 * (31 down to 28) switches a block off, and its state bit (27 down to 24)
 * follows when that has happened. A block's clock gate is the parent of its
 * switch, so the clock runs before the power comes and after it has gone.
 */

static u32 x2000_pwr_ctrl_bit(struct clk_hw *hw)
{
	return BIT(31 - (to_ingenic_clk(hw)->idx - X2000_CLK_ISP0_PWR));
}

static int x2000_pwr_is_enabled(struct clk_hw *hw)
{
	return !(readl(cgu->base + CGU_REG_LCR) & (x2000_pwr_ctrl_bit(hw) >> 4));
}

static int x2000_pwr_switch(struct clk_hw *hw, bool on)
{
	u32 ctrl = x2000_pwr_ctrl_bit(hw), state = ctrl >> 4, lcr;
	unsigned long flags;

	spin_lock_irqsave(&cgu->lock, flags);
	lcr = readl(cgu->base + CGU_REG_LCR);
	writel(on ? lcr & ~ctrl : lcr | ctrl, cgu->base + CGU_REG_LCR);
	spin_unlock_irqrestore(&cgu->lock, flags);

	return readl_poll_timeout_atomic(cgu->base + CGU_REG_LCR, lcr,
					 !(lcr & state) == on, 0,
					 20 * USEC_PER_MSEC);
}

static int x2000_pwr_enable(struct clk_hw *hw)
{
	return x2000_pwr_switch(hw, true);
}

static void x2000_pwr_disable(struct clk_hw *hw)
{
	if (x2000_pwr_switch(hw, false))
		pr_warn_once("x2000-cgu: %s did not switch off\n", clk_hw_get_name(hw));
}

static const struct clk_ops x2000_pwr_ops = {
	.enable		= x2000_pwr_enable,
	.disable	= x2000_pwr_disable,
	.is_enabled	= x2000_pwr_is_enabled,
};

#define X2000_GATE(_name, _parent, _reg, _bit)				\
	{								\
		_name, CGU_CLK_GATE,					\
		.parents = { _parent, -1, -1, -1 },			\
		.gate = { _reg, _bit },					\
	}

/* The same, for a clock that stays as the boot loader left it if nothing uses it */
#define X2000_GATE_KEPT(_name, _parent, _reg, _bit)			\
	{								\
		_name, CGU_CLK_GATE,					\
		.flags = CLK_IGNORE_UNUSED,				\
		.parents = { _parent, -1, -1, -1 },			\
		.gate = { _reg, _bit },					\
	}

/* A divider of a peripheral: source in bits 31:30, change enable, busy, stop */
#define X2000_PERIPH_PARENTS						\
	.parents = { X2000_CLK_SCLKA, X2000_CLK_MPLL, X2000_CLK_EPLL, -1 }

#define X2000_MSC(_name, _reg, _gate_reg, _gate_bit)			\
	{								\
		_name, CGU_CLK_MUX | CGU_CLK_MUX_GLITCHFREE |		\
		       CGU_CLK_DIV | CGU_CLK_GATE,			\
		.parents = { X2000_CLK_SCLKA, X2000_CLK_MPLL,		\
			     X2000_CLK_EXCLK, -1 },			\
		.mux = { _reg, 30, 2 },					\
		.div = {						\
			.reg = _reg,					\
			.shift = 0,					\
			.div = 4,					\
			.bits = 8,					\
			.ce_bit = 29,					\
			.busy_bit = 28,					\
			.stop_bit = 27,					\
			.ce_clear = true,				\
		},							\
		.gate = { _gate_reg, _gate_bit },			\
	}

#define X2000_CPCCR_DIV(_name, _parent)					\
	{								\
		_name, CGU_CLK_CUSTOM,					\
		.flags = CLK_IS_CRITICAL,				\
		.parents = { _parent, -1, -1, -1 },			\
		.custom = { &x2000_cpccr_div_ops },			\
	}

#define X2000_PWR(_name, _parent)					\
	{								\
		_name, CGU_CLK_CUSTOM,					\
		.flags = CLK_IGNORE_UNUSED,				\
		.parents = { _parent, -1, -1, -1 },			\
		.custom = { &x2000_pwr_ops },				\
	}

static const struct ingenic_cgu_clk_info x2000_cgu_clocks[] = {

	/* External clocks */

	[X2000_CLK_EXCLK] = { "ext", CGU_CLK_EXT },
	[X2000_CLK_RTCLK] = { "osc32k", CGU_CLK_EXT },

	/* PLLs */

	[X2000_CLK_APLL] = {
		"apll", CGU_CLK_CUSTOM,
		.parents = { X2000_CLK_EXCLK, -1, -1, -1 },
		.custom = { &x2000_apll_ops },
	},

	[X2000_CLK_MPLL] = {
		"mpll", CGU_CLK_CUSTOM,
		.parents = { X2000_CLK_EXCLK, -1, -1, -1 },
		.custom = { &x2000_mpll_ops },
	},

	[X2000_CLK_EPLL] = {
		"epll", CGU_CLK_CUSTOM,
		/*
		 * The boot loader leaves it running and the SDK's driver could
		 * not switch a PLL off. Nothing here uses it; it is left as it
		 * is.
		 */
		.flags = CLK_IGNORE_UNUSED,
		.parents = { X2000_CLK_EXCLK, -1, -1, -1 },
		.custom = { &x2000_epll_ops },
	},

	/* Muxes & dividers */

	[X2000_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, X2000_CLK_EXCLK, X2000_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[X2000_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, X2000_CLK_SCLKA, X2000_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	/*
	 * Disabling the CPU clock, the L2 cache clock or a bus clock, or any
	 * of their parents, will hang the system; mark them critical. The
	 * buses' own gates (CLKGR0 bits 28 and 29) are not described.
	 */

	[X2000_CLK_CPU] = X2000_CPCCR_DIV("cpu", X2000_CLK_CPUMUX),
	[X2000_CLK_L2CACHE] = X2000_CPCCR_DIV("l2cache", X2000_CLK_CPUMUX),

	[X2000_CLK_AHB0MUX] = {
		"ahb0_mux", CGU_CLK_MUX,
		.parents = { -1, X2000_CLK_SCLKA, X2000_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
	},

	[X2000_CLK_AHB0] = X2000_CPCCR_DIV("ahb0", X2000_CLK_AHB0MUX),

	[X2000_CLK_AHB2PMUX] = {
		"ahb2_apb_mux", CGU_CLK_MUX,
		.parents = { -1, X2000_CLK_SCLKA, X2000_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
	},

	[X2000_CLK_AHB2] = X2000_CPCCR_DIV("ahb2", X2000_CLK_AHB2PMUX),
	[X2000_CLK_PCLK] = X2000_CPCCR_DIV("pclk", X2000_CLK_AHB2PMUX),

	[X2000_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		/*
		 * Disabling DDR clock or its parents will render DRAM
		 * inaccessible; mark it critical.
		 */
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, X2000_CLK_SCLKA, X2000_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	[X2000_CLK_ISP] = {
		"isp", CGU_CLK_MUX | CGU_CLK_DIV,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_ISPCDR, 30, 2 },
		.div = { CGU_REG_ISPCDR, 0, 1, 4, 29, 28, 27 },
	},

	[X2000_CLK_MACPTP] = {
		"mac_ptp", CGU_CLK_MUX | CGU_CLK_DIV,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_MACPTPCDR, 30, 2 },
		.div = { CGU_REG_MACPTPCDR, 0, 1, 8, 29, 28, 27 },
	},

	[X2000_CLK_RSA] = {
		"rsa", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_RSACDR, 30, 2 },
		.div = { CGU_REG_RSACDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 25 },
	},

	[X2000_CLK_MACPHY] = {
		"mac_phy", CGU_CLK_MUX | CGU_CLK_DIV,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, 29, 28, 27 },
	},

	[X2000_CLK_MACTXPHY0] = {
		"mac_tx_phy0", CGU_CLK_MUX | CGU_CLK_DIV,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_MACTXCDR, 30, 2 },
		.div = { CGU_REG_MACTXCDR, 0, 1, 8, 29, 28, 27 },
	},

	[X2000_CLK_SSIPLL] = {
		"ssi_pll", CGU_CLK_MUX | CGU_CLK_DIV,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_SSICDR, 30, 2 },
		.div = { CGU_REG_SSICDR, 0, 1, 8, 29, 28, 27 },
	},

	[X2000_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		/* The SDK's driver leaves the panel's clocks as they are found */
		.flags = CLK_IGNORE_UNUSED,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	/*
	 * The clock of an SD host is what its card gets: the controller does
	 * not divide it again. The crystal is the source for the 400 kHz of a
	 * card's identification, which the MPLL cannot be divided down to.
	 *
	 * Source and divider are taken over together, when the divider is
	 * written with the change enable bit, and only while the host asks
	 * for its clock: the host sees to that, and stops the card's clock
	 * for the change. So the source may be written while the clock is
	 * enabled. The change enable bit is cleared afterwards, as the SDK's
	 * driver did: the host writes one bit of the register itself, and
	 * that is not to start a change.
	 */

	[X2000_CLK_MSC0] = X2000_MSC("msc0", CGU_REG_MSC0CDR, CGU_REG_CLKGR0, 4),

	[X2000_CLK_PWM] = {
		"pwm", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_PWMCDR, 30, 2 },
		.div = { CGU_REG_PWMCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[X2000_CLK_SFC] = {
		"sfc", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_SFCCDR, 30, 2 },
		.div = { CGU_REG_SFCCDR, 0, 1, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 2 },
	},

	[X2000_CLK_CIM] = {
		"cim", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_CIMCDR, 30, 2 },
		.div = { CGU_REG_CIMCDR, 0, 1, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 22 },
	},

	[X2000_CLK_MSC1] = X2000_MSC("msc1", CGU_REG_MSC1CDR, CGU_REG_CLKGR0, 5),

	[X2000_CLK_MSC2] = X2000_MSC("msc2", CGU_REG_MSC2CDR, CGU_REG_CLKGR1, 25),

	[X2000_CLK_MACTXPHY1] = {
		"mac_tx_phy1", CGU_CLK_MUX | CGU_CLK_DIV,
		X2000_PERIPH_PARENTS,
		.mux = { CGU_REG_MACTXCDR1, 30, 2 },
		.div = { CGU_REG_MACTXCDR1, 0, 1, 8, 29, 28, 27 },
	},

	[X2000_CLK_EXCLK_DIV512] = {
		"exclk_div512", CGU_CLK_FIXDIV,
		.parents = { X2000_CLK_EXCLK },
		.fixdiv = { 512 },
	},

	/*
	 * What the timer unit and the watchdog count as the RTC clock: the
	 * crystal divided by 512, or the 32 kHz oscillator. Upstream's TCU
	 * clock driver finds it, the crystal and "pclk" by these names.
	 */
	[X2000_CLK_ERCS] = {
		"rtc", CGU_CLK_MUX,
		.parents = { X2000_CLK_EXCLK_DIV512, X2000_CLK_RTCLK, -1, -1 },
		.mux = { CGU_REG_OPCR, 2, 1 },
	},

	/* Gate-only clocks */

	[X2000_CLK_NEMC] = X2000_GATE("nemc", X2000_CLK_AHB2, CGU_REG_CLKGR0, 0),
	[X2000_CLK_EFUSE] = X2000_GATE("efuse", X2000_CLK_AHB2, CGU_REG_CLKGR0, 1),
	[X2000_CLK_OTG] = X2000_GATE("otg", X2000_CLK_AHB2, CGU_REG_CLKGR0, 3),
	[X2000_CLK_SCC] = X2000_GATE("scc", X2000_CLK_PCLK, CGU_REG_CLKGR0, 6),
	[X2000_CLK_I2C0] = X2000_GATE("i2c0", X2000_CLK_PCLK, CGU_REG_CLKGR0, 7),
	[X2000_CLK_I2C1] = X2000_GATE("i2c1", X2000_CLK_PCLK, CGU_REG_CLKGR0, 8),
	[X2000_CLK_I2C2] = X2000_GATE("i2c2", X2000_CLK_PCLK, CGU_REG_CLKGR0, 9),
	[X2000_CLK_I2C3] = X2000_GATE("i2c3", X2000_CLK_PCLK, CGU_REG_CLKGR0, 10),
	[X2000_CLK_AUDIO] = X2000_GATE("audio", X2000_CLK_AHB2, CGU_REG_CLKGR0, 11),
	[X2000_CLK_HELIX] = X2000_GATE("helix", X2000_CLK_AHB0, CGU_REG_CLKGR0, 12),
	[X2000_CLK_SADC] = X2000_GATE("sadc", X2000_CLK_PCLK, CGU_REG_CLKGR0, 13),
	[X2000_CLK_UART0] = X2000_GATE("uart0", X2000_CLK_PCLK, CGU_REG_CLKGR0, 14),
	[X2000_CLK_UART1] = X2000_GATE("uart1", X2000_CLK_PCLK, CGU_REG_CLKGR0, 15),
	[X2000_CLK_UART2] = X2000_GATE("uart2", X2000_CLK_PCLK, CGU_REG_CLKGR0, 16),
	[X2000_CLK_DTRNG] = X2000_GATE("dtrng", X2000_CLK_PCLK, CGU_REG_CLKGR0, 17),
	[X2000_CLK_TCU] = X2000_GATE("tcu", X2000_CLK_PCLK, CGU_REG_CLKGR0, 18),
	[X2000_CLK_SSI0] = X2000_GATE("ssi0", X2000_CLK_SSIPLL, CGU_REG_CLKGR0, 19),
	[X2000_CLK_OST] = X2000_GATE_KEPT("ost", X2000_CLK_EXCLK, CGU_REG_CLKGR0, 20),
	[X2000_CLK_PDMA] = X2000_GATE("pdma", X2000_CLK_AHB2, CGU_REG_CLKGR0, 21),
	[X2000_CLK_AES] = X2000_GATE("aes", X2000_CLK_AHB2, CGU_REG_CLKGR0, 24),
	[X2000_CLK_SSI1] = X2000_GATE("ssi1", X2000_CLK_SSIPLL, CGU_REG_CLKGR0, 26),
	/* The RTC itself always runs on the 32 kHz oscillator */
	[X2000_CLK_RTC] = X2000_GATE("rtc_dev", X2000_CLK_RTCLK, CGU_REG_CLKGR0, 27),

	[X2000_CLK_I2C4] = X2000_GATE("i2c4", X2000_CLK_PCLK, CGU_REG_CLKGR1, 0),
	[X2000_CLK_I2C5] = X2000_GATE("i2c5", X2000_CLK_PCLK, CGU_REG_CLKGR1, 1),
	[X2000_CLK_ISP0] = X2000_GATE("isp0", X2000_CLK_PCLK, CGU_REG_CLKGR1, 2),
	[X2000_CLK_ISP1] = X2000_GATE("isp1", X2000_CLK_PCLK, CGU_REG_CLKGR1, 3),
	[X2000_CLK_FELIX] = X2000_GATE("felix", X2000_CLK_AHB0, CGU_REG_CLKGR1, 4),
	[X2000_CLK_HASH] = X2000_GATE("hash", X2000_CLK_AHB2, CGU_REG_CLKGR1, 6),
	[X2000_CLK_ROT] = X2000_GATE("rot", X2000_CLK_AHB0, CGU_REG_CLKGR1, 7),
	[X2000_CLK_UART3] = X2000_GATE("uart3", X2000_CLK_PCLK, CGU_REG_CLKGR1, 16),
	[X2000_CLK_UART4] = X2000_GATE("uart4", X2000_CLK_PCLK, CGU_REG_CLKGR1, 17),
	[X2000_CLK_UART5] = X2000_GATE("uart5", X2000_CLK_PCLK, CGU_REG_CLKGR1, 18),
	[X2000_CLK_UART6] = X2000_GATE("uart6", X2000_CLK_PCLK, CGU_REG_CLKGR1, 19),
	[X2000_CLK_UART7] = X2000_GATE("uart7", X2000_CLK_PCLK, CGU_REG_CLKGR1, 20),
	[X2000_CLK_UART8] = X2000_GATE("uart8", X2000_CLK_PCLK, CGU_REG_CLKGR1, 21),
	[X2000_CLK_UART9] = X2000_GATE("uart9", X2000_CLK_PCLK, CGU_REG_CLKGR1, 22),
	[X2000_CLK_GMAC0] = X2000_GATE("gmac0", X2000_CLK_AHB2, CGU_REG_CLKGR1, 23),
	[X2000_CLK_GMAC1] = X2000_GATE("gmac1", X2000_CLK_AHB2, CGU_REG_CLKGR1, 24),
	[X2000_CLK_INTC] = X2000_GATE_KEPT("intc", X2000_CLK_AHB2, CGU_REG_CLKGR1, 26),
	[X2000_CLK_CSI] = X2000_GATE("csi", X2000_CLK_AHB0, CGU_REG_CLKGR1, 28),
	/* As "lcd" above */
	[X2000_CLK_DSI] = X2000_GATE_KEPT("dsi", X2000_CLK_AHB0, CGU_REG_CLKGR1, 29),
	[X2000_CLK_ARB] = X2000_GATE_KEPT("arb", X2000_CLK_AHB0, CGU_REG_CLKGR1, 30),

	[X2000_CLK_CIM_MCLK] = X2000_GATE("cim_mclk", X2000_CLK_PCLK, CGU_REG_OPCR, 5),
	[X2000_CLK_USBPHY] = X2000_GATE("usb_phy", X2000_CLK_PCLK, CGU_REG_OPCR, 23),

	/* OPCR.SPENDN0 takes the USB transceiver out of suspend */
	[X2000_CLK_OTGPHY] = {
		"otg_phy", CGU_CLK_GATE,
		.parents = { X2000_CLK_USBPHY, -1, -1, -1 },
		.gate = { CGU_REG_OPCR, 7, true },
	},

	/* Power switches */

	[X2000_CLK_ISP0_PWR] = X2000_PWR("isp0_pwr", X2000_CLK_ISP0),
	[X2000_CLK_ISP1_PWR] = X2000_PWR("isp1_pwr", X2000_CLK_ISP1),
	[X2000_CLK_FELIX_PWR] = X2000_PWR("felix_pwr", X2000_CLK_FELIX),
	[X2000_CLK_HELIX_PWR] = X2000_PWR("helix_pwr", X2000_CLK_HELIX),
};

static void __init x2000_cgu_init(struct device_node *np)
{
	int retval;

	cgu = ingenic_cgu_new(x2000_cgu_clocks,
			      ARRAY_SIZE(x2000_cgu_clocks), np);
	if (!cgu) {
		pr_err("%s: failed to initialise CGU\n", __func__);
		return;
	}

	/*
	 * Bit 21 of MSC0CDR is in that register alone. The SDK's SD host
	 * driver set it whenever it chose the crystal as the source of an MSC
	 * clock, and never cleared it. What it switches is not documented;
	 * letting the crystal through to the three MSC clocks would fit. It
	 * is set here, once, before a host exists: a host setting it when it
	 * needs the crystal would write the first host's clock register while
	 * that host may be changing it.
	 */
	writel(readl(cgu->base + CGU_REG_MSC0CDR) | MSC0CDR_BIT21,
	       cgu->base + CGU_REG_MSC0CDR);

	retval = ingenic_cgu_register_clocks(cgu);
	if (retval) {
		pr_err("%s: failed to register CGU Clocks\n", __func__);
		return;
	}

	ingenic_cgu_register_syscore(cgu);
}

/*
 * CGU has some children devices, this is useful for probing children devices
 * in the case where the device node is compatible with "simple-mfd".
 */
CLK_OF_DECLARE_DRIVER(x2000_cgu, "ingenic,x2000-cgu", x2000_cgu_init);
