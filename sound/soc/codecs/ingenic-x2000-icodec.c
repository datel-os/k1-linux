// SPDX-License-Identifier: GPL-2.0
/*
 * ALSA SoC codec driver for the Ingenic X2000 internal audio codec
 * (Innosilicon).
 *
 * Covers the DAC playback path (SDTI -> DACL -> HPOUTL) with analog
 * playback volume and digital mute at zero.
 *
 * Register layout and power-up/down sequences from Ingenic's SDK kernel 6.6
 * (module_drivers/sound/soc/ingenic/icodec/icdc_inno.c, GPL-2.0).
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

/* Codec registers (32-bit aligned, index * 4) */
#define ICDC_CFG2		(0x03 * 4)
#define ICDC_CFG3		(0x04 * 4)
#define ICDC_CFG4		(0x05 * 4)
#define ICDC_DGAIN		(0x06 * 4)
#define ICDC_DMUTE		(0x07 * 4)
#define ICDC_BIAS		(0x20 * 4)
#define ICDC_PRECHG		(0x21 * 4)
#define ICDC_CTRL1		(0x22 * 4)
#define ICDC_CTRL5		(0x26 * 4)
#define ICDC_CTRL6		(0x27 * 4)
#define ICDC_CTRL7		(0x28 * 4)

#define ICDC_CFG2_MASTER_MASK	GENMASK(7, 4)
#define ICDC_DGAIN_MASK		GENMASK(2, 0)
#define ICDC_DGAIN_DEFAULT	2	/* 0 dB / SDK default (6 -> 8 - 6 = 2) */
#define ICDC_DMUTE_BIT		BIT(3)
#define ICDC_BIAS_MASK		GENMASK(3, 0)
#define ICDC_BIAS_LEVEL		7	/* stock bias_level=7 */
#define ICDC_CTRL7_VOL_MASK	GENMASK(4, 0)

#define ICDC_VOL_MAX		31
#define ICDC_VOL_DEFAULT	24

struct x2000_icodec {
	struct device *dev;
	void __iomem *base;
	struct mutex lock;
	bool powered;
	u8 volume;
	unsigned int fmt_width;
};

static void icodec_update(struct x2000_icodec *ic, unsigned int reg,
			  u32 mask, u32 val)
{
	u32 v = readl(ic->base + reg);

	v = (v & ~mask) | (val & mask);
	writel(v, ic->base + reg);
}

static void x2000_icodec_apply_format(struct x2000_icodec *ic)
{
	u32 reg;

	/* Codec is I2S clock/frame slave (BAIC0 provides BCLK and LRCLK) */
	icodec_update(ic, ICDC_CFG2, ICDC_CFG2_MASTER_MASK, 0);
	msleep(1);

	reg = 0x02;
	if (ic->fmt_width > 16)
		reg |= (0x3 << 2);
	writel(reg, ic->base + ICDC_CFG4);
	msleep(1);

	reg = 0x10 | (0x1 << 2);
	switch (ic->fmt_width) {
	case 32:
		reg |= (0x3 << 5);
		break;
	case 24:
		reg |= (0x2 << 5);
		break;
	case 20:
		reg |= (0x1 << 5);
		break;
	default:
		break;
	}
	writel(reg, ic->base + ICDC_CFG3);
	msleep(1);
}

static void x2000_icodec_apply_volume(struct x2000_icodec *ic)
{
	icodec_update(ic, ICDC_DGAIN, ICDC_DGAIN_MASK, ICDC_DGAIN_DEFAULT);
	icodec_update(ic, ICDC_CTRL7, ICDC_CTRL7_VOL_MASK, ic->volume);
	icodec_update(ic, ICDC_DMUTE, ICDC_DMUTE_BIT,
		      ic->volume ? 0 : ICDC_DMUTE_BIT);
}

