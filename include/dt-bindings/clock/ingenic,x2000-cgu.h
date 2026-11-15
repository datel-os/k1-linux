/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides clock numbers for the ingenic,x2000-cgu DT binding.
 *
 * They are roughly ordered as:
 *   - external clocks
 *   - PLLs
 *   - muxes/dividers in the order of their registers
 *   - gates in order of their bit in the CLKGR* registers, then in OPCR
 *   - the power gates of the LCR register
 */

#ifndef __DT_BINDINGS_CLOCK_INGENIC_X2000_CGU_H__
#define __DT_BINDINGS_CLOCK_INGENIC_X2000_CGU_H__

#define X2000_CLK_EXCLK		0
#define X2000_CLK_RTCLK		1
#define X2000_CLK_APLL		2
#define X2000_CLK_MPLL		3
#define X2000_CLK_EPLL		4
#define X2000_CLK_SCLKA		5
#define X2000_CLK_CPUMUX	6
#define X2000_CLK_CPU		7
#define X2000_CLK_L2CACHE	8
#define X2000_CLK_AHB0MUX	9
#define X2000_CLK_AHB0		10
#define X2000_CLK_AHB2PMUX	11
#define X2000_CLK_AHB2		12
#define X2000_CLK_PCLK		13
#define X2000_CLK_DDR		14
#define X2000_CLK_ISP		15
#define X2000_CLK_MACPTP	16
#define X2000_CLK_RSA		17
#define X2000_CLK_MACPHY	18
#define X2000_CLK_MACTXPHY0	19
#define X2000_CLK_SSIPLL	20
#define X2000_CLK_LCD		21
#define X2000_CLK_MSC0		22
#define X2000_CLK_PWM		23
#define X2000_CLK_SFC		24
#define X2000_CLK_CIM		25
#define X2000_CLK_MSC1		26
#define X2000_CLK_MSC2		27
#define X2000_CLK_MACTXPHY1	28
#define X2000_CLK_EXCLK_DIV512	29
#define X2000_CLK_ERCS		30
#define X2000_CLK_NEMC		31
#define X2000_CLK_EFUSE		32
#define X2000_CLK_OTG		33
#define X2000_CLK_SCC		34
#define X2000_CLK_I2C0		35
#define X2000_CLK_I2C1		36
#define X2000_CLK_I2C2		37
#define X2000_CLK_I2C3		38
#define X2000_CLK_AUDIO		39
#define X2000_CLK_HELIX		40
#define X2000_CLK_SADC		41
#define X2000_CLK_UART0		42
#define X2000_CLK_UART1		43
#define X2000_CLK_UART2		44
#define X2000_CLK_DTRNG		45
#define X2000_CLK_TCU		46
#define X2000_CLK_SSI0		47
#define X2000_CLK_OST		48
#define X2000_CLK_PDMA		49
#define X2000_CLK_AES		50
#define X2000_CLK_SSI1		51
#define X2000_CLK_RTC		52
#define X2000_CLK_I2C4		53
#define X2000_CLK_I2C5		54
#define X2000_CLK_ISP0		55
#define X2000_CLK_ISP1		56
#define X2000_CLK_FELIX		57
#define X2000_CLK_HASH		58
#define X2000_CLK_ROT		59
#define X2000_CLK_UART3		60
#define X2000_CLK_UART4		61
#define X2000_CLK_UART5		62
#define X2000_CLK_UART6		63
#define X2000_CLK_UART7		64
#define X2000_CLK_UART8		65
#define X2000_CLK_UART9		66
#define X2000_CLK_GMAC0		67
#define X2000_CLK_GMAC1		68
#define X2000_CLK_INTC		69
#define X2000_CLK_CSI		70
#define X2000_CLK_DSI		71
#define X2000_CLK_ARB		72
#define X2000_CLK_CIM_MCLK	73
#define X2000_CLK_USBPHY	74
#define X2000_CLK_OTGPHY	75
#define X2000_CLK_ISP0_PWR	76
#define X2000_CLK_ISP1_PWR	77
#define X2000_CLK_FELIX_PWR	78
#define X2000_CLK_HELIX_PWR	79

#endif /* __DT_BINDINGS_CLOCK_INGENIC_X2000_CGU_H__ */
