/*
 * drivers/soc/tegra/pmc.c
 *
 * Copyright (c) 2010 Google, Inc
 * Copyright (c) 2012-2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "tegra-pmc: " fmt

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/reboot.h>
#include <linux/reset.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/tegra_prod.h>
#include <linux/tegra-soc.h>
#include <linux/platform/tegra/io-dpd.h>

#include <soc/tegra/common.h>
#include <soc/tegra/fuse.h>
#include <soc/tegra/pmc.h>

#include <asm/system_misc.h>

#define PMC_CNTRL			0x0
#define  PMC_CNTRL_SYSCLK_POLARITY	(1 << 10)  /* sys clk polarity */
#define  PMC_CNTRL_SYSCLK_OE		(1 << 11)  /* system clock enable */
#define  PMC_CNTRL_SIDE_EFFECT_LP0	(1 << 14)  /* LP0 when CPU pwr gated */
#define  PMC_CNTRL_CPU_PWRREQ_POLARITY	(1 << 15)  /* CPU pwr req polarity */
#define  PMC_CNTRL_CPU_PWRREQ_OE	(1 << 16)  /* CPU pwr req enable */
#define  PMC_CNTRL_INTR_POLARITY	(1 << 17)  /* inverts INTR polarity */

#define DPD_SAMPLE			0x020
#define  DPD_SAMPLE_ENABLE		(1 << 0)
#define  DPD_SAMPLE_DISABLE		(0 << 0)

#define PWRGATE_TOGGLE			0x30
#define  PWRGATE_TOGGLE_START		(1 << 8)

#define REMOVE_CLAMPING			0x34

#define PWRGATE_STATUS			0x38

#define PMC_PWR_DET_ENABLE		0x48

#define PMC_SCRATCH0			0x50
#define  PMC_SCRATCH0_MODE_RECOVERY	(1 << 31)
#define  PMC_SCRATCH0_MODE_BOOTLOADER	(1 << 30)
#define  PMC_SCRATCH0_MODE_RCM		(1 << 1)
#define  PMC_SCRATCH0_MODE_MASK		(PMC_SCRATCH0_MODE_RECOVERY | \
					 PMC_SCRATCH0_MODE_BOOTLOADER | \
					 PMC_SCRATCH0_MODE_RCM)

#define PMC_CPUPWRGOOD_TIMER		0xc8
#define PMC_CPUPWROFF_TIMER		0xcc

#define PMC_PWR_DET_VAL			0xe4

#define PMC_SCRATCH41			0x140

#define PMC_SENSOR_CTRL			0x1b0
#define PMC_SENSOR_CTRL_SCRATCH_WRITE	(1 << 2)
#define PMC_SENSOR_CTRL_ENABLE_RST	(1 << 1)

#define IO_DPD_REQ			0x1b8
#define  IO_DPD_REQ_CODE_IDLE		(0 << 30)
#define  IO_DPD_REQ_CODE_OFF		(1 << 30)
#define  IO_DPD_REQ_CODE_ON		(2 << 30)
#define  IO_DPD_REQ_CODE_MASK		(3 << 30)
#define  IO_DPD_ENABLE_LSB		30

#define IO_DPD_STATUS			0x1bc

#define IO_DPD2_REQ			0x1c0
#define  IO_DPD2_ENABLE_LSB		30

#define IO_DPD2_STATUS			0x1c4
#define SEL_DPD_TIM			0x1c8

#define PMC_SCRATCH54			0x258
#define PMC_SCRATCH54_DATA_SHIFT	8
#define PMC_SCRATCH54_ADDR_SHIFT	0

#define PMC_SCRATCH55			0x25c
#define PMC_SCRATCH55_RESET_TEGRA	(1 << 31)
#define PMC_SCRATCH55_CNTRL_ID_SHIFT	27
#define PMC_SCRATCH55_PINMUX_SHIFT	24
#define PMC_SCRATCH55_16BITOP		(1 << 15)
#define PMC_SCRATCH55_CHECKSUM_SHIFT	16
#define PMC_SCRATCH55_I2CSLV1_SHIFT	0

#define GPU_RG_CNTRL			0x2d4

#define PMC_FUSE_CTRL                   0x450
#define PMC_FUSE_CTRL_ENABLE_REDIRECTION	(1 << 0)
#define PMC_FUSE_CTRL_DISABLE_REDIRECTION	(1 << 1)
#define PMC_FUSE_CTRL_PS18_LATCH_SET    (1 << 8)
#define PMC_FUSE_CTRL_PS18_LATCH_CLEAR  (1 << 9)

/* Scratch 250: Bootrom i2c command base */
#define PMC_BR_COMMAND_BASE		0x908

/* USB2 SLEEPWALK registers */
#define UTMIP(_port, _offset1, _offset2) \
		(((_port) <= 2) ? (_offset1) : (_offset2))

#define APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(x)	UTMIP(x, 0x1fc, 0x4d0)
#define   UTMIP_MASTER_ENABLE(x)		UTMIP(x, BIT(8 * (x)), BIT(0))
#define   UTMIP_FSLS_USE_PMC(x)			UTMIP(x, BIT(8 * (x) + 1), \
							BIT(1))
#define   UTMIP_PCTRL_USE_PMC(x)		UTMIP(x, BIT(8 * (x) + 2), \
							BIT(2))
#define   UTMIP_TCTRL_USE_PMC(x)		UTMIP(x, BIT(8 * (x) + 3), \
							BIT(3))
#define   UTMIP_WAKE_VAL(_port, _value)		(((_value) & 0xf) << \
					(UTMIP(_port, 8 * (_port) + 4, 4)))
#define   UTMIP_WAKE_VAL_NONE(_port)		UTMIP_WAKE_VAL(_port, 12)
#define   UTMIP_WAKE_VAL_ANY(_port)		UTMIP_WAKE_VAL(_port, 15)

#define APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG1	(0x4d0)
#define   UTMIP_RPU_SWITC_LOW_USE_PMC_PX(x)	BIT((x) + 8)
#define   UTMIP_RPD_CTRL_USE_PMC_PX(x)		BIT((x) + 16)

#define APBDEV_PMC_UTMIP_MASTER_CONFIG		(0x274)
#define   UTMIP_PWR(x)				UTMIP(x, BIT(x), BIT(4))
#define   UHSIC_PWR(x)				BIT(3)

#define APBDEV_PMC_USB_DEBOUNCE_DEL		(0xec)
#define   DEBOUNCE_VAL(x)			(((x) & 0xffff) << 0)
#define   UTMIP_LINE_DEB_CNT(x)			(((x) & 0xf) << 16)
#define   UHSIC_LINE_DEB_CNT(x)			(((x) & 0xf) << 20)

#define APBDEV_PMC_UTMIP_UHSIC_FAKE(x)		UTMIP(x, 0x218, 0x294)
#define   UTMIP_FAKE_USBOP_VAL(x)		UTMIP(x, BIT(4 * (x)), BIT(8))
#define   UTMIP_FAKE_USBON_VAL(x)		UTMIP(x, BIT(4 * (x) + 1), \
							BIT(9))
#define   UTMIP_FAKE_USBOP_EN(x)		UTMIP(x, BIT(4 * (x) + 2), \
							BIT(10))
#define   UTMIP_FAKE_USBON_EN(x)		UTMIP(x, BIT(4 * (x) + 3), \
							BIT(11))

#define APBDEV_PMC_UTMIP_UHSIC_SLEEPWALK_CFG(x)	UTMIP(x, 0x200, 0x288)
#define   UTMIP_WAKE_WALK_EN(x)			UTMIP(x, BIT(8 * (x) + 6), \
							BIT(14))
#define   UTMIP_LINEVAL_WALK_EN(x)		UTMIP(x, BIT(8 * (x) + 7), \
							BIT(15))

#define APBDEV_PMC_USB_AO			(0xf0)
#define   USBOP_VAL_PD(x)			UTMIP(x, BIT(4 * (x)), BIT(20))
#define   USBON_VAL_PD(x)			UTMIP(x, BIT(4 * (x) + 1), \
							BIT(21))
#define   STROBE_VAL_PD(x)			BIT(12)
#define   DATA0_VAL_PD(x)			BIT(13)
#define   DATA1_VAL_PD				BIT(24)

#define APBDEV_PMC_UTMIP_UHSIC_SAVED_STATE(x)	UTMIP(x, 0x1f0, 0x280)
#define   SPEED(_port, _value)			(((_value) & 0x3) << \
						(UTMIP(_port, 8 * (_port), 8)))
