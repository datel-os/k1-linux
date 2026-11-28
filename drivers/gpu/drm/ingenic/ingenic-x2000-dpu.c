// SPDX-License-Identifier: GPL-2.0
/*
 * DRM driver for the display controller (DPU) of the Ingenic X2000.
 *
 * The DPU has two ways to feed its TFT output: a composer of four layers
 * and a "simple read" DMA channel (RDMA) that sends one buffer as it is.
 * This driver uses the RDMA alone: one plane, one CRTC, the panel behind
 * a bridge. The RDMA walks a chain of descriptors; here the chain is one
 * descriptor that points at itself, so the controller sends the same
 * buffer every frame, and a page flip writes the new address into it.
 *
 * Register layout and bit meanings from Ingenic's SDK driver
 * (module_drivers/drivers/video/fbdev/ingenic/fb_stage, GPL-2.0).
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#define DC_RDMA_CHAIN_ADDR	0x1000
#define DC_RDMA_CHAIN_CTRL	0x1004
#define DC_CTRL			0x2000
#define DC_ST			0x2004
#define DC_CLR_ST		0x2008
#define DC_INTC			0x200c
#define DC_INT_FLAG		0x2010
#define DC_COM_CONFIG		0x2014
#define DC_PCFG_RD_CTRL		0x2018
#define DC_RDMA_CHAIN_SITE	0x2204
#define DC_DISP_COM		0x8000
#define DC_TFT_HSYNC		0x9000
#define DC_TFT_VSYNC		0x9004
#define DC_TFT_HDE		0x9008
#define DC_TFT_VDE		0x900c
#define DC_TFT_CFG		0x9010

/* DC_RDMA_CHAIN_CTRL */
#define DC_RDMA_START		BIT(0)

/* DC_CTRL */
#define DC_GEN_STP_RDMA		BIT(4)
#define DC_GEN_STP_CMP		BIT(3)
#define DC_QCK_STP_RDMA		BIT(1)
#define DC_QCK_STP_CMP		BIT(0)

/* DC_ST, DC_CLR_ST, DC_INTC and DC_INT_FLAG share these positions */
#define DC_TFT_UNDR		BIT(8)
#define DC_STOP_SRD_ACK		BIT(7)
#define DC_DIRECT_WORKING	BIT(4)
#define DC_SRD_WORKING		BIT(3)
#define DC_SRD_START		BIT(2)
#define DC_SRD_END		BIT(1)
#define DC_CLR_ALL		0x01fffffe

/* DC_COM_CONFIG */
#define DC_BURST_LEN_WDMA	GENMASK(7, 6)
#define DC_BURST_LEN_RDMA	GENMASK(5, 4)
#define DC_BURST_LEN_BDMA	GENMASK(3, 2)
#define DC_BURST_16		2
#define DC_OUT_SEL		GENMASK(1, 0)
#define DC_OUT_SEL_RDMA		2

/* DC_PCFG_RD_CTRL */
#define DC_COMP_ARQOS_CTRL	BIT(0)
#define DC_COMP_ARQOS_VAL	GENMASK(2, 1)

/* DC_DISP_COM */
#define DC_DP_DITHER_DW		GENMASK(21, 16)
#define DC_DITHER_TO_565	0x26	/* R and B drop 3 bits, G drops 2 */
#define DC_DITHER_TO_666	0x15	/* each drops 2 bits */
#define DC_DP_DITHER_EN		BIT(4)
#define DC_DP_IF_SEL		GENMASK(2, 0)
#define DC_DP_IF_TFT		1

/* DC_TFT_HSYNC, _VSYNC, _HDE, _VDE: start in 27:16, end in 11:0 */
#define DC_TFT_START		GENMASK(27, 16)
#define DC_TFT_END		GENMASK(11, 0)

/* DC_TFT_CFG */
#define DC_DE_DL		BIT(9)
#define DC_SYNC_DL		BIT(8)
#define DC_VSYNC_DL		BIT(7)
#define DC_COLOR_EVEN		GENMASK(21, 19)
#define DC_COLOR_ODD		GENMASK(18, 16)
#define DC_TFT_MODE		GENMASK(2, 0)
#define DC_TFT_MODE_888		0
#define DC_TFT_MODE_666		1
#define DC_TFT_MODE_565		2

