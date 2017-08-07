/*
 * exynos_tmu.c - Samsung EXYNOS TMU (Thermal Management Unit)
 *
 *  Copyright (C) 2014 Samsung Electronics
 *  Bartlomiej Zolnierkiewicz <b.zolnierkie@samsung.com>
 *  Lukasz Majewski <l.majewski@samsung.com>
 *
 *  Copyright (C) 2011 Samsung Electronics
 *  Donggeun Kim <dg77.kim@samsung.com>
 *  Amit Daniel Kachhap <amit.kachhap@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/cpufreq.h>
#include <linux/suspend.h>
#include <linux/pm_qos.h>
#include <linux/threads.h>
#include <linux/thermal.h>
#include <linux/gpu_cooling.h>
#include <linux/isp_cooling.h>
#include <linux/slab.h>
#include <soc/samsung/tmu.h>

#include "exynos_tmu.h"
#include "../thermal_core.h"

/* Exynos generic registers */
#define EXYNOS_TMU_REG_TRIMINFO		0x0
#define EXYNOS_TMU_REG_TRIMINFO1	0x4
#define EXYNOS_TMU_REG_TRIMINFO2	0x8
#define EXYNOS_TMU_REG_CONTROL		0x20
#define EXYNOS_TMU_REG_STATUS		0x28
#define EXYNOS_TMU_REG_CURRENT_TEMP1_0 	0x40
#define EXYNOS_TMU_REG_CURRENT_TEMP4_2  0x44
#define EXYNOS_TMU_REG_CURRENT_TEMP7_5  0x48
#define EXYNOS_TMU_REG_INTEN		0x110
#define EXYNOS_TMU_REG_INTSTAT		0x74
#define EXYNOS_TMU_REG_INTCLEAR		0x78

#define EXYNOS_TMU_REF_VOLTAGE_SHIFT	24
#define EXYNOS_TMU_REF_VOLTAGE_MASK	0x1f
#define EXYNOS_TMU_BUF_SLOPE_SEL_MASK	0xf
#define EXYNOS_TMU_BUF_SLOPE_SEL_SHIFT	8
#define EXYNOS_TMU_CORE_EN_SHIFT	0

#define EXYNOS_TMU_TRIP_MODE_SHIFT	13
#define EXYNOS_TMU_TRIP_MODE_MASK	0x7
#define EXYNOS_TMU_THERM_TRIP_EN_SHIFT	12

#define EXYNOS_TMU_INTEN_RISE0_SHIFT	0
#define EXYNOS_TMU_INTEN_FALL0_SHIFT	16

#define EXYNOS_EMUL_TIME	0x57F0
#define EXYNOS_EMUL_TIME_MASK	0xffff
#define EXYNOS_EMUL_TIME_SHIFT	16
#define EXYNOS_EMUL_DATA_SHIFT	7
#define EXYNOS_EMUL_DATA_MASK	0x1FF
#define EXYNOS_EMUL_ENABLE	0x1

#define EXYNOS_THD_TEMP_RISE7_6			0x50
#define EXYNOS_THD_TEMP_FALL7_6			0x60
#define EXYNOS_THD_TEMP_R_OFFSET		0x120
#define EXYNOS_TMU_INTEN_RISE0_SHIFT		0
#define EXYNOS_TMU_INTEN_RISE1_SHIFT		1
#define EXYNOS_TMU_INTEN_RISE2_SHIFT		2
#define EXYNOS_TMU_INTEN_RISE3_SHIFT		3
#define EXYNOS_TMU_INTEN_RISE4_SHIFT		4
#define EXYNOS_TMU_INTEN_RISE5_SHIFT		5
#define EXYNOS_TMU_INTEN_RISE6_SHIFT		6
#define EXYNOS_TMU_INTEN_RISE7_SHIFT		7

#define EXYNOS_TMU_CALIB_SEL_SHIFT		(23)
#define EXYNOS_TMU_CALIB_SEL_MASK		(0x1)
#define EXYNOS_TMU_TEMP_SHIFT			(9)
#define EXYNOS_TMU_TEMP_MASK			(0x1ff)
#define EXYNOS_TMU_TRIMINFO_85_P0_SHIFT		(9)
#define EXYNOS_TRIMINFO_ONE_POINT_TRIMMING	(0)
#define EXYNOS_TRIMINFO_TWO_POINT_TRIMMING	(1)
#define EXYNOS_TMU_T_BUF_VREF_SEL_SHIFT		(18)
#define EXYNOS_TMU_T_BUF_VREF_SEL_MASK		(0x1F)
#define EXYNOS_TMU_T_BUF_SLOPE_SEL_SHIFT	(18)
#define EXYNOS_TMU_T_BUF_SLOPE_SEL_MASK		(0xF)

#define EXYNOS_TMU_REG_INTPEND0			(0x118)
#define EXYNOS_TMU_REG_INTPEND5			(0x318)
#define EXYNOS_TMU_REG_INTPEN_OFFSET		(0x10)
#define EXYNOS_TMU_REG_EMUL_CON			(0x160)

#define EXYNOS_TMU_REG_AVG_CON			(0x38)
#define EXYNOS_TMU_AVG_CON_SHIFT		(18)
#define EXYNOS_TMU_AVG_CON_MASK			(0x3)
#define EXYNOS_TMU_AVG_MODE_MASK		(0x7)
#define EXYNOS_TMU_AVG_MODE_DEFAULT		(0x0)
#define EXYNOS_TMU_AVG_MODE_2			(0x5)
#define EXYNOS_TMU_AVG_MODE_4			(0x6)

#define EXYNOS_TMU_DEM_ENABLE			(1)
#define EXYNOS_TMU_DEM_SHIFT			(4)

#define MCELSIUS	1000

#define TOTAL_SENSORS	8

static bool suspended;
static bool is_cpu_hotplugged_out;
static DEFINE_MUTEX (thermal_suspend_lock);

/* list of multiple instance for each thermal sensor */
static LIST_HEAD(dtm_dev_list);
struct cpufreq_frequency_table gpu_freq_table[10];

struct remote_sensor_info {
	u16 sensor_num;
	u16 cal_type;
	u32 temp_error1;
	u32 temp_error2;
};

/**
 * struct exynos_tmu_data : A structure to hold the private data of the TMU
	driver
 * @id: identifier of the one instance of the TMU controller.
 * @pdata: pointer to the tmu platform/configuration data
 * @base: base address of the single instance of the TMU controller.
 * @base_second: base address of the common registers of the TMU controller.
 * @irq: irq number of the TMU controller.
 * @soc: id of the SOC type.
 * @irq_work: pointer to the irq work structure.
 * @lock: lock to implement synchronization.
 * @temp_error1: fused value of the first point trim.
 * @temp_error2: fused value of the second point trim.
 * @regulator: pointer to the TMU regulator structure.
 * @reg_conf: pointer to structure to register with core thermal.
 * @tmu_initialize: SoC specific TMU initialization method
 * @tmu_control: SoC specific TMU control method
 * @tmu_read: SoC specific TMU temperature read method
 * @tmu_set_emulation: SoC specific TMU emulation setting method
 * @tmu_clear_irqs: SoC specific TMU interrupts clearing method
 */