#define   UTMI_HS(_port)			SPEED(_port, 0)
#define   UTMI_FS(_port)			SPEED(_port, 1)
#define   UTMI_LS(_port)			SPEED(_port, 2)
#define   UTMI_RST(_port)			SPEED(_port, 3)

#define APBDEV_PMC_UTMIP_UHSIC_TRIGGERS		(0x1ec)
#define   UTMIP_CLR_WALK_PTR(x)			UTMIP(x, BIT(x), BIT(16))
#define   UTMIP_CAP_CFG(x)			UTMIP(x, BIT((x) + 4), BIT(17))
#define   UTMIP_CLR_WAKE_ALARM(x)		UTMIP(x, BIT((x) + 12), \
							BIT(19))
#define   UHSIC_CLR_WALK_PTR			BIT(3)
#define   UHSIC_CLR_WAKE_ALARM			BIT(15)

#define APBDEV_PMC_UTMIP_SLEEPWALK_PX(x)	UTMIP(x, 0x204 + (4 * (x)), \
							0x4e0)
/* phase A */
#define   UTMIP_USBOP_RPD_A			BIT(0)
#define   UTMIP_USBON_RPD_A			BIT(1)
#define   UTMIP_AP_A				BIT(4)
#define   UTMIP_AN_A				BIT(5)
#define   UTMIP_HIGHZ_A				BIT(6)
/* phase B */
#define   UTMIP_USBOP_RPD_B			BIT(8)
#define   UTMIP_USBON_RPD_B			BIT(9)
#define   UTMIP_AP_B				BIT(12)
#define   UTMIP_AN_B				BIT(13)
#define   UTMIP_HIGHZ_B				BIT(14)
/* phase C */
#define   UTMIP_USBOP_RPD_C			BIT(16)
#define   UTMIP_USBON_RPD_C			BIT(17)
#define   UTMIP_AP_C				BIT(20)
#define   UTMIP_AN_C				BIT(21)
#define   UTMIP_HIGHZ_C				BIT(22)
/* phase D */
#define   UTMIP_USBOP_RPD_D			BIT(24)
#define   UTMIP_USBON_RPD_D			BIT(25)
#define   UTMIP_AP_D				BIT(28)
#define   UTMIP_AN_D				BIT(29)
#define   UTMIP_HIGHZ_D				BIT(30)

#define APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP	(0x26c)
#define   UTMIP_LINE_WAKEUP_EN(x)		UTMIP(x, BIT(x), BIT(4))
#define   UHSIC_LINE_WAKEUP_EN			BIT(3)

#define APBDEV_PMC_UTMIP_TERM_PAD_CFG		(0x1f8)
#define   PCTRL_VAL(x)				(((x) & 0x3f) << 1)
#define   TCTRL_VAL(x)				(((x) & 0x3f) << 7)

#define APBDEV_PMC_UTMIP_PAD_CFGX(x)		(0x4c0 + (4 * (x)))
#define   RPD_CTRL_PX(x)			(((x) & 0x1f) << 22)

#define APBDEV_PMC_UHSIC_SLEEP_CFG		\
		APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(0)
#define   UHSIC_MASTER_ENABLE			BIT(24)
#define   UHSIC_WAKE_VAL(_value)		(((_value) & 0xf) << 28)
#define   UHSIC_WAKE_VAL_SD10			UHSIC_WAKE_VAL(2)
#define   UHSIC_WAKE_VAL_NONE			UHSIC_WAKE_VAL(12)

#define APBDEV_PMC_UHSIC_FAKE			APBDEV_PMC_UTMIP_UHSIC_FAKE(0)
#define   UHSIC_FAKE_STROBE_VAL			BIT(12)
#define   UHSIC_FAKE_DATA_VAL			BIT(13)
#define   UHSIC_FAKE_STROBE_EN			BIT(14)
#define   UHSIC_FAKE_DATA_EN			BIT(15)

#define APBDEV_PMC_UHSIC_SAVED_STATE		\
		APBDEV_PMC_UTMIP_UHSIC_SAVED_STATE(0)
#define   UHSIC_MODE(_value)			(((_value) & 0x1) << 24)
#define   UHSIC_HS				UHSIC_MODE(0)
#define   UHSIC_RST				UHSIC_MODE(1)

#define APBDEV_PMC_UHSIC_SLEEPWALK_CFG		\
		APBDEV_PMC_UTMIP_UHSIC_SLEEPWALK_CFG(0)
#define   UHSIC_WAKE_WALK_EN			BIT(30)
#define   UHSIC_LINEVAL_WALK_EN			BIT(31)

#define APBDEV_PMC_UHSIC_SLEEPWALK_P0		(0x210)
#define   UHSIC_DATA0_RPD_A			BIT(1)
#define   UHSIC_DATA0_RPU_B			BIT(11)
#define   UHSIC_DATA0_RPU_C			BIT(19)
#define   UHSIC_DATA0_RPU_D			BIT(27)
#define   UHSIC_STROBE_RPU_A			BIT(2)
#define   UHSIC_STROBE_RPD_B			BIT(8)
#define   UHSIC_STROBE_RPD_C			BIT(16)
#define   UHSIC_STROBE_RPD_D			BIT(24)

/* io dpd off request code */
#define IO_DPD_CODE_OFF		1

struct io_dpd_reg_info {
	u32 req_reg_off;
	u8 dpd_code_lsb;
};

static struct io_dpd_reg_info t3_io_dpd_req_regs[] = {
	{0x1b8, 30},
	{0x1c0, 30},
};

static DEFINE_SPINLOCK(tegra_io_dpd_lock);
static DEFINE_SPINLOCK(tegra_pmc_access_lock);
static struct tegra_prod *prod_list;

#ifdef CONFIG_PADCTRL_TEGRA210_PMC
extern int tegra210_pmc_padctrl_init(struct device *dev,
				     struct device_node *np);
#endif

#ifdef CONFIG_TEGRA210_BOOTROM_PMC
extern int tegra210_boorom_pmc_init(struct device *dev);
#endif

struct tegra_pmc_soc {
	unsigned int num_powergates;
	const char *const *powergates;
	unsigned int num_cpu_powergates;
	const u8 *cpu_powergates;

	bool has_tsense_reset;
	bool has_gpu_clamps;
	bool has_ps18;
};

/**
 * struct tegra_pmc - NVIDIA Tegra PMC
 * @base: pointer to I/O remapped register region
 * @clk: pointer to pclk clock
 * @rate: currently configured rate of pclk
 * @suspend_mode: lowest suspend mode available
 * @cpu_good_time: CPU power good time (in microseconds)
 * @cpu_off_time: CPU power off time (in microsecends)
 * @core_osc_time: core power good OSC time (in microseconds)
 * @core_pmu_time: core power good PMU time (in microseconds)
 * @core_off_time: core power off time (in microseconds)
 * @corereq_high: core power request is active-high
 * @sysclkreq_high: system clock request is active-high
 * @combined_req: combined power request for CPU & core
 * @cpu_pwr_good_en: CPU power good signal is enabled
 * @lp0_vec_phys: physical base address of the LP0 warm boot code
 * @lp0_vec_size: size of the LP0 warm boot code
 * @powergates_lock: mutex for power gate register access
 */
struct tegra_pmc {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;

	const struct tegra_pmc_soc *soc;

	unsigned long rate;

	enum tegra_suspend_mode suspend_mode;
	u32 cpu_good_time;
	u32 cpu_off_time;
	u32 core_osc_time;
	u32 core_pmu_time;
	u32 core_off_time;
	bool corereq_high;
	bool sysclkreq_high;
	bool combined_req;
	bool cpu_pwr_good_en;
	u32 lp0_vec_phys;
	u32 lp0_vec_size;

	struct mutex powergates_lock;
};

static struct tegra_pmc *pmc = &(struct tegra_pmc) {
	.base = NULL,
	.suspend_mode = TEGRA_SUSPEND_NONE,
};

static u32 tegra_pmc_readl(unsigned long offset)
{
	return readl(pmc->base + offset);
}

static void tegra_pmc_writel(u32 value, unsigned long offset)
{
	writel(value, pmc->base + offset);
}