/* RDMA descriptor, chain configuration word */
#define RDMA_CHAIN_END		BIT(0)
#define RDMA_COLOR		GENMASK(18, 16)
#define RDMA_FORMAT		GENMASK(22, 19)
#define RDMA_FORMAT_565		2
#define RDMA_FORMAT_888		4
#define RDMA_COLOR_RGB		0

/* RDMA descriptor, interrupt control word */
#define RDMA_IRQ_SOS		BIT(2)

struct x2000_rdma_desc {
	u32 next;
	u32 buffer;
	u32 stride;		/* in pixels */
	u32 chain_cfg;
	u32 irq_ctrl;
} __aligned(8);

struct x2000_dpu {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	void __iomem *base;
	struct clk *clk;

	struct x2000_rdma_desc *desc;
	dma_addr_t desc_phys;
	bool running;
};

static inline struct x2000_dpu *pipe_to_dpu(struct drm_simple_display_pipe *pipe)
{
	return container_of(pipe, struct x2000_dpu, pipe);
}

static inline u32 dpu_read(struct x2000_dpu *dpu, u32 reg)
{
	return readl(dpu->base + reg);
}

static inline void dpu_write(struct x2000_dpu *dpu, u32 reg, u32 val)
{
	writel(val, dpu->base + reg);
}

static const u32 x2000_dpu_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
};

static u32 x2000_dpu_rdma_format(u32 fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_RGB565:
		return FIELD_PREP(RDMA_FORMAT, RDMA_FORMAT_565);
	case DRM_FORMAT_XRGB8888:
	default:
		return FIELD_PREP(RDMA_FORMAT, RDMA_FORMAT_888);
	}
}

/*
 * Stop both channels at once, whoever started them: the RTOS that puts up
 * the boot logo leaves the controller running.
 */
static void x2000_dpu_quick_stop(struct x2000_dpu *dpu)
{
	u32 st;

	dpu_write(dpu, DC_CTRL, DC_QCK_STP_RDMA | DC_QCK_STP_CMP);
	if (readl_poll_timeout(dpu->base + DC_ST, st,
			       !(st & (DC_SRD_WORKING | DC_DIRECT_WORKING)),
			       10, 100000))
		drm_warn(&dpu->drm, "controller did not stop, status %08x\n", st);
	dpu_write(dpu, DC_CLR_ST, DC_CLR_ALL);
}

static u32 x2000_dpu_bus_format(struct drm_simple_display_pipe *pipe)
{
	struct drm_connector *connector = pipe->connector;
	struct drm_connector_list_iter iter;
	u32 fmt = MEDIA_BUS_FMT_RGB888_1X24;

	if (connector) {
		if (connector->display_info.num_bus_formats)
			fmt = connector->display_info.bus_formats[0];
		return fmt;
	}

	drm_connector_list_iter_begin(pipe->crtc.dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		if (connector->display_info.num_bus_formats) {
			fmt = connector->display_info.bus_formats[0];
			break;
		}
	}
	drm_connector_list_iter_end(&iter);

	return fmt;
}

static void x2000_dpu_set_timing(struct x2000_dpu *dpu,
				 const struct drm_display_mode *m)
{
	u32 hsw = m->hsync_end - m->hsync_start;
	u32 hbp = m->htotal - m->hsync_end;
	u32 vsw = m->vsync_end - m->vsync_start;
	u32 vbp = m->vtotal - m->vsync_end;

	/* Counted from the start of the sync pulse, which begins at 0. */
	dpu_write(dpu, DC_TFT_HSYNC, FIELD_PREP(DC_TFT_START, hsw) |
				     FIELD_PREP(DC_TFT_END, m->htotal));
	dpu_write(dpu, DC_TFT_VSYNC, FIELD_PREP(DC_TFT_START, vsw) |
				     FIELD_PREP(DC_TFT_END, m->vtotal));
	dpu_write(dpu, DC_TFT_HDE, FIELD_PREP(DC_TFT_START, hsw + hbp) |
				   FIELD_PREP(DC_TFT_END, hsw + hbp + m->hdisplay));
	dpu_write(dpu, DC_TFT_VDE, FIELD_PREP(DC_TFT_START, vsw + vbp) |
				   FIELD_PREP(DC_TFT_END, vsw + vbp + m->vdisplay));
}