struct exynos_tmu_data {
	int id;
	/* Throttle hotplug related variables */
	bool hotplug_enable;
	int hotplug_in_threshold;
	int hotplug_out_threshold;
	struct exynos_tmu_platform_data *pdata;
	void __iomem *base;
	int irq;
	enum soc_type soc;
	struct work_struct irq_work;
	struct mutex lock;
	u16 temp_error1, temp_error2;
	struct thermal_zone_device *tzd;
	struct thermal_cooling_device *cool_dev;
	struct list_head node;
	u32 sensors;
	int num_of_remotes;
	struct remote_sensor_info *remote_sensors;
	int sensing_mode;
	char tmu_name[THERMAL_NAME_LENGTH];
	struct device_node *np;

	int (*tmu_initialize)(struct platform_device *pdev);
	void (*tmu_control)(struct platform_device *pdev, bool on);
	int (*tmu_read)(struct exynos_tmu_data *data);
	void (*tmu_set_emulation)(struct exynos_tmu_data *data, int temp);
	void (*tmu_clear_irqs)(struct exynos_tmu_data *data);
};

static void exynos_report_trigger(struct exynos_tmu_data *p)
{
	char data[10], *envp[] = { data, NULL };
	struct thermal_zone_device *tz = p->tzd;
	int temp;
	unsigned int i;

	if (!tz) {
		pr_err("No thermal zone device defined\n");
		return;
	}

	thermal_zone_device_update(tz);

	mutex_lock(&tz->lock);
	/* Find the level for which trip happened */
	for (i = 0; i < of_thermal_get_ntrips(tz); i++) {
		tz->ops->get_trip_temp(tz, i, &temp);
		if (tz->last_temperature < temp)
			break;
	}

	snprintf(data, sizeof(data), "%u", i);
	kobject_uevent_env(&tz->device.kobj, KOBJ_CHANGE, envp);
	mutex_unlock(&tz->lock);
}

/*
 * TMU treats temperature as a mapped temperature code.
 * The temperature is converted differently depending on the calibration type.
 */
static int temp_to_code(struct exynos_tmu_data *data, u8 temp)
{
	struct exynos_tmu_platform_data *pdata = data->pdata;
	int temp_code;

	if (temp > EXYNOS_MAX_TEMP)
		temp = EXYNOS_MAX_TEMP;
	else if (temp < EXYNOS_MIN_TEMP)
		temp = EXYNOS_MIN_TEMP;

	switch (pdata->cal_type) {
	case TYPE_TWO_POINT_TRIMMING:
		temp_code = (temp - pdata->first_point_trim) *
			(data->temp_error2 - data->temp_error1) /
			(pdata->second_point_trim - pdata->first_point_trim) +
			data->temp_error1;
		break;
	case TYPE_ONE_POINT_TRIMMING:
		temp_code = temp + data->temp_error1 - pdata->first_point_trim;
		break;
	default:
		temp_code = temp + pdata->default_temp_offset;
		break;
	}

	return temp_code;
}

/*
 * Calculate a temperature value from a temperature code.
 * The unit of the temperature is degree Celsius.
 */
static int code_to_temp(struct exynos_tmu_data *data, u16 temp_code)
{
	struct exynos_tmu_platform_data *pdata = data->pdata;
	int temp;

	switch (pdata->cal_type) {
	case TYPE_TWO_POINT_TRIMMING:
		temp = (temp_code - data->temp_error1) *
			(pdata->second_point_trim - pdata->first_point_trim) /
			(data->temp_error2 - data->temp_error1) +
			pdata->first_point_trim;
		break;
	case TYPE_ONE_POINT_TRIMMING:
		temp = temp_code - data->temp_error1 + pdata->first_point_trim;
		break;
	default:
		temp = temp_code - pdata->default_temp_offset;
		break;
	}

	/* temperature should range between minimum and maximum */
	if (temp > EXYNOS_MAX_TEMP)
		temp = EXYNOS_MAX_TEMP;
	else if (temp < EXYNOS_MIN_TEMP)
		temp = EXYNOS_MIN_TEMP;

	return temp;
}

static int exynos_tmu_initialize(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	int ret;

	mutex_lock(&data->lock);
	ret = data->tmu_initialize(pdev);
	mutex_unlock(&data->lock);

	return ret;
}

static u32 get_con_reg(struct exynos_tmu_data *data, u32 con)
{
	struct exynos_tmu_platform_data *pdata = data->pdata;

	con &= ~(EXYNOS_TMU_REF_VOLTAGE_MASK << EXYNOS_TMU_REF_VOLTAGE_SHIFT);
	con |= pdata->reference_voltage << EXYNOS_TMU_REF_VOLTAGE_SHIFT;

	con &= ~(EXYNOS_TMU_BUF_SLOPE_SEL_MASK << EXYNOS_TMU_BUF_SLOPE_SEL_SHIFT);
	con |= (pdata->gain << EXYNOS_TMU_BUF_SLOPE_SEL_SHIFT);

	if (pdata->noise_cancel_mode) {
		con &= ~(EXYNOS_TMU_TRIP_MODE_MASK << EXYNOS_TMU_TRIP_MODE_SHIFT);
		con |= (pdata->noise_cancel_mode << EXYNOS_TMU_TRIP_MODE_SHIFT);
	}

	return con;
}

static void exynos_tmu_control(struct platform_device *pdev, bool on)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);

	mutex_lock(&data->lock);
	data->tmu_control(pdev, on);
	mutex_unlock(&data->lock);
}