static void tegra_pmc_register_update(int offset,
		unsigned long mask, unsigned long val)
{
	u32 pmc_reg;

	pmc_reg = tegra_pmc_readl(offset);
	pmc_reg = (pmc_reg & ~mask) | (val & mask);
	tegra_pmc_writel(pmc_reg, offset);
}

int tegra_read_wake_status(u32 *wake_status)
{
	// TODO: need to check if tegra-wakeups.c is still needed by t210
	return 0;
}

#ifndef CONFIG_TEGRA_POWERGATE
/**
 * tegra_powergate_set() - set the state of a partition
 * @id: partition ID
 * @new_state: new state of the partition
 */
static int tegra_powergate_set(int id, bool new_state)
{
	bool status;

	mutex_lock(&pmc->powergates_lock);

	status = tegra_pmc_readl(PWRGATE_STATUS) & (1 << id);

	if (status == new_state) {
		mutex_unlock(&pmc->powergates_lock);
		return 0;
	}

	tegra_pmc_writel(PWRGATE_TOGGLE_START | id, PWRGATE_TOGGLE);

	mutex_unlock(&pmc->powergates_lock);

	return 0;
}

/**
 * tegra_powergate_power_on() - power on partition
 * @id: partition ID
 */
int tegra_powergate_power_on(int id)
{
	if (!pmc->soc || id < 0 || id >= pmc->soc->num_powergates)
		return -EINVAL;

	return tegra_powergate_set(id, true);
}

/**
 * tegra_powergate_power_off() - power off partition
 * @id: partition ID
 */
int tegra_powergate_power_off(int id)
{
	if (!pmc->soc || id < 0 || id >= pmc->soc->num_powergates)
		return -EINVAL;

	return tegra_powergate_set(id, false);
}
EXPORT_SYMBOL(tegra_powergate_power_off);

/**
 * tegra_powergate_is_powered() - check if partition is powered
 * @id: partition ID
 */
int tegra_powergate_is_powered(int id)
{
	u32 status;

	if (!pmc->soc || id < 0 || id >= pmc->soc->num_powergates)
		return -EINVAL;

	status = tegra_pmc_readl(PWRGATE_STATUS) & (1 << id);
	return !!status;
}

/**
 * tegra_powergate_remove_clamping() - remove power clamps for partition
 * @id: partition ID
 */
int tegra_powergate_remove_clamping(int id)
{
	u32 mask;

	if (!pmc->soc || id < 0 || id >= pmc->soc->num_powergates)
		return -EINVAL;

	/*
	 * On Tegra124 and later, the clamps for the GPU are controlled by a
	 * separate register (with different semantics).
	 */
	if (id == TEGRA_POWERGATE_3D) {
		if (pmc->soc->has_gpu_clamps) {
			tegra_pmc_writel(0, GPU_RG_CNTRL);
			return 0;
		}
	}

	/*
	 * Tegra 2 has a bug where PCIE and VDE clamping masks are
	 * swapped relatively to the partition ids
	 */
	if (id == TEGRA_POWERGATE_VDEC)
		mask = (1 << TEGRA_POWERGATE_PCIE);
	else if (id == TEGRA_POWERGATE_PCIE)
		mask = (1 << TEGRA_POWERGATE_VDEC);
	else
		mask = (1 << id);

	tegra_pmc_writel(mask, REMOVE_CLAMPING);

	return 0;
}
EXPORT_SYMBOL(tegra_powergate_remove_clamping);

/**
 * tegra_powergate_sequence_power_up() - power up partition
 * @id: partition ID
 * @clk: clock for partition
 * @rst: reset for partition
 *
 * Must be called with clk disabled, and returns with clk enabled.
 */
int tegra_powergate_sequence_power_up(int id, struct clk *clk,
				      struct reset_control *rst)
{
	int ret;

	reset_control_assert(rst);

	ret = tegra_powergate_power_on(id);
	if (ret)
		goto err_power;

	ret = clk_prepare_enable(clk);
	if (ret)
		goto err_clk;

	usleep_range(10, 20);

	ret = tegra_powergate_remove_clamping(id);
	if (ret)
		goto err_clamp;

	usleep_range(10, 20);
	reset_control_deassert(rst);

	return 0;

err_clamp:
	clk_disable_unprepare(clk);
err_clk:
	tegra_powergate_power_off(id);
err_power:
	return ret;
}
EXPORT_SYMBOL(tegra_powergate_sequence_power_up);

#ifdef CONFIG_SMP
/**
 * tegra_get_cpu_powergate_id() - convert from CPU ID to partition ID
 * @cpuid: CPU partition ID
 *
 * Returns the partition ID corresponding to the CPU partition ID or a
 * negative error code on failure.
 */
static int tegra_get_cpu_powergate_id(int cpuid)
{
	if (pmc->soc && cpuid > 0 && cpuid < pmc->soc->num_cpu_powergates)
		return pmc->soc->cpu_powergates[cpuid];

	return -EINVAL;
}

/**
 * tegra_pmc_cpu_is_powered() - check if CPU partition is powered
 * @cpuid: CPU partition ID
 */
bool tegra_pmc_cpu_is_powered(int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return false;

	return tegra_powergate_is_powered(id);
}

/**
 * tegra_pmc_cpu_power_on() - power on CPU partition
 * @cpuid: CPU partition ID
 */
int tegra_pmc_cpu_power_on(int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return id;

	return tegra_powergate_set(id, true);
}

/**
 * tegra_pmc_cpu_remove_clamping() - remove power clamps for CPU partition
 * @cpuid: CPU partition ID
 */
int tegra_pmc_cpu_remove_clamping(int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return id;

	return tegra_powergate_remove_clamping(id);
}
#endif /* CONFIG_SMP */
#endif /* CONFIG_TEGRA_POWERGATE */

static void tegra_pmc_program_reboot_reason(const char *cmd)
{
	u32 value;

	value = tegra_pmc_readl(PMC_SCRATCH0);
	value &= ~PMC_SCRATCH0_MODE_MASK;

	if (cmd) {
		if (strcmp(cmd, "recovery") == 0)
			value |= PMC_SCRATCH0_MODE_RECOVERY;

		if (strcmp(cmd, "bootloader") == 0)
			value |= PMC_SCRATCH0_MODE_BOOTLOADER;

		if (strcmp(cmd, "forced-recovery") == 0)
			value |= PMC_SCRATCH0_MODE_RCM;
	}

	tegra_pmc_writel(value, PMC_SCRATCH0);
}

static int tegra_pmc_restart_notify(struct notifier_block *this,
				    unsigned long action, void *data)
{
	const char *cmd = data;
	u32 value;

	tegra_pmc_program_reboot_reason(cmd);

	value = tegra_pmc_readl(0);
	value |= 0x10;
	tegra_pmc_writel(value, 0);

	return NOTIFY_DONE;
}

static struct notifier_block tegra_pmc_restart_handler = {
	.notifier_call = tegra_pmc_restart_notify,
	.priority = 128,
};

#ifndef CONFIG_TEGRA_POWERGATE
static int powergate_show(struct seq_file *s, void *data)
{
	unsigned int i;

	seq_printf(s, " powergate powered\n");
	seq_printf(s, "------------------\n");

	for (i = 0; i < pmc->soc->num_powergates; i++) {
		if (!pmc->soc->powergates[i])
			continue;

		seq_printf(s, " %9s %7s\n", pmc->soc->powergates[i],
			   tegra_powergate_is_powered(i) ? "yes" : "no");
	}

	return 0;
}

static int powergate_open(struct inode *inode, struct file *file)
{
	return single_open(file, powergate_show, inode->i_private);
}

