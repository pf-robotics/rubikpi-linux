// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Invensense, Inc.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger_consumer.h>

#include "inv_icm42600.h"
#include "inv_icm42600_buffer.h"

static const struct regmap_range_cfg inv_icm42600_regmap_ranges[] = {
	{
		.name = "user banks",
		.range_min = 0x0000,
		.range_max = 0x4FFF,
		.selector_reg = INV_ICM42600_REG_BANK_SEL,
		.selector_mask = INV_ICM42600_BANK_SEL_MASK,
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 0x1000,
	},
};

const struct regmap_config inv_icm42600_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x4FFF,
	.ranges = inv_icm42600_regmap_ranges,
	.num_ranges = ARRAY_SIZE(inv_icm42600_regmap_ranges),
};
EXPORT_SYMBOL_NS_GPL(inv_icm42600_regmap_config, IIO_ICM42600);

struct inv_icm42600_hw {
	uint8_t whoami;
	const char *name;
	const struct inv_icm42600_conf *conf;
};

/* chip initial default configuration */
static const struct inv_icm42600_conf inv_icm42600_default_conf = {
	.gyro = {
		.mode = INV_ICM42600_SENSOR_MODE_OFF,
		.fs = INV_ICM42600_GYRO_FS_2000DPS,
		.odr = INV_ICM42600_ODR_50HZ,
		.filter = INV_ICM42600_FILTER_BW_ODR_DIV_2,
	},
	.accel = {
		.mode = INV_ICM42600_SENSOR_MODE_OFF,
		.fs = INV_ICM42600_ACCEL_FS_16G,
		.odr = INV_ICM42600_ODR_50HZ,
		.filter = INV_ICM42600_FILTER_BW_ODR_DIV_2,
	},
	.temp_en = false,
};

static const struct inv_icm42600_conf inv_icm42670_default_conf = {
	.gyro = {
		.mode = INV_ICM42600_SENSOR_MODE_LOW_NOISE,
		.fs = INV_ICM42600_GYRO_FS_2000DPS,
		.odr = INV_ICM42670_ODR_200HZ,
		.filter = INV_ICM42600_FILTER_BW_ODR_DIV_2,
	},
	.accel = {
		.mode = INV_ICM42600_SENSOR_MODE_LOW_NOISE,
		.fs = INV_ICM42600_ACCEL_FS_16G,
		.odr = INV_ICM42670_ODR_200HZ,
		.filter = INV_ICM42600_FILTER_BW_ODR_DIV_2,
	},
	.temp_en = false,
};

static const struct inv_icm42600_hw inv_icm42600_hw[INV_CHIP_NB] = {
	[INV_CHIP_ICM42600] = {
		.whoami = INV_ICM42600_WHOAMI_ICM42600,
		.name = "icm42600",
		.conf = &inv_icm42600_default_conf,
	},
	[INV_CHIP_ICM42602] = {
		.whoami = INV_ICM42600_WHOAMI_ICM42602,
		.name = "icm42602",
		.conf = &inv_icm42600_default_conf,
	},
	[INV_CHIP_ICM42605] = {
		.whoami = INV_ICM42600_WHOAMI_ICM42605,
		.name = "icm42605",
		.conf = &inv_icm42600_default_conf,
	},
	[INV_CHIP_ICM42622] = {
		.whoami = INV_ICM42600_WHOAMI_ICM42622,
		.name = "icm42622",
		.conf = &inv_icm42600_default_conf,
	},
	[INV_CHIP_ICM42631] = {
		.whoami = INV_ICM42600_WHOAMI_ICM42631,
		.name = "icm42631",
		.conf = &inv_icm42600_default_conf,
	},
	[INV_CHIP_ICM42670] = {
		.whoami = INV_ICM42600_WHOAMI_ICM42670,
		.name = "icm42670",
		.conf = &inv_icm42670_default_conf,
	},
};

