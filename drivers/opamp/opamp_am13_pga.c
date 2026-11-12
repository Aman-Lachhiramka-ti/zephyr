/*
 * Copyright (c) 2026 Texas Instruments Incorporated
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_am13_pga

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/opamp.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/* Register offsets from instance base */
#define AM13_PGA_PGACTL_OFFSET  0x00U
#define AM13_PGA_MUXSEL_OFFSET  0x04U

/* PGACTL bit fields */
#define AM13_PGA_PGACTL_PGAEN            BIT(0)
#define AM13_PGA_PGACTL_FILT_MASK        GENMASK(3, 1)
#define AM13_PGA_PGACTL_GAIN_MASK        GENMASK(7, 5)
#define AM13_PGA_PGACTL_OUTENABLE        BIT(8)
/* PGA_OUTEN_INTGAIN is active-low: 0 = route internal gain to pin */
#define AM13_PGA_PGACTL_OUTEN_INTGAIN    BIT(9)

/* MUXSEL bit fields */
#define AM13_PGA_MUXSEL_PMUX_MASK        GENMASK(3, 0)
#define AM13_PGA_MUXSEL_NMUX_MASK        GENMASK(6, 4)
#define AM13_PGA_MUXSEL_MMUX_MASK        GENMASK(10, 8)
#define AM13_PGA_MUXSEL_FBONPIN          BIT(12)
#define AM13_PGA_MUXSEL_CHOP_MASK        GENMASK(14, 13)

struct am13_pga_config {
	mm_reg_t base;
	const struct pinctrl_dev_config *pinctrl;
	uint16_t pgactl_init; /* pre-computed; excludes PGAEN bit */
	uint16_t muxsel_init;
	bool has_gain; /* false when MMUXSEL selects the standalone op-amp */
};

struct am13_pga_data {
	struct k_mutex lock;
};

/*
 * Gain switch settling takes up to 10 us (AM13E23019 datasheet, Table
 * 6.9.3.1.2). The caller must wait before triggering an ADC conversion.
 */
static int am13_pga_set_gain(const struct device *dev, enum opamp_gain gain)
{
	const struct am13_pga_config *config = dev->config;
	struct am13_pga_data *data = dev->data;
	uint16_t code;
	uint16_t val;

	if (!config->has_gain) {
		return -ENOTSUP;
	}

	switch (gain) {
	case OPAMP_GAIN_1:
		code = 0;
		break;
	case OPAMP_GAIN_2:
		code = 1;
		break;
	case OPAMP_GAIN_4:
		code = 2;
		break;
	case OPAMP_GAIN_8:
		code = 3;
		break;
	case OPAMP_GAIN_16:
		code = 4;
		break;
	case OPAMP_GAIN_32:
		code = 5;
		break;
	case OPAMP_GAIN_64:
		code = 6;
		break;
	default:
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	val = sys_read16(config->base + AM13_PGA_PGACTL_OFFSET);
	val &= ~AM13_PGA_PGACTL_GAIN_MASK;
	val |= FIELD_PREP(AM13_PGA_PGACTL_GAIN_MASK, code);
	sys_write16(val, config->base + AM13_PGA_PGACTL_OFFSET);

	k_mutex_unlock(&data->lock);

	return 0;
}

static DEVICE_API(opamp, am13_pga_api) = {
	.set_gain = am13_pga_set_gain,
};

static int am13_pga_init(const struct device *dev)
{
	const struct am13_pga_config *config = dev->config;
	struct am13_pga_data *data = dev->data;
	int ret = 0;

	k_mutex_init(&data->lock);

	if (config->pinctrl != NULL) {
		ret = pinctrl_apply_state(config->pinctrl, PINCTRL_STATE_DEFAULT);
		if (ret != 0) {
			return ret;
		}
	}

	/*
	 * No PWREN/RSTCTL sequence needed — PGA is clocked by the system
	 * bus and has no power-domain enable register. Factory offset and
	 * gain trim is applied by ROM boot code (Device_cal) before main().
	 *
	 * Write MUXSEL before PGACTL so the input topology is settled before
	 * PGAEN=1 starts the amplifier.
	 */
	sys_write16(config->muxsel_init, config->base + AM13_PGA_MUXSEL_OFFSET);
	sys_write16(config->pgactl_init, config->base + AM13_PGA_PGACTL_OFFSET);
	sys_write16(config->pgactl_init | AM13_PGA_PGACTL_PGAEN,
		    config->base + AM13_PGA_PGACTL_OFFSET);

	return 0;
}

#define AM13_PGA_PINCTRL_DT_INST_DEFINE(inst)					\
	COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(inst, default),			\
		    (PINCTRL_DT_INST_DEFINE(inst)), ())