static int exynos8890_tmu_initialize(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct thermal_zone_device *tz = data->tzd;
	struct exynos_tmu_platform_data *pdata = data->pdata;
	unsigned int rising_threshold = 0, falling_threshold = 0;
	int temp, temp_hist;
	unsigned int trim_info;
	unsigned int reg_off, bit_off;
	int threshold_code, i;

	/* Check tmu core ready status */
	trim_info = readl(data->base + EXYNOS_TMU_REG_TRIMINFO);

	/* Check thermal calibration type */
	pdata->cal_type = (trim_info >> EXYNOS_TMU_CALIB_SEL_SHIFT)
			& EXYNOS_TMU_CALIB_SEL_MASK;

	/* Check temp_error1 and error2 value */
	data->temp_error1 = trim_info & EXYNOS_TMU_TEMP_MASK;
	data->temp_error2 = (trim_info >> EXYNOS_TMU_TRIMINFO_85_P0_SHIFT)
				& EXYNOS_TMU_TEMP_MASK;

	if (!data->temp_error1)
		data->temp_error1 = pdata->efuse_value & EXYNOS_TMU_TEMP_MASK;
	if (!data->temp_error2)
		data->temp_error2 = (pdata->efuse_value >>
					EXYNOS_TMU_TRIMINFO_85_P0_SHIFT)
					& EXYNOS_TMU_TEMP_MASK;
	/* Write temperature code for rising and falling threshold */
	for (i = (of_thermal_get_ntrips(tz) - 1); i >= 0; i--) {
		/*
		 * On exynos7 there are 4 rising and 4 falling threshold
		 * registers (0x50-0x5c and 0x60-0x6c respectively). Each
		 * register holds the value of two threshold levels (at bit
		 * offsets 0 and 16). Based on the fact that there are atmost
		 * eight possible trigger levels, calculate the register and
		 * bit offsets where the threshold levels are to be written.
		 *
		 * e.g. EXYNOS_THD_TEMP_RISE7_6 (0x50)
		 * [24:16] - Threshold level 7
		 * [8:0] - Threshold level 6
		 * e.g. EXYNOS_THD_TEMP_RISE5_4 (0x54)
		 * [24:16] - Threshold level 5
		 * [8:0] - Threshold level 4
		 *
		 * and similarly for falling thresholds.
		 *
		 * Based on the above, calculate the register and bit offsets
		 * for rising/falling threshold levels and populate them.
		 */
		reg_off = ((7 - i) / 2) * 4;
		bit_off = ((8 - i) % 2);

		tz->ops->get_trip_temp(tz, i, &temp);
		temp /= MCELSIUS;

		tz->ops->get_trip_hyst(tz, i, &temp_hist);
		temp_hist = temp - (temp_hist / MCELSIUS);

		/* Set 9-bit temperature code for rising threshold levels */
		threshold_code = temp_to_code(data, temp);
		rising_threshold = readl(data->base +
			EXYNOS_THD_TEMP_RISE7_6 + reg_off);
		rising_threshold &= ~(EXYNOS_TMU_TEMP_MASK << (16 * bit_off));
		rising_threshold |= threshold_code << (16 * bit_off);
		writel(rising_threshold,
		       data->base + EXYNOS_THD_TEMP_RISE7_6 + reg_off);

		/* Set 9-bit temperature code for falling threshold levels */
		threshold_code = temp_to_code(data, temp_hist);
		falling_threshold &= ~(EXYNOS_TMU_TEMP_MASK << (16 * bit_off));
		falling_threshold |= threshold_code << (16 * bit_off);
		writel(falling_threshold,
		       data->base + EXYNOS_THD_TEMP_FALL7_6 + reg_off);
	}

	data->tmu_clear_irqs(data);

	return 0;
}

static void exynos8890_tmu_control(struct platform_device *pdev, bool on)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct thermal_zone_device *tz = data->tzd;
	unsigned int con, interrupt_en, trim_info, trim_info1;
	unsigned int t_buf_vref_sel, t_buf_slope_sel;

	trim_info = readl(data->base + EXYNOS_TMU_REG_TRIMINFO);
	trim_info1 = readl(data->base + EXYNOS_TMU_REG_TRIMINFO1);

	/* Save fuse buf_vref_sel, calib_sel value to TRIMINFO and 1 register */
	t_buf_vref_sel = (trim_info >> EXYNOS_TMU_T_BUF_VREF_SEL_SHIFT)
				& (EXYNOS_TMU_T_BUF_VREF_SEL_MASK);
	t_buf_slope_sel = (trim_info1 >> EXYNOS_TMU_T_BUF_SLOPE_SEL_SHIFT)
				& (EXYNOS_TMU_T_BUF_SLOPE_SEL_MASK);

	con = get_con_reg(data, readl(data->base + EXYNOS_TMU_REG_CONTROL));

	if (on) {
		con |= (t_buf_vref_sel << EXYNOS_TMU_REF_VOLTAGE_SHIFT);
		con |= (t_buf_slope_sel << EXYNOS_TMU_BUF_SLOPE_SEL_SHIFT);
		con |= (1 << EXYNOS_TMU_CORE_EN_SHIFT);
		con |= (1 << EXYNOS_TMU_THERM_TRIP_EN_SHIFT);
		interrupt_en =
			(of_thermal_is_trip_valid(tz, 7)
			<< EXYNOS_TMU_INTEN_RISE7_SHIFT) |
			(of_thermal_is_trip_valid(tz, 6)
			<< EXYNOS_TMU_INTEN_RISE6_SHIFT) |
			(of_thermal_is_trip_valid(tz, 5)
			<< EXYNOS_TMU_INTEN_RISE5_SHIFT) |
			(of_thermal_is_trip_valid(tz, 4)
			<< EXYNOS_TMU_INTEN_RISE4_SHIFT) |
			(of_thermal_is_trip_valid(tz, 3)
			<< EXYNOS_TMU_INTEN_RISE3_SHIFT) |
			(of_thermal_is_trip_valid(tz, 2)
			<< EXYNOS_TMU_INTEN_RISE2_SHIFT) |
			(of_thermal_is_trip_valid(tz, 1)
			<< EXYNOS_TMU_INTEN_RISE1_SHIFT) |
			(of_thermal_is_trip_valid(tz, 0)
			<< EXYNOS_TMU_INTEN_RISE0_SHIFT);

		interrupt_en |=
			interrupt_en << EXYNOS_TMU_INTEN_FALL0_SHIFT;
	} else {
		con &= ~(1 << EXYNOS_TMU_CORE_EN_SHIFT);
		con &= ~(1 << EXYNOS_TMU_THERM_TRIP_EN_SHIFT);
		interrupt_en = 0; /* Disable all interrupts */
	}

	writel(interrupt_en, data->base + EXYNOS_TMU_REG_INTEN);
	writel(con, data->base + EXYNOS_TMU_REG_CONTROL);
}

