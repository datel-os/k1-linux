// SPDX-License-Identifier: GPL-2.0
/*
 * ALSA SoC audio playback driver for the Ingenic X2000 Audio System v2.
 *
 * Drives the fixed playback path to the internal codec:
 *   AS DMA ch 0 -> AS FIFO ch 0 -> AS Format Converter ch 0
 *   -> AS DSP (LI8 -> timeslot 1 -> LO0) -> BAIC0 TX (I2S).
 *
 * Register layout and sequences from Ingenic's SDK kernel 6.6
 * (module_drivers/sound/soc/ingenic/as-v2/{as-dma,as-fmtcov,as-dsp,as-baic}.c,
 * GPL-2.0).
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* AS DMA channel 0 and global registers */
#define AS_DMA_DBA0		0x00
#define AS_DMA_DTC0		0x04
#define AS_DMA_DCR0		0x08
#define AS_DMA_DSR0		0x0c
#define AS_DMA_DCM0		0x10
#define AS_DMA_DDA0		0x14
#define AS_DMA_DGRR		0x100
#define AS_DMA_DGER		0x104
#define AS_DMA_AIPR		0x110

#define AS_DMA_DCR_RESET	BIT(1)
#define AS_DMA_DCR_CTE		BIT(0)

#define AS_DMA_DSR_TT_INT	BIT(5)
#define AS_DMA_DSR_LTT_INT	BIT(4)
#define AS_DMA_DSR_TT		BIT(3)
#define AS_DMA_DSR_LTT		BIT(2)
#define AS_DMA_DSR_RST_EN	BIT(1)
#define AS_DMA_DSR_CLR_ALL	(AS_DMA_DSR_TT_INT | AS_DMA_DSR_LTT_INT | \
				 AS_DMA_DSR_TT | AS_DMA_DSR_LTT)

#define AS_DMA_DCM_NDES		BIT(31)
#define AS_DMA_DCM_TIE		BIT(1)

#define AS_DMA_DGRR_RESET	BIT(0)
#define AS_DMA_DGER_EN		BIT(0)
#define AS_DMA_AIPR_CH0		BIT(0)

/* Fixed 32-word (128-byte) burst per hardware FTH quirk */
#define AS_DMA_BURST_CODE	4
#define AS_DMA_BURST_WORDS	32
#define AS_DMA_BURST_BYTES	(AS_DMA_BURST_WORDS * 4)

/* AS FIFO channel 0 registers */
#define AS_FIFO_FAS0		0x00
#define AS_FIFO_FCR0		0x04
#define AS_FIFO_FFR0		0x08

#define AS_FIFO_DEPTH_WORDS	384
#define AS_FIFO_FCR_EN		BIT(0)
#define AS_FIFO_FFR_FTH_MASK	GENMASK(28, 16)

/* AS Format Converter channel 0 register */
#define AS_FMTCOV_DFCR0		0x00
#define AS_FMTCOV_CHNUM_MASK	GENMASK(15, 12)
#define AS_FMTCOV_SS_MASK	GENMASK(10, 8)
#define AS_FMTCOV_ENABLE	BIT(0)

/* AS DSP / DBUS routing registers */
#define AS_DSP_BTSET		0x00
#define AS_DSP_BFCR0		0x10
#define AS_DSP_BFCR1		0x14
#define AS_DSP_BFCR2		0x18
#define AS_DSP_BST1		0x20
#define AS_DSP_BTT0		0x28

#define AS_DSP_BFCR0_TF0	BIT(16)
#define AS_DSP_BFCR1_DBE_MASK	GENMASK(11, 0)
#define AS_DSP_BFCR2_DBE_MASK	GENMASK(14, 0)
#define AS_DSP_LI8_SHIFT	10	/* DMA0 source port in BST1 */
#define AS_DSP_LO0_SHIFT	0	/* BAIC0 TX target port in BTT0 */
#define AS_DSP_TIMESLOT		1