const struct iio_mount_matrix *
inv_icm42600_get_mount_matrix(const struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan)
{
	const struct inv_icm42600_state *st = iio_device_get_drvdata(indio_dev);

	return &st->orientation;
}

uint32_t inv_icm42600_odr_to_period(enum inv_icm42600_odr odr)
{
	static uint32_t odr_periods[INV_ICM42600_ODR_NB] = {
		/* reserved values */
		0, 0, 0,
		/* 8kHz */
		125000,
		/* 4kHz */
		250000,
		/* 2kHz */
		500000,
		/* 1kHz */
		1000000,
		/* 200Hz */
		5000000,
		/* 100Hz */
		10000000,
		/* 50Hz */
		20000000,
		/* 25Hz */
		40000000,
		/* 12.5Hz */
		80000000,
		/* 6.25Hz */
		160000000,
		/* 3.125Hz */
		320000000,
		/* 1.5625Hz */
		640000000,
		/* 500Hz */
		2000000,
	};

	return odr_periods[odr];
}

static int inv_icm42600_set_pwr_mgmt0(struct inv_icm42600_state *st,
				      enum inv_icm42600_sensor_mode gyro,
				      enum inv_icm42600_sensor_mode accel,
				      bool temp, unsigned int *sleep_ms)
{
	enum inv_icm42600_sensor_mode oldgyro = st->conf.gyro.mode;
	enum inv_icm42600_sensor_mode oldaccel = st->conf.accel.mode;
	bool oldtemp = st->conf.temp_en;
	unsigned int sleepval;
	unsigned int val;
	int ret;
	/* if nothing changed, exit */
	if (gyro == oldgyro && accel == oldaccel && temp == oldtemp)
		return 0;

	val = INV_ICM42600_PWR_MGMT0_GYRO(gyro) |
	      INV_ICM42600_PWR_MGMT0_ACCEL(accel);
	ret = regmap_write(st->map, INV_ICM42670_REG_PWR_MGMT0, val);
	if (ret)
		return ret;
	st->conf.gyro.mode = gyro;
	st->conf.accel.mode = accel;
	st->conf.temp_en = temp;

	/* compute required wait time for sensors to stabilize */
	sleepval = 0;
	/* temperature stabilization time */
	if (temp && !oldtemp) {
		if (sleepval < INV_ICM42600_TEMP_STARTUP_TIME_MS)
			sleepval = INV_ICM42600_TEMP_STARTUP_TIME_MS;
	}
	/* accel startup time */
	if (accel != oldaccel && oldaccel == INV_ICM42600_SENSOR_MODE_OFF) {
		/* block any register write for at least 200 µs */
		usleep_range(200, 300);
		if (sleepval < INV_ICM42600_ACCEL_STARTUP_TIME_MS)
			sleepval = INV_ICM42600_ACCEL_STARTUP_TIME_MS;
	}
	if (gyro != oldgyro) {
		/* gyro startup time */
		if (oldgyro == INV_ICM42600_SENSOR_MODE_OFF) {
			/* block any register write for at least 200 µs */
			usleep_range(200, 300);
			if (sleepval < INV_ICM42600_GYRO_STARTUP_TIME_MS)
				sleepval = INV_ICM42600_GYRO_STARTUP_TIME_MS;
		/* gyro stop time */
		} else if (gyro == INV_ICM42600_SENSOR_MODE_OFF) {
			if (sleepval < INV_ICM42600_GYRO_STOP_TIME_MS)
				sleepval =  INV_ICM42600_GYRO_STOP_TIME_MS;
		}
	}

	/* deferred sleep value if sleep pointer is provided or direct sleep */
	if (sleep_ms)
		*sleep_ms = sleepval;
	else if (sleepval)
		msleep(sleepval);

	return 0;
}