static enum drm_mode_status
x2000_dpu_mode_valid(struct drm_simple_display_pipe *pipe,
		     const struct drm_display_mode *mode)
{
	if (mode->htotal > 4095 || mode->vtotal > 4095)
		return MODE_BAD;

	return MODE_OK;
}

static void x2000_dpu_enable(struct drm_simple_display_pipe *pipe,
			     struct drm_crtc_state *crtc_state,
			     struct drm_plane_state *plane_state)
{
	struct x2000_dpu *dpu = pipe_to_dpu(pipe);
	const struct drm_display_mode *m = &crtc_state->adjusted_mode;
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_gem_dma_object *gem;
	u32 disp_com, tft_cfg, com_cfg, mode, dither;
	int ret;

	ret = clk_set_rate(dpu->clk, m->clock * 1000);
	if (ret)
		drm_warn(&dpu->drm, "pixel clock %d kHz: %d\n", m->clock, ret);
	ret = clk_prepare_enable(dpu->clk);
	if (ret) {
		drm_err(&dpu->drm, "clock: %d\n", ret);
		return;
	}

	x2000_dpu_quick_stop(dpu);

	switch (x2000_dpu_bus_format(pipe)) {
	case MEDIA_BUS_FMT_RGB565_1X16:
		mode = DC_TFT_MODE_565;
		dither = DC_DITHER_TO_565;
		break;
	case MEDIA_BUS_FMT_RGB666_1X18:
		mode = DC_TFT_MODE_666;
		dither = DC_DITHER_TO_666;
		break;
	default:
		mode = DC_TFT_MODE_888;
		dither = 0;
		break;
	}

	disp_com = dpu_read(dpu, DC_DISP_COM);
	disp_com &= ~(DC_DP_IF_SEL | DC_DP_DITHER_EN | DC_DP_DITHER_DW);
	disp_com |= FIELD_PREP(DC_DP_IF_SEL, DC_DP_IF_TFT);
	if (dither && fb->format->format == DRM_FORMAT_XRGB8888)
		disp_com |= DC_DP_DITHER_EN | FIELD_PREP(DC_DP_DITHER_DW, dither);
	dpu_write(dpu, DC_DISP_COM, disp_com);

	dpu_write(dpu, DC_PCFG_RD_CTRL,
		  DC_COMP_ARQOS_CTRL | FIELD_PREP(DC_COMP_ARQOS_VAL, 3));

	com_cfg = dpu_read(dpu, DC_COM_CONFIG);
	com_cfg &= ~(DC_OUT_SEL | DC_BURST_LEN_WDMA | DC_BURST_LEN_RDMA |
		     DC_BURST_LEN_BDMA);
	com_cfg |= FIELD_PREP(DC_OUT_SEL, DC_OUT_SEL_RDMA) |
		   FIELD_PREP(DC_BURST_LEN_WDMA, DC_BURST_16) |
		   FIELD_PREP(DC_BURST_LEN_RDMA, DC_BURST_16) |
		   FIELD_PREP(DC_BURST_LEN_BDMA, DC_BURST_16);
	dpu_write(dpu, DC_COM_CONFIG, com_cfg);

	x2000_dpu_set_timing(dpu, m);

	tft_cfg = dpu_read(dpu, DC_TFT_CFG);
	tft_cfg &= ~(DC_DE_DL | DC_SYNC_DL | DC_VSYNC_DL | DC_COLOR_EVEN |
		     DC_COLOR_ODD | DC_TFT_MODE);
	if (m->flags & DRM_MODE_FLAG_NHSYNC)
		tft_cfg |= DC_SYNC_DL;
	if (m->flags & DRM_MODE_FLAG_NVSYNC)
		tft_cfg |= DC_VSYNC_DL;
	tft_cfg |= FIELD_PREP(DC_TFT_MODE, mode);
	dpu_write(dpu, DC_TFT_CFG, tft_cfg);