static const struct file_operations powergate_fops = {
	.open = powergate_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tegra_powergate_debugfs_init(void)
{
	struct dentry *d;

	d = debugfs_create_file("powergate", S_IRUGO, NULL, NULL,
				&powergate_fops);
	if (!d)
		return -ENOMEM;

	return 0;
}
#else
static int tegra_powergate_debugfs_init(void)
{
	return 0;
}
#endif

#ifndef CONFIG_TEGRA_POWERGATE
static int tegra_io_rail_prepare(int id, unsigned long *request,
				 unsigned long *status, unsigned int *bit)
{
	unsigned long rate, value;

	*bit = id % 32;

	/*
	 * There are two sets of 30 bits to select IO rails, but bits 30 and
	 * 31 are control bits rather than IO rail selection bits.
	 */
	if (id > 63 || *bit == 30 || *bit == 31)
		return -EINVAL;

	if (id < 32) {
		*status = IO_DPD_STATUS;
		*request = IO_DPD_REQ;
	} else {
		*status = IO_DPD2_STATUS;
		*request = IO_DPD2_REQ;
	}

	rate = clk_get_rate(pmc->clk);

	tegra_pmc_writel(DPD_SAMPLE_ENABLE, DPD_SAMPLE);

	/* must be at least 200 ns, in APB (PCLK) clock cycles */
	value = DIV_ROUND_UP(1000000000, rate);
	value = DIV_ROUND_UP(200, value);
	tegra_pmc_writel(value, SEL_DPD_TIM);

	return 0;
}

static int tegra_io_rail_poll(unsigned long offset, unsigned long mask,
			      unsigned long val, unsigned long timeout)
{
	unsigned long value;

	timeout = jiffies + msecs_to_jiffies(timeout);

	while (time_after(timeout, jiffies)) {
		value = tegra_pmc_readl(offset);
		if ((value & mask) == val)
			return 0;

		usleep_range(250, 1000);
	}

	return -ETIMEDOUT;
}

static void tegra_io_rail_unprepare(void)
{
	tegra_pmc_writel(DPD_SAMPLE_DISABLE, DPD_SAMPLE);
}

int tegra_io_rail_power_on(int id)
{
	unsigned long request, status, value;
	unsigned int bit, mask;
	int err;

	err = tegra_io_rail_prepare(id, &request, &status, &bit);
	if (err < 0)
		return err;

	mask = 1 << bit;

	value = tegra_pmc_readl(request);
	value |= mask;
	value &= ~IO_DPD_REQ_CODE_MASK;
	value |= IO_DPD_REQ_CODE_OFF;
	tegra_pmc_writel(value, request);

	err = tegra_io_rail_poll(status, mask, 0, 250);
	if (err < 0) {
		pr_info("tegra_io_rail_poll() failed: %d\n", err);
		return err;
	}

	tegra_io_rail_unprepare();

	return 0;
}
EXPORT_SYMBOL(tegra_io_rail_power_on);

int tegra_io_rail_power_off(int id)
{
	unsigned long request, status, value;
	unsigned int bit, mask;
	int err;

	err = tegra_io_rail_prepare(id, &request, &status, &bit);
	if (err < 0) {
		pr_info("tegra_io_rail_prepare() failed: %d\n", err);
		return err;
	}

	mask = 1 << bit;

	value = tegra_pmc_readl(request);
	value |= mask;
	value &= ~IO_DPD_REQ_CODE_MASK;
	value |= IO_DPD_REQ_CODE_ON;
	tegra_pmc_writel(value, request);

	err = tegra_io_rail_poll(status, mask, mask, 250);
	if (err < 0)
		return err;

	tegra_io_rail_unprepare();

	return 0;
}
EXPORT_SYMBOL(tegra_io_rail_power_off);
#endif /* CONFIG_TEGRA_POWERGATE */

void tegra_pmc_write_bootrom_command(u32 command_offset, unsigned long val)
{
	tegra_pmc_writel(val, command_offset + PMC_BR_COMMAND_BASE);
}
EXPORT_SYMBOL(tegra_pmc_write_bootrom_command);

void tegra_pmc_reset_system(void)
{
	u32 val;

	val = tegra_pmc_readl(PMC_CNTRL);
	val |= 0x10;
	tegra_pmc_writel(val, PMC_CNTRL);
}
EXPORT_SYMBOL(tegra_pmc_reset_system);

void tegra_pmc_iopower_enable(int reg, u32 bit_mask)
{
	tegra_pmc_register_update(reg, bit_mask, 0);
}
EXPORT_SYMBOL(tegra_pmc_iopower_enable);

void  tegra_pmc_iopower_disable(int reg, u32 bit_mask)
{
	tegra_pmc_register_update(reg, bit_mask, bit_mask);
}
EXPORT_SYMBOL(tegra_pmc_iopower_disable);

int tegra_pmc_iopower_get_status(int reg, u32 bit_mask)
{
	unsigned int no_iopower;

	no_iopower = tegra_pmc_readl(reg);
	if (no_iopower & bit_mask)
		return 0;
	else
		return 1;
}
EXPORT_SYMBOL(tegra_pmc_iopower_get_status);

void tegra_io_dpd_enable(struct tegra_io_dpd *hnd)
{
	unsigned int enable_mask;
	unsigned int dpd_status;
	unsigned int dpd_enable_lsb;

	if (!hnd)
		return;

	spin_lock(&tegra_io_dpd_lock);
	dpd_enable_lsb = (hnd->io_dpd_reg_index) ? IO_DPD2_ENABLE_LSB :
						IO_DPD_ENABLE_LSB;
	tegra_pmc_writel(0x1, DPD_SAMPLE);
	tegra_pmc_writel(0x10, SEL_DPD_TIM);
	enable_mask = ((1 << hnd->io_dpd_bit) | (2 << dpd_enable_lsb));
	tegra_pmc_writel(enable_mask, IO_DPD_REQ + hnd->io_dpd_reg_index * 8);
	/* delay pclk * (reset SEL_DPD_TIM value 127 + 5) */
	udelay(7);
	dpd_status = tegra_pmc_readl(IO_DPD_STATUS + hnd->io_dpd_reg_index * 8);
	if (!(dpd_status & (1 << hnd->io_dpd_bit))) {
		if (!tegra_platform_is_fpga()) {
			pr_info("Error: dpd%d enable failed, status=%#x\n",
			(hnd->io_dpd_reg_index + 1), dpd_status);
		}
	}
	/* Sample register must be reset before next sample operation */
	tegra_pmc_writel(0x0, DPD_SAMPLE);
	spin_unlock(&tegra_io_dpd_lock);
}
EXPORT_SYMBOL(tegra_io_dpd_enable);

int tegra_pmc_io_dpd_enable(int reg, int bit_pos)
{
        struct tegra_io_dpd io_dpd;

        io_dpd.io_dpd_bit = bit_pos;
        io_dpd.io_dpd_reg_index = reg;
        tegra_io_dpd_enable(&io_dpd);
        return 0;
}
EXPORT_SYMBOL(tegra_pmc_io_dpd_enable);

void tegra_io_dpd_disable(struct tegra_io_dpd *hnd)
{
	unsigned int enable_mask;
	unsigned int dpd_status;
	unsigned int dpd_enable_lsb;

	if (!hnd)
		return;

	spin_lock(&tegra_io_dpd_lock);
	dpd_enable_lsb = (hnd->io_dpd_reg_index) ? IO_DPD2_ENABLE_LSB :
						IO_DPD_ENABLE_LSB;
	enable_mask = ((1 << hnd->io_dpd_bit) | (1 << dpd_enable_lsb));
	tegra_pmc_writel(enable_mask, IO_DPD_REQ + hnd->io_dpd_reg_index * 8);
	dpd_status = tegra_pmc_readl(IO_DPD_STATUS + hnd->io_dpd_reg_index * 8);
	if (dpd_status & (1 << hnd->io_dpd_bit)) {
		if (!tegra_platform_is_fpga()) {
			pr_info("Error: dpd%d disable failed, status=%#x\n",
			(hnd->io_dpd_reg_index + 1), dpd_status);
		}
	}
	spin_unlock(&tegra_io_dpd_lock);
}
EXPORT_SYMBOL(tegra_io_dpd_disable);

int tegra_pmc_io_dpd_disable(int reg, int bit_pos)
{
        struct tegra_io_dpd io_dpd;

        io_dpd.io_dpd_bit = bit_pos;
        io_dpd.io_dpd_reg_index = reg;
        tegra_io_dpd_disable(&io_dpd);
        return 0;
}
EXPORT_SYMBOL(tegra_pmc_io_dpd_disable);

int tegra_pmc_io_dpd_get_status(int reg, int bit_pos)
{
	unsigned int dpd_status;

	dpd_status = tegra_pmc_readl(IO_DPD_STATUS + reg * 8);
	if (dpd_status & BIT(bit_pos))
		return 1;
	else
		return 0;
}
EXPORT_SYMBOL(tegra_pmc_io_dpd_get_status);

/* cleans io dpd settings from bootloader during kernel init */
void tegra_bl_io_dpd_cleanup(void)
{
	int i;
	unsigned int dpd_mask;
	unsigned int dpd_status;

	pr_info("Clear bootloader IO dpd settings\n");
	/* clear all dpd requests from bootloader */
	for (i = 0; i < ARRAY_SIZE(t3_io_dpd_req_regs); i++) {
		dpd_mask = ((1 << t3_io_dpd_req_regs[i].dpd_code_lsb) - 1);
		dpd_mask |= (IO_DPD_CODE_OFF <<
			t3_io_dpd_req_regs[i].dpd_code_lsb);
		tegra_pmc_writel(dpd_mask, t3_io_dpd_req_regs[i].req_reg_off);
		/* dpd status register is next to req reg in tegra3 */
		dpd_status =
			tegra_pmc_readl(t3_io_dpd_req_regs[i].req_reg_off + 4);
	}
	return;
}
EXPORT_SYMBOL(tegra_bl_io_dpd_cleanup);

void tegra_pmc_io_dpd_clear(void)
{
	tegra_bl_io_dpd_cleanup();
}
EXPORT_SYMBOL(tegra_pmc_io_dpd_clear);

void tegra_pmc_pwr_detect_update(unsigned long mask, unsigned long val)
{
	unsigned long flags;

	spin_lock_irqsave(&tegra_pmc_access_lock, flags);
	tegra_pmc_register_update(PMC_PWR_DET_ENABLE, mask, mask);
	tegra_pmc_register_update(PMC_PWR_DET_VAL, mask, val);
	spin_unlock_irqrestore(&tegra_pmc_access_lock, flags);
}
EXPORT_SYMBOL(tegra_pmc_pwr_detect_update);

unsigned long tegra_pmc_pwr_detect_get(unsigned long mask)
{
	return tegra_pmc_readl(PMC_PWR_DET_VAL);
}
EXPORT_SYMBOL(tegra_pmc_pwr_detect_get);

/* T210 USB2 SLEEPWALK APIs */
int tegra_pmc_utmi_phy_enable_sleepwalk(int port, enum usb_device_speed speed,
					struct tegra_utmi_pad_config *config)
{
	u32 reg;

	pr_info("PMC %s : port %d, speed %d\n", __func__, port, speed);

	/* ensure sleepwalk logic is disabled */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));
	reg &= ~UTMIP_MASTER_ENABLE(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));

	/* ensure sleepwalk logics are in low power mode */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_MASTER_CONFIG);
	reg |= UTMIP_PWR(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_MASTER_CONFIG);

	/* set debounce time */
	reg = tegra_pmc_readl(APBDEV_PMC_USB_DEBOUNCE_DEL);
	reg &= ~UTMIP_LINE_DEB_CNT(~0);
	reg |= UTMIP_LINE_DEB_CNT(0x1);
	tegra_pmc_writel(reg, APBDEV_PMC_USB_DEBOUNCE_DEL);

	/* ensure fake events of sleepwalk logic are desiabled */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_FAKE(port));
	reg &= ~(UTMIP_FAKE_USBOP_VAL(port) | UTMIP_FAKE_USBON_VAL(port) |
			UTMIP_FAKE_USBOP_EN(port) | UTMIP_FAKE_USBON_EN(port));
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_FAKE(port));

	/* ensure wake events of sleepwalk logic are not latched */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);
	reg &= ~UTMIP_LINE_WAKEUP_EN(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);

	/* disable wake event triggers of sleepwalk logic */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));
	reg &= ~UTMIP_WAKE_VAL(port, ~0);
	reg |= UTMIP_WAKE_VAL_NONE(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));

	/* power down the line state detectors of the pad */
	reg = tegra_pmc_readl(APBDEV_PMC_USB_AO);
	reg |= (USBOP_VAL_PD(port) | USBON_VAL_PD(port));
	tegra_pmc_writel(reg, APBDEV_PMC_USB_AO);

	/* save state per speed */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SAVED_STATE(port));
	reg &= ~SPEED(port, ~0);
	if (speed == USB_SPEED_HIGH)
		reg |= UTMI_HS(port);
	else if (speed == USB_SPEED_FULL)
		reg |= UTMI_FS(port);
	else if (speed == USB_SPEED_LOW)
		reg |= UTMI_LS(port);
	else
		reg |= UTMI_RST(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SAVED_STATE(port));

	/* enable the trigger of the sleepwalk logic */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEPWALK_CFG(port));
	reg |= (UTMIP_WAKE_WALK_EN(port) | UTMIP_LINEVAL_WALK_EN(port));
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEPWALK_CFG(port));

	/* reset the walk pointer and clear the alarm of the sleepwalk logic,
	 * as well as capture the configuration of the USB2.0 pad
	 */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_TRIGGERS);
	reg |= (UTMIP_CLR_WALK_PTR(port) | UTMIP_CLR_WAKE_ALARM(port) |
		UTMIP_CAP_CFG(port));
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_TRIGGERS);

	/* program electrical parameters read from XUSB PADCTL */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_TERM_PAD_CFG);
	reg &= ~(TCTRL_VAL(~0) | PCTRL_VAL(~0));
	reg |= (TCTRL_VAL(config->tctrl) | PCTRL_VAL(config->pctrl));
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_TERM_PAD_CFG);

	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_PAD_CFGX(port));
	reg &= ~RPD_CTRL_PX(~0);
	reg |= RPD_CTRL_PX(config->rpd_ctrl);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_PAD_CFGX(port));

	/* setup the pull-ups and pull-downs of the signals during the four
	 * stages of sleepwalk.
	 * if device is connected, program sleepwalk logic to maintain a J and
	 * keep driving K upon seeing remote wake.
	 */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_SLEEPWALK_PX(port));
	reg = (UTMIP_USBOP_RPD_A | UTMIP_USBOP_RPD_B | UTMIP_USBOP_RPD_C |
		UTMIP_USBOP_RPD_D);
	reg |= (UTMIP_USBON_RPD_A | UTMIP_USBON_RPD_B | UTMIP_USBON_RPD_C |
		UTMIP_USBON_RPD_D);
	if (speed == USB_SPEED_UNKNOWN) {
		reg |= (UTMIP_HIGHZ_A | UTMIP_HIGHZ_B | UTMIP_HIGHZ_C |
			UTMIP_HIGHZ_D);
	} else if ((speed == USB_SPEED_HIGH) || (speed == USB_SPEED_FULL)) {
		/* J state: D+/D- = high/low, K state: D+/D- = low/high */
		reg |= UTMIP_HIGHZ_A;
		reg |= UTMIP_AP_A;
		reg |= (UTMIP_AN_B | UTMIP_AN_C | UTMIP_AN_D);
	} else if (speed == USB_SPEED_LOW) {
		/* J state: D+/D- = low/high, K state: D+/D- = high/low */
		reg |= UTMIP_HIGHZ_A;
		reg |= UTMIP_AN_A;
		reg |= (UTMIP_AP_B | UTMIP_AP_C | UTMIP_AP_D);
	}
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_SLEEPWALK_PX(port));

	/* power up the line state detectors of the pad */
	reg = tegra_pmc_readl(APBDEV_PMC_USB_AO);
	reg &= ~(USBOP_VAL_PD(port) | USBON_VAL_PD(port));
	tegra_pmc_writel(reg, APBDEV_PMC_USB_AO);

	usleep_range(50, 100);

	/* switch the electric control of the USB2.0 pad to PMC */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));
	reg |= (UTMIP_FSLS_USE_PMC(port) | UTMIP_PCTRL_USE_PMC(port) |
			UTMIP_TCTRL_USE_PMC(port));
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));

	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG1);
	reg |= (UTMIP_RPD_CTRL_USE_PMC_PX(port) |
			UTMIP_RPU_SWITC_LOW_USE_PMC_PX(port));
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG1);

	/* set the wake signaling trigger events */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));
	reg &= ~UTMIP_WAKE_VAL(port, ~0);
	reg |= UTMIP_WAKE_VAL_ANY(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));

	/* enable the wake detection */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));
	reg |= UTMIP_MASTER_ENABLE(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));

	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);
	reg |= UTMIP_LINE_WAKEUP_EN(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);

	return 0;
}
EXPORT_SYMBOL(tegra_pmc_utmi_phy_enable_sleepwalk);