/* BAIC0 registers */
#define BAIC_TCTL		0x10
#define BAIC_TCFG		0x14
#define BAIC_TDIV		0x18
#define BAIC_TLCR		0x20
#define BAIC_CCR		0x24

#define BAIC_TCTL_RST		BIT(0)
#define BAIC_TCFG_CH_MASK	GENMASK(19, 17)
#define BAIC_TCFG_SS_MASK	GENMASK(10, 8)
#define BAIC_TCFG_MODE_MASK	GENMASK(5, 2)
#define BAIC_TCFG_MODE_I2S	4
#define BAIC_TCFG_MASTER	BIT(0)
#define BAIC_TDIV_BCLK_MASK	GENMASK(23, 16)
#define BAIC_TDIV_SYNC_MASK	GENMASK(7, 0)
#define BAIC_TLCR_CLK_SPLIT	BIT(0)
#define BAIC_CCR_TEN		BIT(3)

#define X2000_AS_BUF_MAX	(256 * 1024)
#define X2000_AS_PERIOD_MIN	4096
#define X2000_AS_PERIODS_MIN	4
#define X2000_AS_MAX_DESCS	(X2000_AS_BUF_MAX / X2000_AS_PERIOD_MIN)

struct x2000_as_dma_desc {
	u32 cmd;
	u32 dba;
	u32 ndoa;
	u32 dtc;
} __packed;

#define AS_DESC_TIE		BIT(1)
#define AS_DESC_LINK		BIT(2)
#define AS_DESC_TSZ_MASK	GENMASK(6, 4)
#define AS_DESC_BAI		BIT(8)

struct x2000_as {
	struct device *dev;
	void __iomem *dma_base;
	void __iomem *fifo_base;
	void __iomem *fmtcov_base;
	void __iomem *dsp_base;
	void __iomem *baic_base;

	struct clk *clk_baic0;
	struct clk *clk_i2s_tx;

	struct dma_pool *desc_pool;
	struct x2000_as_dma_desc *descs[X2000_AS_MAX_DESCS];
	dma_addr_t descs_phy[X2000_AS_MAX_DESCS];
	unsigned int desc_cnt;

	spinlock_t lock;
	struct snd_pcm_substream *substream;
	dma_addr_t hw_ptr;
	u32 baic_tcfg;
	u32 baic_tdiv;
};

static u32 x2000_as_ss_code(int width)
{
	switch (width) {
	case 8:
		return 0;
	case 16:
		return 3;
	case 20:
		return 5;
	case 24:
		return 6;
	case 32:
		return 7;
	default:
		return 3;
	}
}

static const struct snd_pcm_hardware x2000_as_pcm_hw = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S24_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
	.rates			= SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_96000,
	.rate_min		= 8000,
	.rate_max		= 96000,
	.channels_min		= 1,
	.channels_max		= 2,
	.buffer_bytes_max	= X2000_AS_BUF_MAX,
	.period_bytes_min	= X2000_AS_PERIOD_MIN,
	.period_bytes_max	= X2000_AS_BUF_MAX / X2000_AS_PERIODS_MIN,
	.periods_min		= X2000_AS_PERIODS_MIN,
	.periods_max		= X2000_AS_MAX_DESCS,
	.fifo_size		= AS_FIFO_DEPTH_WORDS * 4,
};

static void x2000_as_free_descs(struct x2000_as *as)
{
	unsigned int i;

	for (i = 0; i < as->desc_cnt; i++) {
		dma_pool_free(as->desc_pool, as->descs[i], as->descs_phy[i]);
		as->descs[i] = NULL;
		as->descs_phy[i] = 0;
	}
	as->desc_cnt = 0;
}