int inv_icm42600_set_accel_conf(struct inv_icm42600_state *st,
				struct inv_icm42600_sensor_conf *conf,
				unsigned int *sleep_ms)
{
	struct inv_icm42600_sensor_conf *oldconf = &st->conf.accel;
	unsigned int val;
	int ret;

	/* Sanitize missing values with current values */
	if (conf->mode < 0)
		conf->mode = oldconf->mode;
	if (conf->fs < 0)
		conf->fs = oldconf->fs;
	if (conf->odr < 0)
		conf->odr = oldconf->odr;
	if (conf->filter < 0)
		conf->filter = oldconf->filter;

	/* set ACCEL_CONFIG0 register (accel fullscale & odr) */
	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = INV_ICM42600_ACCEL_CONFIG0_FS(conf->fs) |
		      INV_ICM42600_ACCEL_CONFIG0_ODR(conf->odr);
		ret = regmap_write(st->map, INV_ICM42600_REG_ACCEL_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	/* set GYRO_ACCEL_CONFIG0 register (accel filter) */
	if (conf->filter != oldconf->filter) {
		val = INV_ICM42600_GYRO_ACCEL_CONFIG0_ACCEL_FILT(conf->filter) |
		      INV_ICM42600_GYRO_ACCEL_CONFIG0_GYRO_FILT(st->conf.gyro.filter);
		ret = regmap_write(st->map, INV_ICM42600_REG_GYRO_ACCEL_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->filter = conf->filter;
	}

	/* set PWR_MGMT0 register (accel sensor mode) */
	return inv_icm42600_set_pwr_mgmt0(st, st->conf.gyro.mode, conf->mode,
					  st->conf.temp_en, sleep_ms);
}

int inv_icm42600_set_gyro_conf(struct inv_icm42600_state *st,
			       struct inv_icm42600_sensor_conf *conf,
			       unsigned int *sleep_ms)
{
	struct inv_icm42600_sensor_conf *oldconf = &st->conf.gyro;
	unsigned int val;
	int ret;

	/* sanitize missing values with current values */
	if (conf->mode < 0)
		conf->mode = oldconf->mode;
	if (conf->fs < 0)
		conf->fs = oldconf->fs;
	if (conf->odr < 0)
		conf->odr = oldconf->odr;
	if (conf->filter < 0)
		conf->filter = oldconf->filter;

	/* set GYRO_CONFIG0 register (gyro fullscale & odr) */
	if (conf->fs != oldconf->fs || conf->odr != oldconf->odr) {
		val = INV_ICM42600_GYRO_CONFIG0_FS(conf->fs) |
		      INV_ICM42600_GYRO_CONFIG0_ODR(conf->odr);
		ret = regmap_write(st->map, INV_ICM42600_REG_GYRO_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->fs = conf->fs;
		oldconf->odr = conf->odr;
	}

	/* set GYRO_ACCEL_CONFIG0 register (gyro filter) */
	if (conf->filter != oldconf->filter) {
		val = INV_ICM42600_GYRO_ACCEL_CONFIG0_ACCEL_FILT(st->conf.accel.filter) |
		      INV_ICM42600_GYRO_ACCEL_CONFIG0_GYRO_FILT(conf->filter);
		ret = regmap_write(st->map, INV_ICM42600_REG_GYRO_ACCEL_CONFIG0, val);
		if (ret)
			return ret;
		oldconf->filter = conf->filter;
	}

	/* set PWR_MGMT0 register (gyro sensor mode) */
	return inv_icm42600_set_pwr_mgmt0(st, conf->mode, st->conf.accel.mode,
					  st->conf.temp_en, sleep_ms);

	return 0;
}

int inv_icm42600_set_temp_conf(struct inv_icm42600_state *st, bool enable,
			       unsigned int *sleep_ms)
{
	return inv_icm42600_set_pwr_mgmt0(st, st->conf.gyro.mode,
					  st->conf.accel.mode, enable,
					  sleep_ms);
}

int inv_icm42600_debugfs_reg(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval)
{
	struct inv_icm42600_state *st = iio_device_get_drvdata(indio_dev);
	int ret;

	mutex_lock(&st->lock);

	if (readval)
		ret = regmap_read(st->map, reg, readval);
	else
		ret = regmap_write(st->map, reg, writeval);

	mutex_unlock(&st->lock);

	return ret;
}

static int inv_icm42600_set_conf(struct inv_icm42600_state *st,
				 const struct inv_icm42600_conf *conf)
{
	unsigned int val;
	int ret;

	/* set PWR_MGMT0 register (gyro & accel sensor mode, temp enabled) */
	val = INV_ICM42600_PWR_MGMT0_GYRO(conf->gyro.mode) |
	      INV_ICM42600_PWR_MGMT0_ACCEL(conf->accel.mode);
	ret = regmap_write(st->map, INV_ICM42670_REG_PWR_MGMT0, val);
	if (ret)
		return ret;

	/* set GYRO_CONFIG0 register (gyro fullscale & odr) */
	val = INV_ICM42670_GYRO_CONFIG0_FS(conf->gyro.fs) |
	      INV_ICM42670_GYRO_CONFIG0_ODR(conf->gyro.odr);
	ret = regmap_write(st->map, INV_ICM42670_REG_GYRO_CONFIG0, val);
	if (ret)
		return ret;

	/* set ACCEL_CONFIG0 register (accel fullscale & odr) */
	val = INV_ICM42670_ACCEL_CONFIG0_FS(conf->accel.fs) |
	      INV_ICM42670_ACCEL_CONFIG0_ODR(conf->accel.odr);
	ret = regmap_write(st->map, INV_ICM42670_REG_ACCEL_CONFIG0, val);
	if (ret)
		return ret;

	/* update internal conf */
	st->conf = *conf;

	return 0;
}

/**
 *  inv_icm42600_setup() - check and setup chip
 *  @st:	driver internal state
 *  @bus_setup:	callback for setting up bus specific registers
 *
 *  Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm42600_setup(struct inv_icm42600_state *st,
			      inv_icm42600_bus_setup bus_setup)
{
	const struct inv_icm42600_hw *hw = &inv_icm42600_hw[st->chip];
	const struct device *dev = regmap_get_device(st->map);
	unsigned int val;
	int ret;

	/* check chip self-identification value */
	ret = regmap_read(st->map, INV_ICM42600_REG_WHOAMI, &val);
	if (ret)
		return ret;
	if (val != hw->whoami) {
		dev_err(dev, "invalid whoami %#02x expected %#02x (%s)\n",
			val, hw->whoami, hw->name);
		return -ENODEV;
	}
	st->name = hw->name;

	/* reset to make sure previous state are not there */
	ret = regmap_write(st->map, INV_ICM42670_REG_SIGNAL_PATH_RESET,
			   INV_ICM42670_SIGNAL_PATH_RESET_SOFT_RESET_DEVICE_CONFIG);
	if (ret)
		return ret;
	msleep(INV_ICM42600_RESET_TIME_MS);

	ret = regmap_read(st->map, INV_ICM42670_REG_INT_STATUS, &val);
	if (ret)
		return ret;
	if (!(val & INV_ICM42600_INT_STATUS_RESET_DONE)) {
		dev_err(dev, "reset error, reset done bit not set\n");
		return -ENODEV;
	}
	/* it seems that next function configures I3C setting inside, which is not used.
	 * see inv_icm42600_spi_bus_setup in inv_icm42600_spi.c
	 */
	// ret = bus_setup(st);
	// if (ret)
	// 	return ret;

	/* sensor data in little-endian */
	ret = regmap_update_bits(st->map, INV_ICM42670_REG_INTF_CONFIG0,
				 INV_ICM42600_INTF_CONFIG0_SENSOR_DATA_ENDIAN,
				 0x00);
	if (ret)
		return ret;

	return inv_icm42600_set_conf(st, hw->conf);
}

static irqreturn_t inv_icm42600_irq_timestamp(int irq, void *_data)
{
	struct inv_icm42600_state *st = _data;

	st->timestamp.gyro = iio_get_time_ns(st->indio_dev);
	st->timestamp.accel = iio_get_time_ns(st->indio_dev);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t inv_icm42600_irq_handler(int irq, void *_data)
{
	struct inv_icm42600_state *st = _data;
	struct device *dev = regmap_get_device(st->map);
	unsigned int status;
	int ret;
	u8 data[12];
	int16_t accel_data[3], gyro_data[3];
	struct iio_dev *indio_dev = st->indio_dev;
	uint64_t timestamp = st->timestamp.accel;

	mutex_lock(&st->lock);

	ret = regmap_read(st->map, INV_ICM42670_REG_INT_STATUS_DRDY, &status);
	if (ret)
		goto out_unlock;

	/* DATA ready interrupt */
	if (status) {
		ret = regmap_bulk_read(st->map, INV_ICM42670_REG_ACCEL_DATA_X, data, sizeof(data));
		if (ret) {
			dev_err(dev, "Register read error %d\n", ret);
			goto out_unlock;
		}

		accel_data[0] = ((int16_t)data[0]) | (((int16_t)data[1]) << 8);
		accel_data[1] = ((int16_t)data[2]) | (((int16_t)data[3]) << 8);
		accel_data[2] = ((int16_t)data[4]) | (((int16_t)data[5]) << 8);

		gyro_data[0] = ((int16_t)data[6]) | (((int16_t)data[7]) << 8);
		gyro_data[1] = ((int16_t)data[8]) | (((int16_t)data[9]) << 8);
		gyro_data[2] = ((int16_t)data[10]) | (((int16_t)data[11]) << 8);

		if (indio_dev && iio_buffer_enabled(indio_dev)) {
			struct {
				int16_t channels[6];
				int64_t ts __aligned(8);
			} scan;

			scan.channels[0] = accel_data[0];
			scan.channels[1] = accel_data[1];
			scan.channels[2] = accel_data[2];
			scan.channels[3] = gyro_data[0];
			scan.channels[4] = gyro_data[1];
			scan.channels[5] = gyro_data[2];
			scan.ts = timestamp;

			iio_push_to_buffers(indio_dev, &scan);
		}
	}

out_unlock:
	mutex_unlock(&st->lock);
	return IRQ_HANDLED;
}

/**
 * inv_icm42600_irq_init() - initialize int pin and interrupt handler
 * @st:		driver internal state
 * @irq:	irq number
 * @irq_type:	irq trigger type
 * @open_drain:	true if irq is open drain, false for push-pull
 *
 * Returns 0 on success, a negative error code otherwise.
 */
static int inv_icm42600_irq_init(struct inv_icm42600_state *st, int irq,
				 int irq_type, bool open_drain)
{
	struct device *dev = regmap_get_device(st->map);
	unsigned int val;
	int ret;

	/* configure INT1 interrupt: default is active low on edge */
	switch (irq_type) {
	case IRQF_TRIGGER_RISING:
	case IRQF_TRIGGER_HIGH:
		val = INV_ICM42600_INT_CONFIG_INT1_ACTIVE_HIGH;
		break;
	default:
		val = INV_ICM42600_INT_CONFIG_INT1_ACTIVE_LOW;
		break;
	}

	switch (irq_type) {
	case IRQF_TRIGGER_LOW:
	case IRQF_TRIGGER_HIGH:
		val |= INV_ICM42600_INT_CONFIG_INT1_LATCHED;
		break;
	default:
		break;
	}

	if (!open_drain)
		val |= INV_ICM42600_INT_CONFIG_INT1_PUSH_PULL;

	ret = regmap_write(st->map, INV_ICM42670_REG_INT_CONFIG, val);
	if (ret)
		return ret;

	/* Deassert async reset for proper INT pin operation (cf datasheet) */
	ret = regmap_update_bits(st->map, INV_ICM42600_REG_INT_CONFIG1,
				 INV_ICM42600_INT_CONFIG1_ASYNC_RESET, 0);
	if (ret)
		return ret;

	return devm_request_threaded_irq(dev, irq, inv_icm42600_irq_timestamp,
					 inv_icm42600_irq_handler, irq_type,
					 "inv_icm42600", st);
}

static int inv_icm42600_timestamp_setup(struct inv_icm42600_state *st)
{
	unsigned int val;

	/* enable timestamp register */
	val = INV_ICM42600_TMST_CONFIG_TMST_TO_REGS_EN |
	      INV_ICM42600_TMST_CONFIG_TMST_EN;
	return regmap_update_bits(st->map, INV_ICM42600_REG_TMST_CONFIG,
				  INV_ICM42600_TMST_CONFIG_MASK, val);
}

static int inv_icm42600_enable_regulator_vddio(struct inv_icm42600_state *st)
{
	int ret;

	ret = regulator_enable(st->vddio_supply);
	if (ret)
		return ret;

	/* wait a little for supply ramp */
	usleep_range(3000, 4000);

	return 0;
}

static void inv_icm42600_disable_vdd_reg(void *_data)
{
	struct inv_icm42600_state *st = _data;
	const struct device *dev = regmap_get_device(st->map);
	int ret;

	ret = regulator_disable(st->vdd_supply);
	if (ret)
		dev_err(dev, "failed to disable vdd error %d\n", ret);
}

static void inv_icm42600_disable_vddio_reg(void *_data)
{
	struct inv_icm42600_state *st = _data;
	const struct device *dev = regmap_get_device(st->map);
	int ret;

	ret = regulator_disable(st->vddio_supply);
	if (ret)
		dev_err(dev, "failed to disable vddio error %d\n", ret);
}

static void inv_icm42600_disable_pm(void *_data)
{
	struct device *dev = _data;

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
}

int inv_icm42600_core_probe(struct regmap *regmap, int chip, int irq,
			    inv_icm42600_bus_setup bus_setup)
{
	struct device *dev = regmap_get_device(regmap);
	struct inv_icm42600_state *st;
	struct irq_data *irq_desc;
	int irq_type;
	bool open_drain;
	int ret;

	if (chip <= INV_CHIP_INVALID || chip >= INV_CHIP_NB) {
		dev_err(dev, "invalid chip = %d\n", chip);
		return -ENODEV;
	}

	/* get irq properties, set trigger falling by default */
	irq_desc = irq_get_irq_data(irq);
	if (!irq_desc) {
		dev_err(dev, "could not find IRQ %d\n", irq);
		return -EINVAL;
	}

	irq_type = irqd_get_trigger_type(irq_desc);
	if (!irq_type)
		irq_type = IRQF_TRIGGER_FALLING;

	open_drain = device_property_read_bool(dev, "drive-open-drain");

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	dev_set_drvdata(dev, st);
	mutex_init(&st->lock);
	st->chip = chip;
	st->map = regmap;

	ret = iio_read_mount_matrix(dev, &st->orientation);
	if (ret) {
		dev_err(dev, "failed to retrieve mounting matrix %d\n", ret);
		return ret;
	}

	st->vdd_supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(st->vdd_supply))
		return PTR_ERR(st->vdd_supply);

	st->vddio_supply = devm_regulator_get(dev, "vddio");
	if (IS_ERR(st->vddio_supply))
		return PTR_ERR(st->vddio_supply);

	ret = regulator_enable(st->vdd_supply);
	if (ret)
		return ret;
	msleep(INV_ICM42600_POWER_UP_TIME_MS);

	ret = devm_add_action_or_reset(dev, inv_icm42600_disable_vdd_reg, st);
	if (ret)
		return ret;

	ret = inv_icm42600_enable_regulator_vddio(st);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, inv_icm42600_disable_vddio_reg, st);
	if (ret)
		return ret;

	/* setup chip registers */
	ret = inv_icm42600_setup(st, bus_setup);
	if (ret)
		return ret;

	ret = inv_icm42600_timestamp_setup(st);
	if (ret)
		return ret;

	ret = inv_icm42600_buffer_init(st);
	if (ret)
		return ret;
	
	/* Initialize the unified IMU device */
	st->indio_dev = inv_icm42600_imu_init(st);
	if (IS_ERR(st->indio_dev))
		return PTR_ERR(st->indio_dev);

	ret = inv_icm42600_irq_init(st, irq, irq_type, open_drain);
	if (ret)
		return ret;

	/* setup runtime power management */
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, INV_ICM42600_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put(dev);

	return devm_add_action_or_reset(dev, inv_icm42600_disable_pm, dev);
}
EXPORT_SYMBOL_NS_GPL(inv_icm42600_core_probe, IIO_ICM42600);

/*
 * Suspend saves sensors state and turns everything off.
 * Check first if runtime suspend has not already done the job.
 */
static int inv_icm42600_suspend(struct device *dev)
{
	struct inv_icm42600_state *st = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&st->lock);

	st->suspended.gyro = st->conf.gyro.mode;
	st->suspended.accel = st->conf.accel.mode;
	st->suspended.temp = st->conf.temp_en;
	if (pm_runtime_suspended(dev)) {
		ret = 0;
		goto out_unlock;
	}

	/* disable FIFO data streaming */
	if (st->fifo.on) {
		ret = regmap_write(st->map, INV_ICM42600_REG_FIFO_CONFIG,
				   INV_ICM42600_FIFO_CONFIG_BYPASS);
		if (ret)
			goto out_unlock;
	}

	ret = inv_icm42600_set_pwr_mgmt0(st, INV_ICM42600_SENSOR_MODE_OFF,
					 INV_ICM42600_SENSOR_MODE_OFF, false,
					 NULL);
	if (ret)
		goto out_unlock;

	regulator_disable(st->vddio_supply);

out_unlock:
	mutex_unlock(&st->lock);
	return ret;
}