int tegra_pmc_utmi_phy_disable_sleepwalk(int port)
{
	u32 reg;

	pr_info("PMC %s : port %dn", __func__, port);

	/* disable the wake detection */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));
	reg &= ~UTMIP_MASTER_ENABLE(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));

	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);
	reg &= ~UTMIP_LINE_WAKEUP_EN(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);

	/* switch the electric control of the USB2.0 pad to XUSB or USB2 */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));
	reg &= ~(UTMIP_FSLS_USE_PMC(port) | UTMIP_PCTRL_USE_PMC(port) |
			UTMIP_TCTRL_USE_PMC(port));
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));

	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG1);
	reg &= ~(UTMIP_RPD_CTRL_USE_PMC_PX(port) |
			UTMIP_RPU_SWITC_LOW_USE_PMC_PX(port));
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG1);

	/* disable wake event triggers of sleepwalk logic */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));
	reg &= ~UTMIP_WAKE_VAL(port, ~0);
	reg |= UTMIP_WAKE_VAL_NONE(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_SLEEP_CFG(port));

	/* power down the line state detectors of the port */
	reg = tegra_pmc_readl(APBDEV_PMC_USB_AO);
	reg |= (USBOP_VAL_PD(port) | USBON_VAL_PD(port));
	tegra_pmc_writel(reg, APBDEV_PMC_USB_AO);

	/* clear alarm of the sleepwalk logic */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_TRIGGERS);
	reg |= UTMIP_CLR_WAKE_ALARM(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_TRIGGERS);

	return 0;
}
EXPORT_SYMBOL(tegra_pmc_utmi_phy_disable_sleepwalk);