static int exynos8895_tmu_initialize(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct thermal_zone_device *tz = data->tzd;
	struct exynos_tmu_platform_data *pdata = data->pdata;
	unsigned int rising_threshold = 0, falling_threshold = 0;
	int temp, temp_hist;
	unsigned int trim_info;
	unsigned int reg_off, bit_off;
	int threshold_code, i, j;

	for (i = 0; i < TOTAL_SENSORS; i++) {
		if (data->sensors & (1 << i)) {
			/* Check tmu core ready status */
			trim_info = readl(data->base + EXYNOS_TMU_REG_TRIMINFO + 0x4 * i);

			/* If i is 0, it is main sensor. The others are remote sensors */
			if (!i) {
				/* Check thermal calibration type */
				pdata->cal_type = (trim_info >> EXYNOS_TMU_CALIB_SEL_SHIFT)
						& EXYNOS_TMU_CALIB_SEL_MASK;
				/* Check temp_error1 value */
				data->temp_error1 = trim_info & EXYNOS_TMU_TEMP_MASK;
				if (!data->temp_error1)
					data->temp_error1 = pdata->efuse_value & EXYNOS_TMU_TEMP_MASK;

				/* Check temp_error2 if calibration type is TYPE_TWO_POINT_TRIMMING */
				if(pdata->cal_type == TYPE_TWO_POINT_TRIMMING) {
					data->temp_error2 = (trim_info >> EXYNOS_TMU_TRIMINFO_85_P0_SHIFT) & EXYNOS_TMU_TEMP_MASK;

					if (!data->temp_error2)
						data->temp_error2 = (pdata->efuse_value >> EXYNOS_TMU_TRIMINFO_85_P0_SHIFT)
								& EXYNOS_TMU_TEMP_MASK;
				}
			} else {
				/* Check thermal calibration type */
				data->remote_sensors[i].cal_type = (trim_info >> EXYNOS_TMU_CALIB_SEL_SHIFT)
								& EXYNOS_TMU_CALIB_SEL_MASK;
				/* Check temp_error1 value */
				data->remote_sensors[i].temp_error1 = trim_info & EXYNOS_TMU_TEMP_MASK;
				if (!data->remote_sensors[i].temp_error1)
					data->remote_sensors[i].temp_error1 = pdata->efuse_value & EXYNOS_TMU_TEMP_MASK;

				/* Check temp_error2 if calibration type is TYPE_TWO_POINT_TRIMMING */
				if(pdata->cal_type == TYPE_TWO_POINT_TRIMMING) {
					data->remote_sensors[i].temp_error2 = (trim_info >> EXYNOS_TMU_TRIMINFO_85_P0_SHIFT) & EXYNOS_TMU_TEMP_MASK;
					if (!data->remote_sensors[i].temp_error2)
						data->remote_sensors[i].temp_error2 = (pdata->efuse_value >> EXYNOS_TMU_TRIMINFO_85_P0_SHIFT)
								& EXYNOS_TMU_TEMP_MASK;
				}
			}
		}
 	}

	/* If the governor is power allocator, we ignore interrupt and don't update thermal zone
	   Even though we don't control it, thermal framework can handle it by polling.
	 */
	if (strcmp(tz->tzp->governor_name, "power_allocator")) {
		for (j = 0; j < TOTAL_SENSORS; j++) {
			if (data->sensors & (1 << j)) {
				/* Write temperature code for rising and falling threshold */
				for (i = (of_thermal_get_ntrips(tz) - 1); i >= 0; i--) {
					/*
					 * On exynos8 there are 4 rising and 4 falling threshold
					 * registers (0x50-0x5c and 0x60-0x6c respectively). Each
					 * register holds the value of two threshold levels (at bit
					 * offsets 0 and 16). Based on the fact that there are atmost
					 * eight possible trigger levels, calculate the register and
					 * bit offsets where the threshold levels are to be written.
					 *
					 * e.g. EXYNOS_THD_TEMP_RISE7_6 (0x50)
					 * [24:16] - Threshold level 7
					 * [8:0] - Threshold level 6
					 * e.g. EXYNOS_THD_TEMP_RISE5_4 (0x54)
					 * [24:16] - Threshold level 5
					 * [8:0] - Threshold level 4
					 *
					 * and similarly for falling thresholds.
					 *
					 * Based on the above, calculate the register and bit offsets
					 * for rising/falling threshold levels and populate them.
					 */
					reg_off = ((7 - i) / 2) * 4;
					bit_off = ((8 - i) % 2);

					if (j > 0)
						reg_off = reg_off + EXYNOS_THD_TEMP_R_OFFSET;

					tz->ops->get_trip_temp(tz, i, &temp);
					temp /= MCELSIUS;

					tz->ops->get_trip_hyst(tz, i, &temp_hist);
					temp_hist = temp - (temp_hist / MCELSIUS);

					/* Set 9-bit temperature code for rising threshold levels */
					threshold_code = temp_to_code(data, temp);
					rising_threshold = readl(data->base +
						EXYNOS_THD_TEMP_RISE7_6 + reg_off);
					rising_threshold &= ~(EXYNOS_TMU_TEMP_MASK << (16 * bit_off));
					rising_threshold |= threshold_code << (16 * bit_off);
					writel(rising_threshold,
					       data->base + EXYNOS_THD_TEMP_RISE7_6 + reg_off);

					/* Set 9-bit temperature code for falling threshold levels */
					threshold_code = temp_to_code(data, temp_hist);
					falling_threshold &= ~(EXYNOS_TMU_TEMP_MASK << (16 * bit_off));
					falling_threshold |= threshold_code << (16 * bit_off);
					writel(falling_threshold,
					       data->base + EXYNOS_THD_TEMP_FALL7_6 + reg_off);
				}
			}
		}
	}

	data->tmu_clear_irqs(data);

	return 0;
}

static void exynos8895_tmu_control(struct platform_device *pdev, bool on)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct thermal_zone_device *tz = data->tzd;
	unsigned int con, interrupt_en, trim_info, trim_info1, trim_info2;
	unsigned int t_buf_vref_sel, t_buf_slope_sel;
	int i;
	u32 avg_con, avg_sel;

	con = readl(data->base + EXYNOS_TMU_REG_CONTROL);
	con &= ~(1 << EXYNOS_TMU_CORE_EN_SHIFT);
	con &= ~(1 << EXYNOS_TMU_THERM_TRIP_EN_SHIFT);
	writel(con, data->base + EXYNOS_TMU_REG_CONTROL);
	con = 0;

	trim_info = readl(data->base + EXYNOS_TMU_REG_TRIMINFO);
	trim_info1 = readl(data->base + EXYNOS_TMU_REG_TRIMINFO1);
	trim_info2 = readl(data->base + EXYNOS_TMU_REG_TRIMINFO2);

	/* Save fuse buf_vref_sel, calib_sel value to TRIMINFO and 1 register */
	t_buf_vref_sel = (trim_info >> EXYNOS_TMU_T_BUF_VREF_SEL_SHIFT)
				& (EXYNOS_TMU_T_BUF_VREF_SEL_MASK);
	t_buf_slope_sel = (trim_info1 >> EXYNOS_TMU_T_BUF_SLOPE_SEL_SHIFT)
				& (EXYNOS_TMU_T_BUF_SLOPE_SEL_MASK);
	avg_sel = (trim_info2 >> EXYNOS_TMU_AVG_CON_SHIFT) & EXYNOS_TMU_AVG_CON_MASK;

	con = get_con_reg(data, readl(data->base + EXYNOS_TMU_REG_CONTROL));
	avg_con = readl(data->base + EXYNOS_TMU_REG_AVG_CON);

	if(avg_sel)
		avg_con |= ((avg_con & EXYNOS_TMU_AVG_MODE_MASK) | EXYNOS_TMU_AVG_MODE_DEFAULT);
	else
		avg_con |= ((avg_con & EXYNOS_TMU_AVG_MODE_MASK) | EXYNOS_TMU_AVG_MODE_4);

	if (on) {
		con |= (t_buf_vref_sel << EXYNOS_TMU_REF_VOLTAGE_SHIFT);
		con |= (t_buf_slope_sel << EXYNOS_TMU_BUF_SLOPE_SEL_SHIFT);
		con |= (1 << EXYNOS_TMU_CORE_EN_SHIFT);
		con |= (1 << EXYNOS_TMU_THERM_TRIP_EN_SHIFT);
		interrupt_en =
			(of_thermal_is_trip_valid(tz, 7)
			<< EXYNOS_TMU_INTEN_RISE7_SHIFT) |
			(of_thermal_is_trip_valid(tz, 6)
			<< EXYNOS_TMU_INTEN_RISE6_SHIFT) |
			(of_thermal_is_trip_valid(tz, 5)
			<< EXYNOS_TMU_INTEN_RISE5_SHIFT) |
			(of_thermal_is_trip_valid(tz, 4)
			<< EXYNOS_TMU_INTEN_RISE4_SHIFT) |
			(of_thermal_is_trip_valid(tz, 3)
			<< EXYNOS_TMU_INTEN_RISE3_SHIFT) |
			(of_thermal_is_trip_valid(tz, 2)
			<< EXYNOS_TMU_INTEN_RISE2_SHIFT) |
			(of_thermal_is_trip_valid(tz, 1)
			<< EXYNOS_TMU_INTEN_RISE1_SHIFT) |
			(of_thermal_is_trip_valid(tz, 0)
			<< EXYNOS_TMU_INTEN_RISE0_SHIFT);

		interrupt_en |=
			interrupt_en << EXYNOS_TMU_INTEN_FALL0_SHIFT;
	} else {
		con &= ~(1 << EXYNOS_TMU_CORE_EN_SHIFT);
		con &= ~(1 << EXYNOS_TMU_THERM_TRIP_EN_SHIFT);
		interrupt_en = 0; /* Disable all interrupts */
	}

	if (strcmp(tz->tzp->governor_name, "power_allocator")) {
		for (i = 0; i < TOTAL_SENSORS; i++) {
			if (data->sensors & (1 << i)) {
				writel(interrupt_en, data->base + EXYNOS_TMU_REG_INTEN + 0x10 * i);
			}
		}
	}
	writel(con, data->base + EXYNOS_TMU_REG_CONTROL);
	writel(avg_con, data->base + EXYNOS_TMU_REG_AVG_CON);
}
static int exynos_get_temp(void *p, int *temp)
{
	struct exynos_tmu_data *data = p;
	struct thermal_cooling_device *cdev;

	if (!data || !data->tmu_read)
		return -EINVAL;

	mutex_lock(&data->lock);

	*temp = code_to_temp(data, data->tmu_read(data)) * MCELSIUS;

	mutex_unlock(&data->lock);

	cdev = data->cool_dev;

	if (!cdev)
		return 0;

	mutex_lock(&thermal_suspend_lock);

	if (cdev->ops->set_cur_temp && data->id != 1)
		cdev->ops->set_cur_temp(cdev, suspended, *temp / 1000);

	mutex_unlock(&thermal_suspend_lock);

	return 0;
}