	gem = drm_fb_dma_get_gem_obj(fb, 0);
	dpu->desc->next = dpu->desc_phys;
	dpu->desc->buffer = gem->dma_addr + fb->offsets[0];
	dpu->desc->stride = fb->pitches[0] / fb->format->cpp[0];
	dpu->desc->chain_cfg = x2000_dpu_rdma_format(fb->format->format) |
			       FIELD_PREP(RDMA_COLOR, RDMA_COLOR_RGB);
	dpu->desc->irq_ctrl = RDMA_IRQ_SOS;
	wmb();

	dpu_write(dpu, DC_CLR_ST, DC_CLR_ALL);
	dpu_write(dpu, DC_INTC, DC_TFT_UNDR);
	dpu_write(dpu, DC_RDMA_CHAIN_ADDR, dpu->desc_phys);
	dpu_write(dpu, DC_RDMA_CHAIN_CTRL, DC_RDMA_START);
	dpu->running = true;

	drm_crtc_vblank_on(&pipe->crtc);
}

static void x2000_dpu_disable(struct drm_simple_display_pipe *pipe)
{
	struct x2000_dpu *dpu = pipe_to_dpu(pipe);
	u32 st;

	drm_crtc_vblank_off(&pipe->crtc);

	dpu_write(dpu, DC_INTC, 0);
	dpu_write(dpu, DC_CTRL, DC_GEN_STP_RDMA);
	if (readl_poll_timeout(dpu->base + DC_ST, st, !(st & DC_SRD_WORKING),
			       100, 100000))
		x2000_dpu_quick_stop(dpu);
	dpu_write(dpu, DC_CLR_ST, DC_CLR_ALL);
	dpu->running = false;

	clk_disable_unprepare(dpu->clk);
}

static void x2000_dpu_update(struct drm_simple_display_pipe *pipe,
			     struct drm_plane_state *old_state)
{
	struct x2000_dpu *dpu = pipe_to_dpu(pipe);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_pending_vblank_event *event;
	struct drm_framebuffer *fb = state->fb;

	if (fb && dpu->running) {
		struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(fb, 0);

		/* Taken by the controller when it reads the descriptor for the next frame. */
		WRITE_ONCE(dpu->desc->buffer, gem->dma_addr + fb->offsets[0]);
		WRITE_ONCE(dpu->desc->stride, fb->pitches[0] / fb->format->cpp[0]);
		WRITE_ONCE(dpu->desc->chain_cfg,
			   x2000_dpu_rdma_format(fb->format->format) |
			   FIELD_PREP(RDMA_COLOR, RDMA_COLOR_RGB));
		wmb();
	}

	spin_lock_irq(&crtc->dev->event_lock);
	event = crtc->state->event;
	if (event) {
		crtc->state->event = NULL;
		if (crtc->state->active && drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static int x2000_dpu_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct x2000_dpu *dpu = pipe_to_dpu(pipe);

	dpu_write(dpu, DC_CLR_ST, DC_SRD_START);
	dpu_write(dpu, DC_INTC, dpu_read(dpu, DC_INTC) | DC_SRD_START);

	return 0;
}

static void x2000_dpu_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct x2000_dpu *dpu = pipe_to_dpu(pipe);

	dpu_write(dpu, DC_INTC, dpu_read(dpu, DC_INTC) & ~DC_SRD_START);
}

static const struct drm_simple_display_pipe_funcs x2000_dpu_pipe_funcs = {
	.mode_valid = x2000_dpu_mode_valid,
	.enable = x2000_dpu_enable,
	.disable = x2000_dpu_disable,
	.update = x2000_dpu_update,
	.enable_vblank = x2000_dpu_enable_vblank,
	.disable_vblank = x2000_dpu_disable_vblank,
};

static irqreturn_t x2000_dpu_irq(int irq, void *arg)
{
	struct x2000_dpu *dpu = arg;
	u32 flag = dpu_read(dpu, DC_INT_FLAG);

	if (!flag)
		return IRQ_NONE;

	dpu_write(dpu, DC_CLR_ST, flag);

	if (flag & DC_SRD_START)
		drm_crtc_handle_vblank(&dpu->pipe.crtc);

	if (flag & DC_TFT_UNDR)
		drm_err_ratelimited(&dpu->drm, "TFT under-run\n");

	return IRQ_HANDLED;
}

static const struct drm_mode_config_funcs x2000_dpu_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(x2000_dpu_fops);

static const struct drm_driver x2000_dpu_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = "ingenic-x2000-dpu",
	.desc = "Ingenic X2000 display controller",
	.major = 1,
	.minor = 0,
	.fops = &x2000_dpu_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
};