int tegra_pmc_hsic_phy_enable_sleepwalk(int port)
{
	u32 reg;

	pr_info("PMC %s : port %dn", __func__, port);

	/* ensure sleepwalk logic is disabled */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_SLEEP_CFG);
	reg &= ~UHSIC_MASTER_ENABLE;
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_SLEEP_CFG);

	/* ensure sleepwalk logics are in low power mode */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_MASTER_CONFIG);
	reg |= UHSIC_PWR(port);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_MASTER_CONFIG);

	/* set debounce time */
	reg = tegra_pmc_readl(APBDEV_PMC_USB_DEBOUNCE_DEL);
	reg &= ~UHSIC_LINE_DEB_CNT(~0);
	reg |= UHSIC_LINE_DEB_CNT(0x1);
	tegra_pmc_writel(reg, APBDEV_PMC_USB_DEBOUNCE_DEL);

	/* ensure fake events of sleepwalk logic are desiabled */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_FAKE);
	reg &= ~(UHSIC_FAKE_STROBE_VAL | UHSIC_FAKE_DATA_VAL |
			UHSIC_FAKE_STROBE_EN | UHSIC_FAKE_DATA_EN);
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_FAKE);

	/* ensure wake events of sleepwalk logic are not latched */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);
	reg &= ~UHSIC_LINE_WAKEUP_EN;
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);

	/* disable wake event triggers of sleepwalk logic */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_SLEEP_CFG);
	reg &= ~UHSIC_WAKE_VAL(~0);
	reg |= UHSIC_WAKE_VAL_NONE;
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_SLEEP_CFG);

	/* power down the line state detectors of the port */
	reg = tegra_pmc_readl(APBDEV_PMC_USB_AO);
	reg |= (STROBE_VAL_PD(port) | DATA0_VAL_PD(port) | DATA1_VAL_PD);
	tegra_pmc_writel(reg, APBDEV_PMC_USB_AO);

	/* save state, HSIC always comes up as HS */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_SAVED_STATE);
	reg &= ~UHSIC_MODE(~0);
	reg |= UHSIC_HS;
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_SAVED_STATE);

	/* enable the trigger of the sleepwalk logic */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_SLEEPWALK_CFG);
	reg |= (UHSIC_WAKE_WALK_EN | UHSIC_LINEVAL_WALK_EN);
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_SLEEPWALK_CFG);

	/* reset the walk pointer and clear the alarm of the sleepwalk logic,
	 * as well as capture the configuration of the USB2.0 port
	 */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_TRIGGERS);
	reg |= (UHSIC_CLR_WALK_PTR | UHSIC_CLR_WAKE_ALARM);
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_TRIGGERS);

	/* setup the pull-ups and pull-downs of the signals during the four
	 * stages of sleepwalk.
	 * maintain a HSIC IDLE and keep driving HSIC RESUME upon remote wake
	 */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_SLEEPWALK_P0);
	reg = (UHSIC_DATA0_RPD_A | UHSIC_DATA0_RPU_B | UHSIC_DATA0_RPU_C |
		UHSIC_DATA0_RPU_D);
	reg |= (UHSIC_STROBE_RPU_A | UHSIC_STROBE_RPD_B | UHSIC_STROBE_RPD_C |
		UHSIC_STROBE_RPD_D);
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_SLEEPWALK_P0);

	/* power up the line state detectors of the port */
	reg = tegra_pmc_readl(APBDEV_PMC_USB_AO);
	reg &= ~(STROBE_VAL_PD(port) | DATA0_VAL_PD(port) | DATA1_VAL_PD);
	tegra_pmc_writel(reg, APBDEV_PMC_USB_AO);

	usleep_range(50, 100);

	/* set the wake signaling trigger events */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_SLEEP_CFG);
	reg &= ~UHSIC_WAKE_VAL(~0);
	reg |= UHSIC_WAKE_VAL_SD10;
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_SLEEP_CFG);

	/* enable the wake detection */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_SLEEP_CFG);
	reg |= UHSIC_MASTER_ENABLE;
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_SLEEP_CFG);

	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);
	reg |= UHSIC_LINE_WAKEUP_EN;
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);

	return 0;
}
EXPORT_SYMBOL(tegra_pmc_hsic_phy_enable_sleepwalk);

int tegra_pmc_hsic_phy_disable_sleepwalk(int port)
{
	u32 reg;

	pr_info("PMC %s : port %dn", __func__, port);

	/* disable the wake detection */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_SLEEP_CFG);
	reg &= ~UHSIC_MASTER_ENABLE;
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_SLEEP_CFG);

	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);
	reg &= ~UHSIC_LINE_WAKEUP_EN;
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_LINE_WAKEUP);

	/* disable wake event triggers of sleepwalk logic */
	reg = tegra_pmc_readl(APBDEV_PMC_UHSIC_SLEEP_CFG);
	reg &= ~UHSIC_WAKE_VAL(~0);
	reg |= UHSIC_WAKE_VAL_NONE;
	tegra_pmc_writel(reg, APBDEV_PMC_UHSIC_SLEEP_CFG);

	/* power down the line state detectors of the port */
	reg = tegra_pmc_readl(APBDEV_PMC_USB_AO);
	reg |= (STROBE_VAL_PD(port) | DATA0_VAL_PD(port) | DATA1_VAL_PD);
	tegra_pmc_writel(reg, APBDEV_PMC_USB_AO);

	/* clear alarm of the sleepwalk logic */
	reg = tegra_pmc_readl(APBDEV_PMC_UTMIP_UHSIC_TRIGGERS);
	reg |= UHSIC_CLR_WAKE_ALARM;
	tegra_pmc_writel(reg, APBDEV_PMC_UTMIP_UHSIC_TRIGGERS);

	return 0;
}
EXPORT_SYMBOL(tegra_pmc_hsic_phy_disable_sleepwalk);

void tegra_pmc_fuse_control_ps18_latch_set(void)
{
	u32 val;

	if (!pmc->soc->has_ps18)
		return;

	val = tegra_pmc_readl(PMC_FUSE_CTRL);
	val &= ~(PMC_FUSE_CTRL_PS18_LATCH_CLEAR);
	tegra_pmc_writel(val, PMC_FUSE_CTRL);
	mdelay(1);
	val |= PMC_FUSE_CTRL_PS18_LATCH_SET;
	tegra_pmc_writel(val, PMC_FUSE_CTRL);
	mdelay(1);
}
EXPORT_SYMBOL(tegra_pmc_fuse_control_ps18_latch_set);

void tegra_pmc_fuse_control_ps18_latch_clear(void)
{
	u32 val;

	if (!pmc->soc->has_ps18)
		return;

	val = tegra_pmc_readl(PMC_FUSE_CTRL);
	val &= ~(PMC_FUSE_CTRL_PS18_LATCH_SET);
	tegra_pmc_writel(val, PMC_FUSE_CTRL);
	mdelay(1);
	val |= PMC_FUSE_CTRL_PS18_LATCH_CLEAR;
	tegra_pmc_writel(val, PMC_FUSE_CTRL);
	mdelay(1);
}
EXPORT_SYMBOL(tegra_pmc_fuse_control_ps18_latch_clear);