#ifdef CONFIG_THERMAL_EMULATION
static u32 get_emul_con_reg(struct exynos_tmu_data *data, unsigned int val,
			    int temp)
{
	if (temp) {
		temp /= MCELSIUS;
		val &= ~(EXYNOS_EMUL_DATA_MASK <<
			EXYNOS_EMUL_DATA_SHIFT);
		val |= (temp_to_code(data, temp) <<
			EXYNOS_EMUL_DATA_SHIFT) |
			EXYNOS_EMUL_ENABLE;
	} else {
		val &= ~EXYNOS_EMUL_ENABLE;
	}

	return val;
}

static void exynos8890_tmu_set_emulation(struct exynos_tmu_data *data,
					 int temp)
{
	unsigned int val;
	u32 emul_con;

	emul_con = EXYNOS_TMU_REG_EMUL_CON;

	val = readl(data->base + emul_con);
	val = get_emul_con_reg(data, val, temp);
	writel(val, data->base + emul_con);
}

static int exynos_tmu_set_emulation(void *drv_data, int temp)
{
	struct exynos_tmu_data *data = drv_data;
	int ret = -EINVAL;

	if (temp && temp < MCELSIUS)
		goto out;

	mutex_lock(&data->lock);
	data->tmu_set_emulation(data, temp);
	mutex_unlock(&data->lock);
	return 0;
out:
	return ret;
}
#else
#define exynos8890_tmu_set_emulation NULL
static int exynos_tmu_set_emulation(void *drv_data, int temp)
	{ return -EINVAL; }
#endif /* CONFIG_THERMAL_EMULATION */

static int exynos8890_tmu_read(struct exynos_tmu_data *data)
{
	return readw(data->base + EXYNOS_TMU_REG_CURRENT_TEMP1_0) &
		EXYNOS_TMU_TEMP_MASK;
}

static int exynos8895_tmu_read(struct exynos_tmu_data *data)
{
	int i;
	u32 reg_offset, bit_offset;
	u32 temp_data[TOTAL_SENSORS];
	u32 count = 0, result = 0;

	for (i = 0; i < TOTAL_SENSORS; i++) {
		if (data->sensors & (1 << i)) {
			if (i < 2) {
				reg_offset = 0;
				bit_offset = EXYNOS_TMU_TEMP_SHIFT * i;
			} else {
				reg_offset = ((i - 2) / 3 + 1) * 4;
				bit_offset = EXYNOS_TMU_TEMP_SHIFT * ((i - 2) % 3);
			}

			temp_data[i] = (readl(data->base + EXYNOS_TMU_REG_CURRENT_TEMP1_0 + reg_offset)
					>> bit_offset) & EXYNOS_TMU_TEMP_MASK;
			count++;

			switch (data->sensing_mode) {
				case AVG : result = result + temp_data[i];
					break;
				case MAX : result = result > temp_data[i] ? result : temp_data[i];
					break;
				case MIN : result = result < temp_data[i] ? result : temp_data[i];
					break;
				default : result = temp_data[i];
					break;
			}
		}
	}

	switch (data->sensing_mode) {
		case AVG : result = result / count;
			break;
		case MAX :
		case MIN :
		default :
			break;
	}

	return result;
	//return readw(data->base + EXYNOS_TMU_REG_CURRENT_TEMP1_0) & EXYNOS_TMU_TEMP_MASK;
}

static void exynos_tmu_work(struct work_struct *work)
{
	struct exynos_tmu_data *data = container_of(work,
			struct exynos_tmu_data, irq_work);

	exynos_report_trigger(data);
	mutex_lock(&data->lock);

	/* TODO: take action based on particular interrupt */
	data->tmu_clear_irqs(data);

	mutex_unlock(&data->lock);
	enable_irq(data->irq);
}

static void exynos8890_tmu_clear_irqs(struct exynos_tmu_data *data)
{
	unsigned int val_irq;

	val_irq = readl(data->base + EXYNOS_TMU_REG_INTPEND0);
	writel(val_irq, data->base + EXYNOS_TMU_REG_INTPEND0);
}

static void exynos8895_tmu_clear_irqs(struct exynos_tmu_data *data)
{
	unsigned int i, val_irq;
	u32 pend_reg;

	for (i = 0; i < TOTAL_SENSORS; i++) {
		if (data->sensors & (1 << i)) {
			if (i < 5)
				pend_reg = EXYNOS_TMU_REG_INTPEND0 + EXYNOS_TMU_REG_INTPEN_OFFSET * i;
			else
				pend_reg = EXYNOS_TMU_REG_INTPEND5 + EXYNOS_TMU_REG_INTPEN_OFFSET * i;

			val_irq = readl(data->base + pend_reg);
			writel(val_irq, data->base + pend_reg);
		}
	}
}