static int x2000_as_alloc_descs(struct x2000_as *as, unsigned int cnt)
{
	unsigned int i;

	if (cnt > X2000_AS_MAX_DESCS)
		return -EINVAL;

	x2000_as_free_descs(as);

	for (i = 0; i < cnt; i++) {
		as->descs[i] = dma_pool_alloc(as->desc_pool, GFP_KERNEL,
					      &as->descs_phy[i]);
		if (!as->descs[i]) {
			x2000_as_free_descs(as);
			return -ENOMEM;
		}
		as->desc_cnt++;
	}

	return 0;
}

static int x2000_as_pcm_open(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	struct x2000_as *as = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	int ret;

	snd_soc_set_runtime_hwparams(substream, &x2000_as_pcm_hw);

	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;

	ret = snd_pcm_hw_constraint_step(runtime, 0,
					 SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					 AS_DMA_BURST_BYTES);
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&as->lock, flags);
	as->substream = substream;
	as->hw_ptr = 0;
	spin_unlock_irqrestore(&as->lock, flags);

	return 0;
}

static int x2000_as_pcm_close(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream)
{
	struct x2000_as *as = snd_soc_component_get_drvdata(component);
	unsigned long flags;

	spin_lock_irqsave(&as->lock, flags);
	as->substream = NULL;
	as->hw_ptr = 0;
	spin_unlock_irqrestore(&as->lock, flags);

	return 0;
}

static int x2000_as_pcm_hw_params(struct snd_soc_component *component,
				  struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct x2000_as *as = snd_soc_component_get_drvdata(component);
	unsigned int periods = params_periods(params);
	unsigned int period_bytes = params_period_bytes(params);
	unsigned int channels = params_channels(params);
	unsigned int width = params_width(params);
	dma_addr_t dma_addr = substream->runtime->dma_addr;
	u32 cmd, dfcr;
	unsigned int i;
	int ret;

	ret = x2000_as_alloc_descs(as, periods);
	if (ret < 0)
		return ret;

	cmd = AS_DESC_LINK | AS_DESC_TIE | AS_DESC_BAI |
	      FIELD_PREP(AS_DESC_TSZ_MASK, AS_DMA_BURST_CODE);

	for (i = 0; i < periods; i++) {
		as->descs[i]->cmd = cmd;
		as->descs[i]->dba = dma_addr + i * period_bytes;
		as->descs[i]->ndoa = as->descs_phy[(i + 1) % periods];
		as->descs[i]->dtc = period_bytes / AS_DMA_BURST_BYTES;
	}
	wmb();

	dfcr = FIELD_PREP(AS_FMTCOV_CHNUM_MASK, channels - 1) |
	       FIELD_PREP(AS_FMTCOV_SS_MASK, x2000_as_ss_code(width));
	writel(dfcr, as->fmtcov_base + AS_FMTCOV_DFCR0);

	writel(FIELD_PREP(AS_FIFO_FFR_FTH_MASK, AS_DMA_BURST_WORDS),
	       as->fifo_base + AS_FIFO_FFR0);

	return 0;
}

static int x2000_as_pcm_hw_free(struct snd_soc_component *component,
				struct snd_pcm_substream *substream)
{
	struct x2000_as *as = snd_soc_component_get_drvdata(component);

	x2000_as_free_descs(as);
	return 0;
}

static snd_pcm_uframes_t
x2000_as_pcm_pointer(struct snd_soc_component *component,
		     struct snd_pcm_substream *substream)
{
	struct x2000_as *as = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	dma_addr_t base = runtime->dma_addr;
	size_t buf_bytes = snd_pcm_lib_buffer_bytes(substream);
	dma_addr_t now;
	size_t pos = 0;

	now = readl(as->dma_base + AS_DMA_DBA0);
	if (now >= base && now < base + buf_bytes)
		pos = now - base;
	else if (as->hw_ptr >= base && as->hw_ptr < base + buf_bytes)
		pos = as->hw_ptr - base;

	return bytes_to_frames(runtime, pos);
}