void tegra_pmc_fuse_disable_mirroring(void)
{
	u32 val;

	val = tegra_pmc_readl(PMC_FUSE_CTRL);
	if (val & PMC_FUSE_CTRL_ENABLE_REDIRECTION)
		tegra_pmc_writel(PMC_FUSE_CTRL_DISABLE_REDIRECTION,
				PMC_FUSE_CTRL);
}
EXPORT_SYMBOL(tegra_pmc_fuse_disable_mirroring);

void tegra_pmc_fuse_enable_mirroring(void)
{
	u32 val;

	val = tegra_pmc_readl(PMC_FUSE_CTRL);
	if (!(val & PMC_FUSE_CTRL_ENABLE_REDIRECTION))
		tegra_pmc_writel(PMC_FUSE_CTRL_ENABLE_REDIRECTION,
				PMC_FUSE_CTRL);
}
EXPORT_SYMBOL(tegra_pmc_fuse_enable_mirroring);

#ifdef CONFIG_PM_SLEEP
enum tegra_suspend_mode tegra_pmc_get_suspend_mode(void)
{
	return pmc->suspend_mode;
}

void tegra_pmc_set_suspend_mode(enum tegra_suspend_mode mode)
{
	if (mode < TEGRA_SUSPEND_NONE || mode >= TEGRA_MAX_SUSPEND_MODE)
		return;

	pmc->suspend_mode = mode;
}

void tegra_pmc_enter_suspend_mode(enum tegra_suspend_mode mode)
{
	unsigned long long rate = 0;
	u32 value;

	switch (mode) {
	case TEGRA_SUSPEND_LP1:
		rate = 32768;
		break;

	case TEGRA_SUSPEND_LP2:
		rate = clk_get_rate(pmc->clk);
		break;

	default:
		break;
	}

	if (WARN_ON_ONCE(rate == 0))
		rate = 100000000;

	if (rate != pmc->rate) {
		u64 ticks;

		ticks = pmc->cpu_good_time * rate + USEC_PER_SEC - 1;
		do_div(ticks, USEC_PER_SEC);
		tegra_pmc_writel(ticks, PMC_CPUPWRGOOD_TIMER);

		ticks = pmc->cpu_off_time * rate + USEC_PER_SEC - 1;
		do_div(ticks, USEC_PER_SEC);
		tegra_pmc_writel(ticks, PMC_CPUPWROFF_TIMER);

		wmb();

		pmc->rate = rate;
	}

	value = tegra_pmc_readl(PMC_CNTRL);
	value &= ~PMC_CNTRL_SIDE_EFFECT_LP0;
	value |= PMC_CNTRL_CPU_PWRREQ_OE;
	tegra_pmc_writel(value, PMC_CNTRL);
}
#endif

static int tegra_pmc_parse_dt(struct tegra_pmc *pmc, struct device_node *np)
{
	u32 value, values[2];

	if (of_property_read_u32(np, "nvidia,suspend-mode", &value)) {
	} else {
		switch (value) {
		case 0:
			pmc->suspend_mode = TEGRA_SUSPEND_LP0;
			break;

		case 1:
			pmc->suspend_mode = TEGRA_SUSPEND_LP1;
			break;

		case 2:
			pmc->suspend_mode = TEGRA_SUSPEND_LP2;
			break;

		default:
			pmc->suspend_mode = TEGRA_SUSPEND_NONE;
			break;
		}
	}

	pmc->suspend_mode = tegra_pm_validate_suspend_mode(pmc->suspend_mode);

	if (of_property_read_u32(np, "nvidia,cpu-pwr-good-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->cpu_good_time = value;

	if (of_property_read_u32(np, "nvidia,cpu-pwr-off-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->cpu_off_time = value;

	if (of_property_read_u32_array(np, "nvidia,core-pwr-good-time",
				       values, ARRAY_SIZE(values)))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->core_osc_time = values[0];
	pmc->core_pmu_time = values[1];

	if (of_property_read_u32(np, "nvidia,core-pwr-off-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->core_off_time = value;

	pmc->corereq_high = of_property_read_bool(np,
				"nvidia,core-power-req-active-high");

	pmc->sysclkreq_high = of_property_read_bool(np,
				"nvidia,sys-clock-req-active-high");

	pmc->combined_req = of_property_read_bool(np,
				"nvidia,combined-power-req");

	pmc->cpu_pwr_good_en = of_property_read_bool(np,
				"nvidia,cpu-pwr-good-en");

	if (of_property_read_u32_array(np, "nvidia,lp0-vec", values,
				       ARRAY_SIZE(values)))
		if (pmc->suspend_mode == TEGRA_SUSPEND_LP0)
			pmc->suspend_mode = TEGRA_SUSPEND_LP1;

	pmc->lp0_vec_phys = values[0];
	pmc->lp0_vec_size = values[1];

	return 0;
}

static void tegra_pmc_init(struct tegra_pmc *pmc)
{
	u32 value;

	/* Always enable CPU power request */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_CPU_PWRREQ_OE;
	tegra_pmc_writel(value, PMC_CNTRL);

	value = tegra_pmc_readl(PMC_CNTRL);

	if (pmc->sysclkreq_high)
		value &= ~PMC_CNTRL_SYSCLK_POLARITY;
	else
		value |= PMC_CNTRL_SYSCLK_POLARITY;

	/* configure the output polarity while the request is tristated */
	tegra_pmc_writel(value, PMC_CNTRL);

	/* now enable the request */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_SYSCLK_OE;
	tegra_pmc_writel(value, PMC_CNTRL);
}

void tegra_pmc_init_tsense_reset(struct tegra_pmc *pmc)
{
	static const char disabled[] = "emergency thermal reset disabled";
	u32 pmu_addr, ctrl_id, reg_addr, reg_data, pinmux;
	struct device *dev = pmc->dev;
	struct device_node *np;
	u32 value, checksum;

	if (!pmc->soc->has_tsense_reset)
		return;

	np = of_find_node_by_name(pmc->dev->of_node, "i2c-thermtrip");
	if (!np) {
		dev_warn(dev, "i2c-thermtrip node not found, %s.\n", disabled);
		return;
	}

	if (of_property_read_u32(np, "nvidia,i2c-controller-id", &ctrl_id)) {
		dev_err(dev, "I2C controller ID missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,bus-addr", &pmu_addr)) {
		dev_err(dev, "nvidia,bus-addr missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,reg-addr", &reg_addr)) {
		dev_err(dev, "nvidia,reg-addr missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,reg-data", &reg_data)) {
		dev_err(dev, "nvidia,reg-data missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,pinmux-id", &pinmux))
		pinmux = 0;

	value = tegra_pmc_readl(PMC_SENSOR_CTRL);
	value |= PMC_SENSOR_CTRL_SCRATCH_WRITE;
	tegra_pmc_writel(value, PMC_SENSOR_CTRL);

	value = (reg_data << PMC_SCRATCH54_DATA_SHIFT) |
		(reg_addr << PMC_SCRATCH54_ADDR_SHIFT);
	tegra_pmc_writel(value, PMC_SCRATCH54);

	value = PMC_SCRATCH55_RESET_TEGRA;
	value |= ctrl_id << PMC_SCRATCH55_CNTRL_ID_SHIFT;
	value |= pinmux << PMC_SCRATCH55_PINMUX_SHIFT;
	value |= pmu_addr << PMC_SCRATCH55_I2CSLV1_SHIFT;

	/*
	 * Calculate checksum of SCRATCH54, SCRATCH55 fields. Bits 23:16 will
	 * contain the checksum and are currently zero, so they are not added.
	 */
	checksum = reg_addr + reg_data + (value & 0xff) + ((value >> 8) & 0xff)
		+ ((value >> 24) & 0xff);
	checksum &= 0xff;
	checksum = 0x100 - checksum;

	value |= checksum << PMC_SCRATCH55_CHECKSUM_SHIFT;

	tegra_pmc_writel(value, PMC_SCRATCH55);

	value = tegra_pmc_readl(PMC_SENSOR_CTRL);
	value |= PMC_SENSOR_CTRL_ENABLE_RST;
	tegra_pmc_writel(value, PMC_SENSOR_CTRL);

	dev_info(pmc->dev, "emergency thermal reset enabled\n");

out:
	of_node_put(np);
}

static int tegra_pmc_probe(struct platform_device *pdev)
{
	void __iomem *base = pmc->base;
	struct resource *res;
	int err;

	err = tegra_pmc_parse_dt(pmc, pdev->dev.of_node);
	if (err < 0)
		return err;

	/* take over the memory region from the early initialization */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pmc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pmc->base))
		return PTR_ERR(pmc->base);

	iounmap(base);

	pmc->clk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(pmc->clk)) {
		err = PTR_ERR(pmc->clk);
		dev_err(&pdev->dev, "failed to get pclk: %d\n", err);
		return err;
	}

	pmc->dev = &pdev->dev;

	tegra_pmc_init(pmc);

	tegra_pmc_init_tsense_reset(pmc);

	if (IS_ENABLED(CONFIG_DEBUG_FS)) {
		err = tegra_powergate_debugfs_init();
		if (err < 0)
			return err;
	}

	err = register_restart_handler(&tegra_pmc_restart_handler);
	if (err) {
		dev_err(&pdev->dev, "unable to register restart handler, %d\n",
			err);
		return err;
	}

	/* Prod setting like platform specific rails */
	prod_list = devm_tegra_prod_get(&pdev->dev);
	if (IS_ERR(prod_list)) {
		err = PTR_ERR(prod_list);
		dev_info(&pdev->dev, "prod list not found: %d\n",
			 err);
		prod_list = NULL;
	} else {
		err = tegra_prod_set_by_name(&base,
				"prod_c_platform_pad_rail", prod_list);
		if (err < 0) {
			dev_info(&pdev->dev,
				 "prod setting for rail not found\n");
		} else {
			dev_info(&pdev->dev,
				 "POWER_DET: 0x%08x, POWR_VAL: 0x%08x\n",
				 tegra_pmc_readl(PMC_PWR_DET_ENABLE),
				 tegra_pmc_readl(PMC_PWR_DET_VAL));
		}
	}

#ifdef CONFIG_PADCTRL_TEGRA210_PMC
	/* Register as pad controller */
	err = tegra210_pmc_padctrl_init(&pdev->dev, pdev->dev.of_node);
	if (err)
		pr_err("ERROR: Pad control driver init failed: %d\n",
			err);
#endif

#ifdef CONFIG_TEGRA210_BOOTROM_PMC
	err = tegra210_boorom_pmc_init(&pdev->dev);
	if (err < 0)
		pr_err("ERROR: Bootrom PMC config failed: %d\n", err);
#endif

	/* handle PMC reboot reason with PSCI */
	if (arm_pm_restart)
		psci_handle_reboot_cmd = tegra_pmc_program_reboot_reason;

	return 0;
}

#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_ARM)
static int tegra_pmc_suspend(struct device *dev)
{
	tegra_pmc_writel(virt_to_phys(tegra_resume), PMC_SCRATCH41);

	return 0;
}

static int tegra_pmc_resume(struct device *dev)
{
	tegra_pmc_writel(0x0, PMC_SCRATCH41);

	return 0;
}

static SIMPLE_DEV_PM_OPS(tegra_pmc_pm_ops, tegra_pmc_suspend, tegra_pmc_resume);

#endif

static const char * const tegra20_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "cpu",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
};

static const struct tegra_pmc_soc tegra20_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra20_powergates),
	.powergates = tegra20_powergates,
	.num_cpu_powergates = 0,
	.cpu_powergates = NULL,
	.has_tsense_reset = false,
	.has_gpu_clamps = false,
};