static void x2000_icodec_power_on(struct x2000_icodec *ic)
{
	int i;

	icodec_update(ic, ICDC_CTRL5, GENMASK(1, 0), 1);
	msleep(10);

	writel(0x01, ic->base + ICDC_PRECHG);
	msleep(10);

	icodec_update(ic, ICDC_CTRL1, BIT(5), BIT(5));
	msleep(10);

	for (i = 0; i < 8; i++) {
		icodec_update(ic, ICDC_PRECHG, BIT(i), BIT(i));
		msleep(20);
	}
	writel(0xa0, ic->base + ICDC_PRECHG);
	msleep(10);
}

static void x2000_icodec_dac_enable(struct x2000_icodec *ic)
{
	icodec_update(ic, ICDC_BIAS, ICDC_BIAS_MASK, ICDC_BIAS_LEVEL);
	msleep(10);

	icodec_update(ic, ICDC_CTRL7, BIT(5), BIT(5));
	msleep(10);
	icodec_update(ic, ICDC_CTRL7, BIT(6), BIT(6));
	msleep(10);

	icodec_update(ic, ICDC_CTRL6, BIT(7), BIT(7));
	msleep(1);
	icodec_update(ic, ICDC_CTRL6, BIT(6), BIT(6));
	msleep(1);
	icodec_update(ic, ICDC_CTRL6, BIT(5), BIT(5));
	msleep(1);
	icodec_update(ic, ICDC_CTRL6, BIT(4), BIT(4));
	msleep(1);

	icodec_update(ic, ICDC_CTRL7, BIT(7), BIT(7));
	msleep(1);

	x2000_icodec_apply_volume(ic);
	msleep(1);
}

static void x2000_icodec_dac_disable(struct x2000_icodec *ic)
{
	icodec_update(ic, ICDC_DMUTE, ICDC_DMUTE_BIT, ICDC_DMUTE_BIT);
	icodec_update(ic, ICDC_CTRL7, ICDC_CTRL7_VOL_MASK, 0);
	msleep(10);

	icodec_update(ic, ICDC_CTRL7, BIT(7), 0);
	msleep(10);
	icodec_update(ic, ICDC_CTRL7, BIT(6), 0);
	msleep(10);
	icodec_update(ic, ICDC_CTRL7, BIT(5), 0);
	msleep(10);

	icodec_update(ic, ICDC_CTRL6, BIT(5), 0);
	msleep(10);
	icodec_update(ic, ICDC_CTRL6, BIT(6), 0);
	msleep(1);
	icodec_update(ic, ICDC_CTRL6, BIT(7), 0);
	msleep(1);
	icodec_update(ic, ICDC_CTRL6, BIT(4), 0);
	msleep(1);
}

static void x2000_icodec_power_off(struct x2000_icodec *ic)
{
	int i;

	writel(0x01, ic->base + ICDC_PRECHG);
	msleep(10);

	icodec_update(ic, ICDC_CTRL1, BIT(5), 0);
	msleep(10);

	for (i = 0; i < 8; i++) {
		icodec_update(ic, ICDC_PRECHG, BIT(i), BIT(i));
		msleep(10);
	}

	writel(0x00, ic->base + ICDC_PRECHG);
	msleep(10);
}

static int x2000_icodec_dac_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *comp = snd_soc_dapm_to_component(w->dapm);
	struct x2000_icodec *ic = snd_soc_component_get_drvdata(comp);

	mutex_lock(&ic->lock);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (!ic->powered) {
			x2000_icodec_power_on(ic);
			x2000_icodec_apply_format(ic);
			x2000_icodec_dac_enable(ic);
			ic->powered = true;
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (ic->powered) {
			x2000_icodec_dac_disable(ic);
			x2000_icodec_power_off(ic);
			ic->powered = false;
		}
		break;
	}
	mutex_unlock(&ic->lock);

	return 0;
}

static int x2000_icodec_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BC_FC)
		return -EINVAL;

	return 0;
}

static int x2000_icodec_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct x2000_icodec *ic = snd_soc_component_get_drvdata(dai->component);
	unsigned int width = params_width(params);

	if (width != 16 && width != 20 && width != 24 && width != 32)
		return -EINVAL;

	mutex_lock(&ic->lock);
	ic->fmt_width = width;
	if (ic->powered)
		x2000_icodec_apply_format(ic);
	mutex_unlock(&ic->lock);

	return 0;
}