static int x2000_as_pcm_construct(struct snd_soc_component *component,
				  struct snd_soc_pcm_runtime *rtd)
{
	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
				       component->dev,
				       64 * 1024, X2000_AS_BUF_MAX);
	return 0;
}

static int x2000_as_dai_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct x2000_as *as = dev_get_drvdata(dai->dev);
	int ret;

	ret = clk_prepare_enable(as->clk_baic0);
	if (ret)
		return ret;

	ret = clk_prepare_enable(as->clk_i2s_tx);
	if (ret) {
		clk_disable_unprepare(as->clk_baic0);
		return ret;
	}

	return 0;
}

static void x2000_as_dai_shutdown(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct x2000_as *as = dev_get_drvdata(dai->dev);

	clk_disable_unprepare(as->clk_i2s_tx);
	clk_disable_unprepare(as->clk_baic0);
}

static int x2000_as_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BP_FP)
		return -EINVAL;

	return 0;
}

static int x2000_as_dai_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct x2000_as *as = dev_get_drvdata(dai->dev);
	unsigned int rate = params_rate(params);
	unsigned int channels = params_channels(params);
	unsigned int width = params_width(params);
	unsigned int sync_div = (width > 16) ? 64 : 32;
	unsigned int bclk_div = 256 / sync_div;
	int ret;

	/* MCLK = 256 * sample_rate; BAIC0 divides it into BCLK and LRCLK */
	ret = clk_set_rate(as->clk_i2s_tx, 256UL * rate);
	if (ret)
		return ret;

	as->baic_tdiv = FIELD_PREP(BAIC_TDIV_BCLK_MASK, bclk_div) |
			FIELD_PREP(BAIC_TDIV_SYNC_MASK, (sync_div / 16) - 1);
	as->baic_tcfg = FIELD_PREP(BAIC_TCFG_CH_MASK, channels >> 1) |
			FIELD_PREP(BAIC_TCFG_SS_MASK, x2000_as_ss_code(width)) |
			FIELD_PREP(BAIC_TCFG_MODE_MASK, BAIC_TCFG_MODE_I2S) |
			BAIC_TCFG_MASTER;

	return 0;
}

static void x2000_as_start(struct x2000_as *as,
			   struct snd_pcm_substream *substream)
{
	u32 val;

	as->hw_ptr = substream->runtime->dma_addr;

	/* Flush DSP TX FIFO 0 */
	writel(AS_DSP_BFCR0_TF0, as->dsp_base + AS_DSP_BFCR0);

	/* Start DMA channel 0 before FIFO and BAIC */
	writel(AS_DMA_DSR_CLR_ALL, as->dma_base + AS_DMA_DSR0);
	writel(AS_DMA_DCM_TIE, as->dma_base + AS_DMA_DCM0);
	writel(as->descs_phy[0], as->dma_base + AS_DMA_DDA0);
	readl(as->dma_base + AS_DMA_DDA0);

	val = readl(as->fmtcov_base + AS_FMTCOV_DFCR0);
	writel(val | AS_FMTCOV_ENABLE, as->fmtcov_base + AS_FMTCOV_DFCR0);

	writel(AS_DMA_DCR_CTE, as->dma_base + AS_DMA_DCR0);
	writel(AS_FIFO_FCR_EN, as->fifo_base + AS_FIFO_FCR0);

	/* Start BAIC0 transmitter */
	writel(as->baic_tdiv, as->baic_base + BAIC_TDIV);
	writel(as->baic_tcfg, as->baic_base + BAIC_TCFG);
	writel(BAIC_TCTL_RST, as->baic_base + BAIC_TCTL);
	writel(BAIC_CCR_TEN, as->baic_base + BAIC_CCR);
}