static const char * const tegra30_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "cpu0",
	[TEGRA_POWERGATE_3D] = "3d0",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_3D1] = "3d1",
};

static const u8 tegra30_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra30_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra30_powergates),
	.powergates = tegra30_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra30_cpu_powergates),
	.cpu_powergates = tegra30_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = false,
};

static const char * const tegra114_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_C1NC] = "c1nc",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
};

static const u8 tegra114_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra114_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra114_powergates),
	.powergates = tegra114_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra114_cpu_powergates),
	.cpu_powergates = tegra114_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = false,
};

static const char * const tegra124_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_C1NC] = "c1nc",
	[TEGRA_POWERGATE_SOR] = "sor",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
	[TEGRA_POWERGATE_VIC] = "vic",
	[TEGRA_POWERGATE_IRAM] = "iram",
};

static const u8 tegra124_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra124_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra124_powergates),
	.powergates = tegra124_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra124_cpu_powergates),
	.cpu_powergates = tegra124_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = true,
};

static const char * const tegra210_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_C1NC] = "c1nc",
	[TEGRA_POWERGATE_SOR] = "sor",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
	[TEGRA_POWERGATE_VIC] = "vic",
	[TEGRA_POWERGATE_IRAM] = "iram",
	[TEGRA_POWERGATE_NVDEC] = "nvdec",
	[TEGRA_POWERGATE_NVJPG] = "nvjpg",
	[TEGRA_POWERGATE_AUD] = "aud",
	[TEGRA_POWERGATE_DFD] = "dfd",
	[TEGRA_POWERGATE_VE2] = "ve2",
};

static const u8 tegra210_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra210_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra210_powergates),
	.powergates = tegra210_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra210_cpu_powergates),
	.cpu_powergates = tegra210_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = true,
	.has_ps18 = true,
};

static const struct of_device_id tegra_pmc_match[] = {
	{ .compatible = "nvidia,tegra210-pmc", .data = &tegra210_pmc_soc },
	{ .compatible = "nvidia,tegra132-pmc", .data = &tegra124_pmc_soc },
	{ .compatible = "nvidia,tegra124-pmc", .data = &tegra124_pmc_soc },
	{ .compatible = "nvidia,tegra114-pmc", .data = &tegra114_pmc_soc },
	{ .compatible = "nvidia,tegra30-pmc", .data = &tegra30_pmc_soc },
	{ .compatible = "nvidia,tegra20-pmc", .data = &tegra20_pmc_soc },
	{ }
};

static struct platform_driver tegra_pmc_driver = {
	.driver = {
		.name = "tegra-pmc",
		.suppress_bind_attrs = true,
		.of_match_table = tegra_pmc_match,
#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_ARM)
		.pm = &tegra_pmc_pm_ops,
#endif
	},
	.probe = tegra_pmc_probe,
};
builtin_platform_driver(tegra_pmc_driver);

/*
 * Early initialization to allow access to registers in the very early boot
 * process.
 */
static int __init tegra_pmc_early_init(void)
{
	const struct of_device_id *match;
	struct device_node *np;
	struct resource regs;
	bool invert;
	u32 value;

	np = of_find_matching_node_and_match(NULL, tegra_pmc_match, &match);
	if (!np) {
		/*
		 * Fall back to legacy initialization for 32-bit ARM only. All
		 * 64-bit ARM device tree files for Tegra are required to have
		 * a PMC node.
		 *
		 * This is for backwards-compatibility with old device trees
		 * that didn't contain a PMC node. Note that in this case the
		 * SoC data can't be matched and therefore powergating is
		 * disabled.
		 */
		if (IS_ENABLED(CONFIG_ARM) && soc_is_tegra()) {
			pr_warn("DT node not found, powergating disabled\n");

			regs.start = 0x7000e400;
			regs.end = 0x7000e7ff;
			regs.flags = IORESOURCE_MEM;

			pr_warn("Using memory region %pR\n", &regs);
		} else {
			/*
			 * At this point we're not running on Tegra, so play
			 * nice with multi-platform kernels.
			 */
			return 0;
		}
	} else {
		/*
		 * Extract information from the device tree if we've found a
		 * matching node.
		 */
		if (of_address_to_resource(np, 0, &regs) < 0) {
			pr_err("failed to get PMC registers\n");
			return -ENXIO;
		}

		pmc->soc = match->data;
	}

	pmc->base = ioremap_nocache(regs.start, resource_size(&regs));
	if (!pmc->base) {
		pr_err("failed to map PMC registers\n");
		return -ENXIO;
	}

	mutex_init(&pmc->powergates_lock);

	/*
	 * Invert the interrupt polarity if a PMC device tree node exists and
	 * contains the nvidia,invert-interrupt property.
	 */
	invert = of_property_read_bool(np, "nvidia,invert-interrupt");

	value = tegra_pmc_readl(PMC_CNTRL);

	if (invert)
		value |= PMC_CNTRL_INTR_POLARITY;
	else
		value &= ~PMC_CNTRL_INTR_POLARITY;

	tegra_pmc_writel(value, PMC_CNTRL);

	return 0;
}
early_initcall(tegra_pmc_early_init);
