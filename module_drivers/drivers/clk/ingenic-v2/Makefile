# SPDX-License-Identifier: GPL-2.0
ccflags-$(CONFIG_CLK_X2000) += -DCLK_PLL_V1

obj-$(CONFIG_COMMON_CLK_INGENIC) += clk.o clk-div.o clk-bus.o power-gate.o

obj-$(CONFIG_CLK_X2000)		+= clk-pll-v1.o
obj-$(CONFIG_CLK_X2000)		+= clk-x2000.o