static void x2000_as_stop(struct x2000_as *as)
{
	unsigned int timeout = 4096;
	u32 val;

	/* Stop BAIC0 transmitter and clock output first */
	writel(0, as->baic_base + BAIC_CCR);
	writel(as->baic_tcfg & ~BAIC_TCFG_MASTER, as->baic_base + BAIC_TCFG);

	/* Stop DMA and FIFO */
	writel(0, as->dma_base + AS_DMA_DCM0);
	writel(AS_DMA_DSR_CLR_ALL, as->dma_base + AS_DMA_DSR0);
	writel(0, as->fifo_base + AS_FIFO_FCR0);
	writel(0, as->dma_base + AS_DMA_DCR0);

	while ((readl(as->dma_base + AS_DMA_DSR0) & AS_DMA_DSR_RST_EN) &&
	       --timeout)
		cpu_relax();

	writel(AS_DMA_DCR_RESET, as->dma_base + AS_DMA_DCR0);
	udelay(10);

	val = readl(as->fmtcov_base + AS_FMTCOV_DFCR0);
	writel(val & ~AS_FMTCOV_ENABLE, as->fmtcov_base + AS_FMTCOV_DFCR0);
}

static int x2000_as_dai_trigger(struct snd_pcm_substream *substream, int cmd,
				struct snd_soc_dai *dai)
{
	struct x2000_as *as = dev_get_drvdata(dai->dev);
	unsigned long flags;

	spin_lock_irqsave(&as->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		x2000_as_start(as, substream);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		x2000_as_stop(as);
		break;
	default:
		spin_unlock_irqrestore(&as->lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&as->lock, flags);

	return 0;
}

static const struct snd_soc_dai_ops x2000_as_dai_ops = {
	.startup	= x2000_as_dai_startup,
	.shutdown	= x2000_as_dai_shutdown,
	.set_fmt	= x2000_as_dai_set_fmt,
	.hw_params	= x2000_as_dai_hw_params,
	.trigger	= x2000_as_dai_trigger,
};

static struct snd_soc_dai_driver x2000_as_dai = {
	.name = "x2000-baic0",
	.playback = {
		.stream_name	= "BAIC0 Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_96000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S24_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &x2000_as_dai_ops,
};

static const struct snd_soc_component_driver x2000_as_component = {
	.name		= "x2000-as",
	.open		= x2000_as_pcm_open,
	.close		= x2000_as_pcm_close,
	.hw_params	= x2000_as_pcm_hw_params,
	.hw_free	= x2000_as_pcm_hw_free,
	.pointer	= x2000_as_pcm_pointer,
	.pcm_construct	= x2000_as_pcm_construct,
};

static irqreturn_t x2000_as_irq(int irq, void *dev_id)
{
	struct x2000_as *as = dev_id;
	struct snd_pcm_substream *substream = NULL;
	unsigned long flags;
	u32 pending, status;

	pending = readl(as->dma_base + AS_DMA_AIPR);
	if (!(pending & AS_DMA_AIPR_CH0))
		return IRQ_NONE;

	status = readl(as->dma_base + AS_DMA_DSR0);
	writel(status, as->dma_base + AS_DMA_DSR0);

	if (status & AS_DMA_DSR_TT_INT) {
		spin_lock_irqsave(&as->lock, flags);
		if (as->substream) {
			as->hw_ptr = readl(as->dma_base + AS_DMA_DBA0);
			substream = as->substream;
		}
		spin_unlock_irqrestore(&as->lock, flags);

		if (substream)
			snd_pcm_period_elapsed(substream);
	}

	return IRQ_HANDLED;
}

static int x2000_as_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct x2000_as *as;
	struct clk *clk_audio;
	int irq, ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	as = devm_kzalloc(dev, sizeof(*as), GFP_KERNEL);
	if (!as)
		return -ENOMEM;

	as->dev = dev;
	spin_lock_init(&as->lock);

	as->dma_base = devm_platform_ioremap_resource_byname(pdev, "dma");
	if (IS_ERR(as->dma_base))
		return PTR_ERR(as->dma_base);