static const struct snd_soc_dai_ops x2000_icodec_dai_ops = {
	.set_fmt	= x2000_icodec_set_fmt,
	.hw_params	= x2000_icodec_hw_params,
};

static struct snd_soc_dai_driver x2000_icodec_dai = {
	.name = "x2000-icodec",
	.playback = {
		.stream_name	= "Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_96000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S24_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &x2000_icodec_dai_ops,
};

static int x2000_icodec_vol_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct x2000_icodec *ic = snd_soc_component_get_drvdata(comp);

	mutex_lock(&ic->lock);
	ucontrol->value.integer.value[0] = ic->volume;
	mutex_unlock(&ic->lock);

	return 0;
}

static int x2000_icodec_vol_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct x2000_icodec *ic = snd_soc_component_get_drvdata(comp);
	long val = ucontrol->value.integer.value[0];
	int changed = 0;

	if (val < 0 || val > ICDC_VOL_MAX)
		return -EINVAL;

	mutex_lock(&ic->lock);
	if (ic->volume != val) {
		ic->volume = val;
		changed = 1;
		if (ic->powered)
			x2000_icodec_apply_volume(ic);
	}
	mutex_unlock(&ic->lock);

	return changed;
}

/* -39 dB to +6 dB in 1.5 dB steps; 0 mutes digitally */
static const DECLARE_TLV_DB_SCALE(x2000_icodec_vol_tlv, -3900, 150, 1);

static const struct snd_kcontrol_new x2000_icodec_controls[] = {
	SOC_SINGLE_EXT_TLV("Speaker Playback Volume", 0, 0, ICDC_VOL_MAX, 0,
			   x2000_icodec_vol_get, x2000_icodec_vol_put,
			   x2000_icodec_vol_tlv),
};

static const struct snd_soc_dapm_widget x2000_icodec_widgets[] = {
	SND_SOC_DAPM_AIF_IN("SDTI", "Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DAC_E("DACL", NULL, SND_SOC_NOPM, 0, 0,
			   x2000_icodec_dac_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("HPOUTL"),
};

static const struct snd_soc_dapm_route x2000_icodec_routes[] = {
	{ "DACL", NULL, "SDTI" },
	{ "HPOUTL", NULL, "DACL" },
};

static const struct snd_soc_component_driver x2000_icodec_component = {
	.controls		= x2000_icodec_controls,
	.num_controls		= ARRAY_SIZE(x2000_icodec_controls),
	.dapm_widgets		= x2000_icodec_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(x2000_icodec_widgets),
	.dapm_routes		= x2000_icodec_routes,
	.num_dapm_routes	= ARRAY_SIZE(x2000_icodec_routes),
};

static int x2000_icodec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct x2000_icodec *ic;

	ic = devm_kzalloc(dev, sizeof(*ic), GFP_KERNEL);
	if (!ic)
		return -ENOMEM;

	ic->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ic->base))
		return PTR_ERR(ic->base);

	ic->dev = dev;
	ic->volume = ICDC_VOL_DEFAULT;
	ic->fmt_width = 16;
	mutex_init(&ic->lock);

	platform_set_drvdata(pdev, ic);

	return devm_snd_soc_register_component(dev, &x2000_icodec_component,
					       &x2000_icodec_dai, 1);
}

static const struct of_device_id x2000_icodec_of_match[] = {
	{ .compatible = "ingenic,x2000-icodec" },
	{ }
};
MODULE_DEVICE_TABLE(of, x2000_icodec_of_match);

static struct platform_driver x2000_icodec_driver = {
	.driver = {
		.name = "x2000-icodec",
		.of_match_table = x2000_icodec_of_match,
	},
	.probe = x2000_icodec_probe,
};
module_platform_driver(x2000_icodec_driver);

MODULE_DESCRIPTION("Ingenic X2000 internal audio codec driver");
MODULE_LICENSE("GPL");