static int x2000_dpu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	struct x2000_dpu *dpu;
	struct drm_device *drm;
	int irq, ret;

	dpu = devm_drm_dev_alloc(dev, &x2000_dpu_driver, struct x2000_dpu, drm);
	if (IS_ERR(dpu))
		return PTR_ERR(dpu);
	drm = &dpu->drm;

	dpu->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dpu->base))
		return PTR_ERR(dpu->base);

	dpu->clk = devm_clk_get(dev, "lcd");
	if (IS_ERR(dpu->clk))
		return dev_err_probe(dev, PTR_ERR(dpu->clk), "no lcd clock\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	dpu->desc = dmam_alloc_coherent(dev, sizeof(*dpu->desc),
					&dpu->desc_phys, GFP_KERNEL);
	if (!dpu->desc)
		return -ENOMEM;

	ret = drm_of_find_panel_or_bridge(dev->of_node, 0, 0, &panel, &bridge);
	if (ret)
		return dev_err_probe(dev, ret, "no panel or bridge\n");
	if (panel) {
		bridge = devm_drm_panel_bridge_add_typed(dev, panel,
							 DRM_MODE_CONNECTOR_DPI);
		if (IS_ERR(bridge))
			return PTR_ERR(bridge);
	}

	/*
	 * Whatever the RTOS left running reads memory Linux now owns: stop it
	 * before anything is allocated. The clock is on while the RTOS's
	 * picture is shown, and is turned on here in case it is not.
	 */
	ret = clk_prepare_enable(dpu->clk);
	if (ret)
		return ret;
	dpu_write(dpu, DC_INTC, 0);
	x2000_dpu_quick_stop(dpu);
	clk_disable_unprepare(dpu->clk);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_width = 2048;
	drm->mode_config.max_height = 2048;
	drm->mode_config.funcs = &x2000_dpu_mode_config_funcs;

	ret = drm_simple_display_pipe_init(drm, &dpu->pipe, &x2000_dpu_pipe_funcs,
					   x2000_dpu_formats,
					   ARRAY_SIZE(x2000_dpu_formats),
					   NULL, NULL);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_attach_bridge(&dpu->pipe, bridge);
	if (ret)
		return ret;

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, irq, x2000_dpu_irq, 0, dev_name(dev), dpu);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	drm_kms_helper_poll_init(drm);

	platform_set_drvdata(pdev, drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_client_setup(drm, NULL);

	return 0;
}

static void x2000_dpu_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	drm_dev_unregister(drm);
	drm_kms_helper_poll_fini(drm);
	drm_atomic_helper_shutdown(drm);
}

static void x2000_dpu_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static const struct of_device_id x2000_dpu_of_match[] = {
	{ .compatible = "ingenic,x2000-dpu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, x2000_dpu_of_match);

static struct platform_driver x2000_dpu_platform_driver = {
	.probe = x2000_dpu_probe,
	.remove = x2000_dpu_remove,
	.shutdown = x2000_dpu_shutdown,
	.driver = {
		.name = "ingenic-x2000-dpu",
		.of_match_table = x2000_dpu_of_match,
	},
};
module_platform_driver(x2000_dpu_platform_driver);

MODULE_DESCRIPTION("DRM driver for the Ingenic X2000 display controller");
MODULE_LICENSE("GPL");