	as->fifo_base = devm_platform_ioremap_resource_byname(pdev, "fifo");
	if (IS_ERR(as->fifo_base))
		return PTR_ERR(as->fifo_base);

	as->fmtcov_base = devm_platform_ioremap_resource_byname(pdev, "fmtcov");
	if (IS_ERR(as->fmtcov_base))
		return PTR_ERR(as->fmtcov_base);

	as->dsp_base = devm_platform_ioremap_resource_byname(pdev, "dsp");
	if (IS_ERR(as->dsp_base))
		return PTR_ERR(as->dsp_base);

	as->baic_base = devm_platform_ioremap_resource_byname(pdev, "baic");
	if (IS_ERR(as->baic_base))
		return PTR_ERR(as->baic_base);

	clk_audio = devm_clk_get_enabled(dev, "audio");
	if (IS_ERR(clk_audio))
		return dev_err_probe(dev, PTR_ERR(clk_audio),
				     "failed to get audio clock\n");

	as->clk_baic0 = devm_clk_get(dev, "baic0");
	if (IS_ERR(as->clk_baic0))
		return dev_err_probe(dev, PTR_ERR(as->clk_baic0),
				     "failed to get baic0 clock\n");

	as->clk_i2s_tx = devm_clk_get(dev, "i2s_tx");
	if (IS_ERR(as->clk_i2s_tx))
		return dev_err_probe(dev, PTR_ERR(as->clk_i2s_tx),
				     "failed to get i2s_tx clock\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	as->desc_pool = dmam_pool_create("x2000_as_desc", dev,
					 sizeof(struct x2000_as_dma_desc),
					 16, 0);
	if (!as->desc_pool)
		return -ENOMEM;

	/* Reset and enable DMA engine, assign FIFO space to channel 0 */
	writel(AS_DMA_DGRR_RESET, as->dma_base + AS_DMA_DGRR);
	writel(AS_DMA_DGER_EN, as->dma_base + AS_DMA_DGER);
	writel(AS_FIFO_DEPTH_WORDS, as->fifo_base + AS_FIFO_FAS0);

	/* Fixed DBUS route: DMA0 (LI8) -> timeslot 1 -> BAIC0 TX (LO0) */
	writel(AS_DSP_BFCR1_DBE_MASK, as->dsp_base + AS_DSP_BFCR1);
	writel(AS_DSP_BFCR2_DBE_MASK, as->dsp_base + AS_DSP_BFCR2);
	writel(AS_DSP_TIMESLOT << AS_DSP_LI8_SHIFT, as->dsp_base + AS_DSP_BST1);
	writel(AS_DSP_TIMESLOT << AS_DSP_LO0_SHIFT, as->dsp_base + AS_DSP_BTT0);
	writel(BIT(AS_DSP_TIMESLOT), as->dsp_base + AS_DSP_BTSET);

	/* BAIC0 uses split TX/RX clocks */
	writel(BAIC_TLCR_CLK_SPLIT, as->baic_base + BAIC_TLCR);
	writel(0, as->baic_base + BAIC_CCR);

	platform_set_drvdata(pdev, as);

	ret = devm_request_irq(dev, irq, x2000_as_irq, IRQF_SHARED,
			       "x2000-as", as);
	if (ret)
		return ret;

	return devm_snd_soc_register_component(dev, &x2000_as_component,
					       &x2000_as_dai, 1);
}

static const struct of_device_id x2000_as_of_match[] = {
	{ .compatible = "ingenic,x2000-as" },
	{ }
};
MODULE_DEVICE_TABLE(of, x2000_as_of_match);

static struct platform_driver x2000_as_driver = {
	.driver = {
		.name = "x2000-as",
		.of_match_table = x2000_as_of_match,
	},
	.probe = x2000_as_probe,
};
module_platform_driver(x2000_as_driver);

MODULE_DESCRIPTION("Ingenic X2000 Audio System v2 playback driver");
MODULE_LICENSE("GPL");