#define AM13_PGA_PINCTRL_DT_INST_GET(inst)					\
	COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(inst, default),			\
		    (PINCTRL_DT_INST_DEV_CONFIG_GET(inst)), (NULL))

/* DT chop enum {disabled, adc-soc, sw-extctrl} -> CHOP field {0, 2, 3} */
#define AM13_PGA_CHOP(inst)							\
	(DT_INST_ENUM_IDX(inst, ti_chop_mode) == 0 ? 0 :			\
	 DT_INST_ENUM_IDX(inst, ti_chop_mode) + 1)

/*
 * FILT_RES_SEL maps DT enum values [0,50,100,200,400,800] to register
 * field codes 0..5 via DT_INST_ENUM_IDX (index into the enum list).
 *
 * PGA_OUTEN_INTGAIN is active-low: the DT property ti,internal-gain-to-pin
 * is active-high, so the driver bit is 0 when the property is set and
 * AM13_PGA_PGACTL_OUTEN_INTGAIN when it is not.
 */
#define AM13_PGA_INIT(inst)							\
	BUILD_ASSERT(!DT_INST_ENUM_HAS_VALUE(inst, functional_mode, non_inverting) ||	\
		     DT_INST_ENUM_IDX(inst, ti_mmux_input) == 1,		\
		     "functional-mode non_inverting requires ti,mmux-input non-inverting");	\
	AM13_PGA_PINCTRL_DT_INST_DEFINE(inst);					\
	static const struct am13_pga_config am13_pga_cfg_##inst = {		\
		.base = DT_INST_REG_ADDR(inst),					\
		.pinctrl = AM13_PGA_PINCTRL_DT_INST_GET(inst),			\
		.pgactl_init =							\
			FIELD_PREP(AM13_PGA_PGACTL_FILT_MASK,			\
				   DT_INST_ENUM_IDX(inst,			\
					ti_filter_resistor_ohms)) |		\
			COND_CODE_1(DT_INST_PROP(inst, ti_output_to_pin),	\
				    (AM13_PGA_PGACTL_OUTENABLE), (0U)) |	\
			COND_CODE_1(DT_INST_PROP(inst,				\
					ti_internal_gain_to_pin),		\
				    (0U),					\
				    (AM13_PGA_PGACTL_OUTEN_INTGAIN)),		\
		.muxsel_init =							\
			FIELD_PREP(AM13_PGA_MUXSEL_PMUX_MASK,			\
				   DT_INST_ENUM_IDX(inst, ti_pmux_input)) |	\
			FIELD_PREP(AM13_PGA_MUXSEL_NMUX_MASK,			\
				   DT_INST_ENUM_IDX(inst, ti_nmux_input)) |	\
			FIELD_PREP(AM13_PGA_MUXSEL_MMUX_MASK,			\
				   DT_INST_ENUM_IDX(inst, ti_mmux_input)) |	\
			COND_CODE_1(DT_INST_PROP(inst, ti_feedback_on_pin),	\
				    (AM13_PGA_MUXSEL_FBONPIN), (0U)) |		\
			FIELD_PREP(AM13_PGA_MUXSEL_CHOP_MASK,			\
				   AM13_PGA_CHOP(inst)),			\
		.has_gain = DT_INST_ENUM_IDX(inst, ti_mmux_input) != 0,		\
	};									\
	static struct am13_pga_data am13_pga_data_##inst;			\
	DEVICE_DT_INST_DEFINE(inst, am13_pga_init, NULL,			\
			      &am13_pga_data_##inst,				\
			      &am13_pga_cfg_##inst, POST_KERNEL,		\
			      CONFIG_OPAMP_INIT_PRIORITY, &am13_pga_api);

DT_INST_FOREACH_STATUS_OKAY(AM13_PGA_INIT)