static irqreturn_t exynos_tmu_irq(int irq, void *id)
{
	struct exynos_tmu_data *data = id;

	disable_irq_nosync(irq);
	schedule_work(&data->irq_work);

	return IRQ_HANDLED;
}

static int exynos_pm_notifier(struct notifier_block *notifier,
			unsigned long event, void *v)
{
	struct exynos_tmu_data *devnode;
	struct thermal_cooling_device *cdev;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		mutex_lock(&thermal_suspend_lock);
		suspended = true;
		list_for_each_entry(devnode, &dtm_dev_list, node) {
			cdev = devnode->cool_dev;

			if (cdev && cdev->ops->set_cur_temp && devnode->id != 1)
				cdev->ops->set_cur_temp(cdev, suspended, 0);
		}
		mutex_unlock(&thermal_suspend_lock);
		break;
	case PM_POST_SUSPEND:
		mutex_lock(&thermal_suspend_lock);
		suspended = false;
		mutex_unlock(&thermal_suspend_lock);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block exynos_tmu_pm_notifier = {
	.notifier_call = exynos_pm_notifier,
};

static const struct of_device_id exynos_tmu_match[] = {
	{ .compatible = "samsung,exynos8890-tmu", },
	{ .compatible = "samsung,exynos8895-tmu", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, exynos_tmu_match);

static int exynos_of_get_soc_type(struct device_node *np)
{
	if (of_device_is_compatible(np, "samsung,exynos8890-tmu"))
		return SOC_ARCH_EXYNOS8890;
	if (of_device_is_compatible(np, "samsung,exynos8895-tmu"))
		return SOC_ARCH_EXYNOS8895;

	return -EINVAL;
}

static int exynos_of_sensor_conf(struct device_node *np,
				 struct exynos_tmu_platform_data *pdata)
{
	u32 value;
	int ret;

	of_node_get(np);

	ret = of_property_read_u32(np, "samsung,tmu_gain", &value);
	pdata->gain = (u8)value;
	of_property_read_u32(np, "samsung,tmu_reference_voltage", &value);
	pdata->reference_voltage = (u8)value;
	of_property_read_u32(np, "samsung,tmu_noise_cancel_mode", &value);
	pdata->noise_cancel_mode = (u8)value;

	of_property_read_u32(np, "samsung,tmu_efuse_value",
			     &pdata->efuse_value);

	of_property_read_u32(np, "samsung,tmu_first_point_trim", &value);
	pdata->first_point_trim = (u8)value;
	of_property_read_u32(np, "samsung,tmu_second_point_trim", &value);
	pdata->second_point_trim = (u8)value;
	of_property_read_u32(np, "samsung,tmu_default_temp_offset", &value);
	pdata->default_temp_offset = (u8)value;

	of_property_read_u32(np, "samsung,tmu_cal_type", &pdata->cal_type);

	of_node_put(np);
	return 0;
}

static int exynos_map_dt_data(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct exynos_tmu_platform_data *pdata;
	struct resource res;
	int i;
	const char *temp, *tmu_name;

	if (!data || !pdev->dev.of_node)
		return -ENODEV;

	data->np = pdev->dev.of_node;

	if (of_property_read_u32(pdev->dev.of_node, "id", &data->id)) {
		dev_err(&pdev->dev, "failed to get TMU ID\n");
		return -ENODEV;
	}

	data->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (data->irq <= 0) {
		dev_err(&pdev->dev, "failed to get IRQ\n");
		return -ENODEV;
	}

	if (of_address_to_resource(pdev->dev.of_node, 0, &res)) {
		dev_err(&pdev->dev, "failed to get Resource 0\n");
		return -ENODEV;
	}

	data->base = devm_ioremap(&pdev->dev, res.start, resource_size(&res));
	if (!data->base) {
		dev_err(&pdev->dev, "Failed to ioremap memory\n");
		return -EADDRNOTAVAIL;
	}

	/* If remote sensor is exist, parse it. Remote sensor is used when reading the temperature. */
	if (!of_property_read_u32(pdev->dev.of_node, "sensors", &data->sensors)) {
		for (i = 1; i < 8; i++) {
			if (data->sensors & (1 << i))
				data->num_of_remotes++;
		}

		data->remote_sensors = kzalloc(sizeof(struct remote_sensor_info) * data->num_of_remotes, GFP_KERNEL);
	} else {
		dev_err(&pdev->dev, "failed to get sensors information \n");
		return -ENODEV;
	}

	of_property_read_string(pdev->dev.of_node, "sensing_method", &temp);

	if (of_property_read_string(pdev->dev.of_node, "tmu_name", &tmu_name)) {
		dev_err(&pdev->dev, "failed to get tmu_name\n");
	} else
		strncpy(data->tmu_name, tmu_name, THERMAL_NAME_LENGTH);

	for (i = 0; i<ARRAY_SIZE(sensing_method); i++)
		if (!strcasecmp(temp, sensing_method[i]))
			data->sensing_mode = i;

	data->hotplug_enable = of_property_read_bool(pdev->dev.of_node, "hotplug_enable");
	if (data->hotplug_enable) {
		dev_info(&pdev->dev, "thermal zone use hotplug function \n");
		of_property_read_u32(pdev->dev.of_node, "hotplug_in_threshold",
					&data->hotplug_in_threshold);
		if (!data->hotplug_in_threshold)
			dev_err(&pdev->dev, "No input hotplug_in_threshold \n");

		of_property_read_u32(pdev->dev.of_node, "hotplug_out_threshold",
					&data->hotplug_out_threshold);
		if (!data->hotplug_out_threshold)
			dev_err(&pdev->dev, "No input hotplug_out_threshold \n");
	}

	pdata = devm_kzalloc(&pdev->dev, sizeof(struct exynos_tmu_platform_data), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	exynos_of_sensor_conf(pdev->dev.of_node, pdata);
	data->pdata = pdata;
	data->soc = exynos_of_get_soc_type(pdev->dev.of_node);

	switch (data->soc) {
	case SOC_ARCH_EXYNOS8890:
		data->tmu_initialize = exynos8890_tmu_initialize;
		data->tmu_control = exynos8890_tmu_control;
		data->tmu_read = exynos8890_tmu_read;
		data->tmu_set_emulation = exynos8890_tmu_set_emulation;
		data->tmu_clear_irqs = exynos8890_tmu_clear_irqs;
		break;
	case SOC_ARCH_EXYNOS8895:
		data->tmu_initialize = exynos8895_tmu_initialize;
		data->tmu_control = exynos8895_tmu_control;
		data->tmu_read = exynos8895_tmu_read;
		data->tmu_set_emulation = exynos8890_tmu_set_emulation;
		data->tmu_clear_irqs = exynos8895_tmu_clear_irqs;
		break;

	default:
		dev_err(&pdev->dev, "Platform not supported\n");
		return -EINVAL;
	}

	return 0;
}
#ifdef CONFIG_GPU_THERMAL
static int gpu_cooling_table_init(struct platform_device *pdev)
{
	struct cpufreq_frequency_table *table_ptr;
	unsigned int table_size;
	u32 gpu_idx_num = 0;
	int ret = 0, i = 0;

	/* gpu cooling frequency table parse */
	ret = of_property_read_u32(pdev->dev.of_node, "gpu_idx_num",
					&gpu_idx_num);
	if (ret < 0)
		dev_err(&pdev->dev, "gpu_idx_num happend error value\n");

	if (gpu_idx_num) {
		table_ptr = kzalloc(sizeof(struct cpufreq_frequency_table)
						* gpu_idx_num, GFP_KERNEL);
		if (!table_ptr) {
			dev_err(&pdev->dev, "failed to allocate for gpu_table\n");
			return -ENODEV;
		}
		table_size = sizeof(struct cpufreq_frequency_table) /
							sizeof(unsigned int);
		ret = of_property_read_u32_array(pdev->dev.of_node, "gpu_cooling_table",
			(unsigned int *)table_ptr, table_size * gpu_idx_num);

		for (i = 0; i < gpu_idx_num; i++) {
			gpu_freq_table[i].flags = table_ptr[i].flags;
			gpu_freq_table[i].driver_data = table_ptr[i].driver_data;
			gpu_freq_table[i].frequency = table_ptr[i].frequency;
			dev_info(&pdev->dev, "[GPU TMU] index : %d, frequency : %d \n",
				gpu_freq_table[i].driver_data, gpu_freq_table[i].frequency);
		}
		kfree(table_ptr);
	}
	return ret;
}
#else
static int gpu_cooling_table_init(struct platform_device *pdev) {return 0;}
#endif

#ifdef CONFIG_ISP_THERMAL
struct isp_fps_table isp_fps_table[10];

static int isp_cooling_table_init(struct platform_device *pdev)
{
	struct isp_fps_table *table_ptr;
	unsigned int table_size;
	u32 isp_idx_num = 0;
	int ret = 0, i = 0;

	/* isp cooling frequency table parse */
	ret = of_property_read_u32(pdev->dev.of_node, "isp_idx_num",
					&isp_idx_num);
	if (ret < 0)
		dev_err(&pdev->dev, "isp_idx_num happend error value\n");

	if (isp_idx_num) {
		table_ptr = kzalloc(sizeof(struct isp_fps_table)
						* isp_idx_num, GFP_KERNEL);
		if (!table_ptr) {
			dev_err(&pdev->dev, "failed to allocate for isp_table\n");
			return -ENODEV;
		}
		table_size = sizeof(struct isp_fps_table) / sizeof(unsigned int);
		ret = of_property_read_u32_array(pdev->dev.of_node, "isp_cooling_table",
			(unsigned int *)table_ptr, table_size * isp_idx_num);

		for (i = 0; i < isp_idx_num; i++) {
			isp_fps_table[i].flags = table_ptr[i].flags;
			isp_fps_table[i].driver_data = table_ptr[i].driver_data;
			isp_fps_table[i].fps = table_ptr[i].fps;
			dev_info(&pdev->dev, "[ISP TMU] index : %d, fps : %d \n",
				isp_fps_table[i].driver_data, isp_fps_table[i].fps);
		}
		kfree(table_ptr);
	}
	return ret;
}
#else
static int isp_cooling_table_init(struct platform_device *pdev) {return 0;}
#endif

struct pm_qos_request thermal_cpu_hotplug_request;
static int exynos_throttle_cpu_hotplug(void *p, int temp)
{
	struct exynos_tmu_data *data = p;
	struct cpufreq_cooling_device *cpufreq_device = (struct cpufreq_cooling_device *)data->cool_dev->devdata;
	int ret = 0;

	temp = temp / MCELSIUS;

	if (is_cpu_hotplugged_out) {
		if (temp < data->hotplug_in_threshold) {
			/*
			 * If current temperature is lower than low threshold,
			 * call cluster1_cores_hotplug(false) for hotplugged out cpus.
			 */
			pm_qos_update_request(&thermal_cpu_hotplug_request,
						NR_CPUS);
			is_cpu_hotplugged_out = false;
			cpufreq_device->cpufreq_state = 0;
		}
	} else {
		if (temp >= data->hotplug_out_threshold) {
			/*
			 * If current temperature is higher than high threshold,
			 * call cluster1_cores_hotplug(true) to hold temperature down.
			 */
			is_cpu_hotplugged_out = true;

			pm_qos_update_request(&thermal_cpu_hotplug_request,
						NR_HOTPLUG_CPUS);
		}
	}

	return ret;
}

static struct thermal_zone_of_device_ops exynos_hotplug_sensor_ops = {
	.get_temp = exynos_get_temp,
	.set_emul_temp = exynos_tmu_set_emulation,
	.throttle_cpu_hotplug = exynos_throttle_cpu_hotplug,
};

static struct thermal_zone_of_device_ops exynos_sensor_ops = {
	.get_temp = exynos_get_temp,
	.set_emul_temp = exynos_tmu_set_emulation,
};

static int exynos_cpufreq_cooling_register(struct exynos_tmu_data *data)
{
	struct device_node *np, *child = NULL, *gchild, *ggchild;
	struct device_node *cool_np;
	struct of_phandle_args cooling_spec;
	struct cpumask mask_val;
	int cpu, ret;
	const char *governor_name;
	u32 power_coefficient = 0;

	np = of_find_node_by_name(NULL, "thermal-zones");
	if (!np)
		return -ENODEV;

	/* Register cpufreq cooling device */
	for_each_child_of_node(np, child) {
		struct device_node *zone_np;
		zone_np = of_parse_phandle(child, "thermal-sensors", 0);

		if (zone_np == data->np) break;
	}

	gchild = of_get_child_by_name(child, "cooling-maps");
	ggchild = of_get_next_child(gchild, NULL);
	ret = of_parse_phandle_with_args(ggchild, "cooling-device", "#cooling-cells",
					 0, &cooling_spec);
	if (ret < 0)
		pr_err("%s do not get cooling spec(err = %d) \n", data->tmu_name, ret);

	cool_np = cooling_spec.np;

	for_each_possible_cpu(cpu)
		if (cpu_topology[cpu].cluster_id == data->id)
			cpumask_copy(&mask_val, topology_core_cpumask(cpu));

	if (!of_property_read_string(child, "governor", &governor_name)) {
		if (!strncasecmp(governor_name, "power_allocator", THERMAL_NAME_LENGTH)) {
			of_property_read_u32(cool_np, "dynamic-power-coefficient", &power_coefficient);
		}
	}

	data->cool_dev = of_cpufreq_power_cooling_register(cool_np, &mask_val, power_coefficient, NULL);

	if (IS_ERR(data->cool_dev))
	        pr_err("cooling device register fail (mask = %x) \n", *(unsigned int*)cpumask_bits(&mask_val));

	return ret;
}

#ifdef CONFIG_GPU_THERMAL
static int exynos_gpufreq_cooling_register(struct exynos_tmu_data *data)
{
	struct device_node *np, *child = NULL, *gchild, *ggchild;
	struct device_node *cool_np;
	struct of_phandle_args cooling_spec;
	int ret;

	np = of_find_node_by_name(NULL, "thermal-zones");
	if (!np)
		return -ENODEV;

	/* Regist gpufreq cooling device */
	for_each_child_of_node(np, child) {
		struct device_node *zone_np;
		zone_np = of_parse_phandle(child, "thermal-sensors", 0);

		if (zone_np == data->np) break;
	}

	gchild = of_get_child_by_name(child, "cooling-maps");
	ggchild = of_get_next_child(gchild, NULL);
	ret = of_parse_phandle_with_args(ggchild, "cooling-device", "#cooling-cells",
					 0, &cooling_spec);
	if (ret < 0)
		pr_err("%s do not get cooling spec(err = %d) \n", data->tmu_name, ret);

	cool_np = cooling_spec.np;

	data->cool_dev = of_gpufreq_cooling_register(cool_np, NULL);

	return ret;
}
#else
static int exynos_gpufreq_cooling_register(struct exynos_tmu_data *data) {return 0;}
#endif

#ifdef CONFIG_ISP_THERMAL
static int exynos_isp_cooling_register(struct exynos_tmu_data *data)
{
	struct device_node *np, *child = NULL, *gchild, *ggchild;
	struct device_node *cool_np;
	struct of_phandle_args cooling_spec;
	int ret;

	np = of_find_node_by_name(NULL, "thermal-zones");
	if (!np)
		return -ENODEV;

	/* Regist isp cooling device */
	for_each_child_of_node(np, child) {
		struct device_node *zone_np;
		zone_np = of_parse_phandle(child, "thermal-sensors", 0);

		if (zone_np == data->np) break;
	}

	gchild = of_get_child_by_name(child, "cooling-maps");
	ggchild = of_get_next_child(gchild, NULL);
	ret = of_parse_phandle_with_args(ggchild, "cooling-device", "#cooling-cells",
					 0, &cooling_spec);
	if (ret < 0)
		pr_err("%s do not get cooling spec(err = %d) \n", data->tmu_name, ret);

	cool_np = cooling_spec.np;

	data->cool_dev = of_isp_cooling_register(cool_np, NULL);

	return ret;
}
#else
static int exynos_isp_cooling_register(struct exynos_tmu_data *data) {return 0;}
#endif

static int exynos_tmu_probe(struct platform_device *pdev)
{
	struct exynos_tmu_data *data;
	int ret;
#ifdef CONFIG_CPU_FREQ
	if (!cpufreq_frequency_get_table(0))
		return -EPROBE_DEFER;
#endif

	data = devm_kzalloc(&pdev->dev, sizeof(struct exynos_tmu_data),
					GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);
	mutex_init(&data->lock);

	ret = exynos_map_dt_data(pdev);
	if (ret)
		goto err_sensor;

	if (data->id == 0 || data->id == 1) {
		ret = exynos_cpufreq_cooling_register(data);
		if (ret) {
			dev_err(&pdev->dev, "Failed cooling register \n");
			goto err_sensor;
		}
	} else if (data->id == 2) {
		ret = gpu_cooling_table_init(pdev);
		if (ret)
			goto err_sensor;

		ret = exynos_gpufreq_cooling_register(data);
		if (ret) {
			dev_err(&pdev->dev, "Failed cooling register \n");
			goto err_sensor;
		}
	} else if (data->id == 3) {
		ret = isp_cooling_table_init(pdev);
		if (ret)
			goto err_sensor;

		ret = exynos_isp_cooling_register(data);
		if (ret) {
			dev_err(&pdev->dev, "Failed cooling register \n");
			goto err_sensor;
		}
	}

	INIT_WORK(&data->irq_work, exynos_tmu_work);

	/*
	 * data->tzd must be registered before calling exynos_tmu_initialize(),
	 * requesting irq and calling exynos_tmu_control().
	 */
	if(data->hotplug_enable)
		pm_qos_add_request(&thermal_cpu_hotplug_request,
					PM_QOS_CPU_ONLINE_MAX,
					PM_QOS_CPU_ONLINE_MAX_DEFAULT_VALUE);

	data->tzd = thermal_zone_of_sensor_register(&pdev->dev, 0, data,
						    data->hotplug_enable ?
						    &exynos_hotplug_sensor_ops :
						    &exynos_sensor_ops);
	if (IS_ERR(data->tzd)) {
		ret = PTR_ERR(data->tzd);
		dev_err(&pdev->dev, "Failed to register sensor: %d\n", ret);
		goto err_sensor;
	}

	ret = exynos_tmu_initialize(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize TMU\n");
		goto err_thermal;
	}

	ret = devm_request_irq(&pdev->dev, data->irq, exynos_tmu_irq,
				IRQF_SHARED, dev_name(&pdev->dev), data);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq: %d\n", data->irq);
		goto err_thermal;
	}

	exynos_tmu_control(pdev, true);

	mutex_lock(&data->lock);
	list_add_tail(&data->node, &dtm_dev_list);
	mutex_unlock(&data->lock);

	if (list_is_singular(&dtm_dev_list))
		register_pm_notifier(&exynos_tmu_pm_notifier);

	if (!IS_ERR(data->tzd))
		data->tzd->ops->set_mode(data->tzd, THERMAL_DEVICE_ENABLED);

	return 0;

err_thermal:
	thermal_zone_of_sensor_unregister(&pdev->dev, data->tzd);
err_sensor:
	return ret;
}

static int exynos_tmu_remove(struct platform_device *pdev)
{
	struct exynos_tmu_data *data = platform_get_drvdata(pdev);
	struct thermal_zone_device *tzd = data->tzd;
	struct exynos_tmu_data *devnode;

	if (list_is_singular(&dtm_dev_list))
		unregister_pm_notifier(&exynos_tmu_pm_notifier);

	thermal_zone_of_sensor_unregister(&pdev->dev, tzd);
	exynos_tmu_control(pdev, false);

	mutex_lock(&data->lock);
	list_for_each_entry(devnode, &dtm_dev_list, node) {
		if (devnode->id == data->id) {
			list_del(&devnode->node);
		}
	}
	mutex_unlock(&data->lock);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int exynos_tmu_suspend(struct device *dev)
{
	exynos_tmu_control(to_platform_device(dev), false);

	return 0;
}

static int exynos_tmu_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);

	exynos_tmu_initialize(pdev);
	exynos_tmu_control(pdev, true);

	return 0;
}

static SIMPLE_DEV_PM_OPS(exynos_tmu_pm,
			 exynos_tmu_suspend, exynos_tmu_resume);
#define EXYNOS_TMU_PM	(&exynos_tmu_pm)
#else
#define EXYNOS_TMU_PM	NULL
#endif

static struct platform_driver exynos_tmu_driver = {
	.driver = {
		.name   = "exynos-tmu",
		.pm     = EXYNOS_TMU_PM,
		.of_match_table = exynos_tmu_match,
	},
	.probe = exynos_tmu_probe,
	.remove	= exynos_tmu_remove,
};

module_platform_driver(exynos_tmu_driver);

MODULE_DESCRIPTION("EXYNOS TMU Driver");
MODULE_AUTHOR("Donggeun Kim <dg77.kim@samsung.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:exynos-tmu");