/*
 * System resume gets the system back on and restores the sensors state.
 * Manually put runtime power management in system active state.
 */
static int inv_icm42600_resume(struct device *dev)
{
	struct inv_icm42600_state *st = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&st->lock);

	ret = inv_icm42600_enable_regulator_vddio(st);
	if (ret)
		goto out_unlock;

	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	/* restore sensors state */
	ret = inv_icm42600_set_pwr_mgmt0(st, st->suspended.gyro,
					 st->suspended.accel,
					 st->suspended.temp, NULL);
	if (ret)
		goto out_unlock;

	/* restore FIFO data streaming */
	if (st->fifo.on)
		ret = regmap_write(st->map, INV_ICM42600_REG_FIFO_CONFIG,
				   INV_ICM42600_FIFO_CONFIG_STREAM);

out_unlock:
	mutex_unlock(&st->lock);
	return ret;
}

/* Runtime suspend will turn off sensors that are enabled by iio devices. */
static int inv_icm42600_runtime_suspend(struct device *dev)
{
	struct inv_icm42600_state *st = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&st->lock);

	/* disable all sensors */
	ret = inv_icm42600_set_pwr_mgmt0(st, INV_ICM42600_SENSOR_MODE_OFF,
					 INV_ICM42600_SENSOR_MODE_OFF, false,
					 NULL);
	if (ret)
		goto error_unlock;

	regulator_disable(st->vddio_supply);

error_unlock:
	mutex_unlock(&st->lock);
	return ret;
}

/* Sensors are enabled by iio devices, no need to turn them back on here. */
static int inv_icm42600_runtime_resume(struct device *dev)
{
	struct inv_icm42600_state *st = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&st->lock);

	ret = inv_icm42600_enable_regulator_vddio(st);

	mutex_unlock(&st->lock);
	return ret;
}

EXPORT_NS_GPL_DEV_PM_OPS(inv_icm42600_pm_ops, IIO_ICM42600) = {
	SYSTEM_SLEEP_PM_OPS(inv_icm42600_suspend, inv_icm42600_resume)
	RUNTIME_PM_OPS(inv_icm42600_runtime_suspend,
		       inv_icm42600_runtime_resume, NULL)
};

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-426xx device driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_INV_SENSORS_TIMESTAMP);
