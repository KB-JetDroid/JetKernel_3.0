/*
 * battery driver for Jet
 *
 * Copyright (C) 2011 Dopi <dopi711 at googlemail.com>
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
 * this file is based on spica-battery.c by
 * Tomasz Figa <tomasz.figa at gmail.com>, 
 */

#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/leds.h>
#include <linux/gpio.h>
#include <linux/err.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wakelock.h>
#include <linux/io.h>
#include <linux/android_alarm.h>
#include <linux/workqueue.h>

#include <linux/power/jet_battery.h>
#include <linux/i2c/max8906.h>

#include <plat/adc.h>

#include <mach/map.h>
#include <mach/regs-sys.h>
#include <mach/regs-syscon-power.h>
#include <mach/irqs.h>

/* Time between samples (in milliseconds) */
#define BAT_POLL_INTERVAL	10000

/* Time to wake up phone and recheck battery after (in seconds) */
#define SUSPEND_INTERVAL	3600

/* Number of samples for averaging (a power of two!) */
#define NUM_SAMPLES		4

/* Battery compensation */
#define TALK_GSM_COMPENSATION	(13)
#define TALK_GSM_FLAG		(1 << 2)

#define TALK_WCDMA_COMPENSATION	(14)
#define TALK_WCDMA_FLAG		(1 << 1)

#define DATA_CALL_COMPENSATION	(25)
#define DATA_CALL_FLAG		(1 << 0)

/*
 * Linear interpolation
 */

/* Interpolation table entry */
struct lookup_entry {
	int start;
	int end;
	int base;
	int delta;
};

/* Interpolation table descriptor */
struct lookup_data {
	struct lookup_entry	*table;
	unsigned int		size;
};

/* Calculates interpolated value based on given input value */
static int lookup_value(struct lookup_data *data, int value)
{
	struct lookup_entry *table = data->table;
	unsigned int min = 0, max = data->size - 1;

	/* Do a binary search in the lookup table */
	while (min != max) {
		if (value < table[(min + max) / 2].start) {
			max = -1 + (min + max) / 2;
			continue;
		}
		if (value >= table[(min + max) / 2].end) {
			min = 1 + (min + max) / 2;
			continue;
		}
		min = (min + max) / 2;
		break;
	}

	/* We should have the correct table entry pointer here */
	return table[min].base + (value - table[min].start)*table[min].delta;
}

/* Creates interpolation table based on platform data */
static int create_lookup_table(
			const struct jet_battery_threshold *thresholds,
			int threshold_count, struct lookup_data *data)
{
	struct lookup_entry *entry;

	if (!thresholds || threshold_count < 2)
		return -EINVAL;

	data->size = threshold_count + 1;

	entry = kmalloc(data->size * sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	data->table = entry;

	/* Don't account the last entry */
	--threshold_count;

	/* Add the bottom sentinel */
	entry->start = 0;
	entry->end = thresholds->adc;
	entry->base = thresholds->val;
	entry->delta = 0;
	++entry;

	do {
		int adc_range, val_range;
		adc_range = (thresholds+1)->adc - thresholds->adc;
		val_range = (thresholds+1)->val - thresholds->val;
		entry->start = thresholds->adc;
		entry->end = (thresholds+1)->adc;
		entry->delta = val_range / adc_range;
		entry->base = thresholds->val;
		++thresholds;
		++entry;
	} while (--threshold_count);

	/* Add the top sentinel */
	entry->start = thresholds->adc;
	entry->end = 0xffffffff;
	entry->base = thresholds->val;
	entry->delta = 0;

	return 0;
}

/* 
 * Avaraging algorithm
 */

/* Algorithm data */
struct average_data {
	int				samples[NUM_SAMPLES];
	unsigned int			cur_sample;
	int				sum;
};

/* Here the assumption that NUM_SAMPLES is a power of two shows its importance,
 * as both the modulo and division operations will be translated to simple
 * bitwise AND and shift operations. */
static int put_sample_get_avg(struct average_data *avg, int sample)
{
	avg->sum -= avg->samples[avg->cur_sample];
	avg->samples[avg->cur_sample] = sample;
	avg->sum += sample;
	avg->cur_sample = (avg->cur_sample + 1) % NUM_SAMPLES;

	return avg->sum / NUM_SAMPLES;
}

/*
 * Battery driver
 */

/* Driver data */
struct jet_battery {
	struct power_supply		bat;
	struct power_supply		psy[JET_BATTERY_NUM];

	struct s3c_adc_client		*client;
	struct jet_battery_pdata	*pdata;
	struct work_struct		work;
	struct workqueue_struct		*workqueue;
	struct delayed_work		poll_work;
	struct mutex			mutex;
	struct device			*dev;
#ifdef CONFIG_HAS_WAKELOCK
	struct wake_lock	wakelock;
	struct wake_lock	chg_wakelock;
	struct wake_lock	fault_wakelock;
	struct wake_lock	suspend_lock;
#endif
#ifdef CONFIG_RTC_INTF_ALARM
	struct alarm		alarm;
#endif
	unsigned int irq_pok;
	unsigned int irq_chg;

	int percent_value;
	int volt_value;
	int temp_value;
	int vol_adc;
	int temp_adc;
	int status;
	int health;
	int online[JET_BATTERY_NUM];
	int fault;
	int chg_enable;
	int compensation;
	int compensation_flags;
	int calibration;
	enum jet_battery_supply supply;

	unsigned int		interval;
	struct average_data	volt_avg;
	struct average_data	temp_avg;
	struct lookup_data	percent_lookup;
	struct lookup_data	volt_lookup;
	struct lookup_data	temp_lookup;
};

#ifdef CONFIG_RTC_INTF_ALARM
/* Alarm handler */
static void jet_battery_alarm(struct alarm *alarm)
{
	/* Nothing to do here */
}
#endif

/* Battery fault handling */
static void jet_battery_set_fault_enable(bool enable)
{
	unsigned long flags;
	u32 reg, val;

	if (enable)
		val = S3C64XX_PWRCFG_CFG_BATFLT_IRQ;
	else
		val = S3C64XX_PWRCFG_CFG_BATFLT_IGNORE;

	local_irq_save(flags);

	reg = readl(S3C64XX_PWR_CFG);
	reg &= ~S3C64XX_PWRCFG_CFG_BATFLT_MASK;
	reg |= val;
	writel(reg, S3C64XX_PWR_CFG);

	local_irq_restore(flags);
}

static irqreturn_t jet_battery_fault_irq(int irq, void *dev_id)
{
	struct jet_battery *bat = dev_id;
	unsigned long flags;
	u32 reg;
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock(&bat->fault_wakelock);
#endif
	local_irq_save(flags);

	jet_battery_set_fault_enable(0);

	reg = readl(S3C64XX_OTHERS);
	reg |= S3C64XX_OTHERS_CLEAR_BATF_INT;
	writel(reg, S3C64XX_OTHERS);

	bat->fault = 1;

	local_irq_restore(flags);

	queue_work(bat->workqueue, &bat->work);

	return IRQ_HANDLED;
}

/* Polling function */
static void jet_battery_poll(struct work_struct *work)
{
	struct delayed_work *dwrk = to_delayed_work(work);
	struct jet_battery *bat =
			container_of(dwrk, struct jet_battery, poll_work);
	struct jet_battery_pdata *pdata = bat->pdata;
	int volt_sample, volt_value, temp_sample, temp_value, percent_value;
	int health, update = 0;

	mutex_lock(&bat->mutex);

	/* Get a voltage sample from the ADC */
#if 0
	volt_sample = s3c_adc_read(bat->client, bat->pdata->volt_channel);
	if (volt_sample < 0) {
		dev_warn(bat->dev, "Failed to get ADC sample.\n");
		bat->health = POWER_SUPPLY_HEALTH_UNKNOWN;
		goto error;
	}
	volt_sample += bat->compensation;
	bat->vol_adc = volt_sample;
	volt_sample = put_sample_get_avg(&bat->volt_avg, volt_sample);
	volt_value = lookup_value(&bat->volt_lookup, volt_sample);
	percent_value = lookup_value(&bat->percent_lookup, volt_sample);
#endif

	/* Get a temperature sample from the ADC */
	temp_sample = s3c_adc_read(bat->client, bat->pdata->temp_channel);
	//printk("KB: %s VF adc read value = %d\n", __func__, temp_sample);
	if (temp_sample < 0) {
		dev_warn(bat->dev, "Failed to get ADC sample.\n");
		bat->health = POWER_SUPPLY_HEALTH_UNKNOWN;
		goto error;
	}
	bat->temp_adc = temp_sample;
	temp_sample = put_sample_get_avg(&bat->temp_avg, temp_sample);
	temp_value = lookup_value(&bat->temp_lookup, temp_sample);
	//printk("KB: %s lookup temp value = %d\n", __func__, temp_value);

	if (bat->health == POWER_SUPPLY_HEALTH_UNKNOWN)
		bat->health = POWER_SUPPLY_HEALTH_GOOD;

	volt_value = 4100000;
	percent_value = 70000;
	//temp_value = 10000;

	bat->volt_value = volt_value;
	bat->percent_value = percent_value;
	bat->temp_value = temp_value;

	health = bat->health;

	if (temp_value <= pdata->low_temp_enter)
		health = POWER_SUPPLY_HEALTH_COLD;

	if (temp_value >= pdata->high_temp_enter)
		health = POWER_SUPPLY_HEALTH_OVERHEAT;

	if (temp_value >= pdata->low_temp_exit
	    && temp_value <= pdata->high_temp_exit)
		health = POWER_SUPPLY_HEALTH_GOOD;

	if (bat->health != health) {
		bat->health = health;
		update = 1;
	}

error:
	mutex_unlock(&bat->mutex);

	/* Schedule next poll */
	if (update) {
		queue_work(bat->workqueue, &bat->work);
	} else {
		queue_delayed_work(bat->workqueue, &bat->poll_work,
					msecs_to_jiffies(bat->interval));
		power_supply_changed(&bat->bat);
	}
}

static void jet_battery_work(struct work_struct *work)
{
	struct jet_battery *bat =
			container_of(work, struct jet_battery, work);
	struct jet_battery_pdata *pdata = bat->pdata;
	int is_plugged, is_healthy, chg_enable, bat_low;
	enum jet_battery_supply type;
	int i;
	unsigned char buf, vac, vbus, charge_mode;
#if 0
	printk("KB: %s Entering\n", __func__);
	/* Cancel any pending works */
	cancel_delayed_work_sync(&bat->poll_work);

	if (Get_MAX8906_PM_ADDR(REG_CHG_CNTL1, &buf ,1))
		printk("KB: %s REG_CHG_CNTL1 = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_CHG_CNTL2, &buf ,1))
		printk("KB: %s REG_CHG_CNTL2 = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_CHG_IRQ1, &buf ,1))
		printk("KB: %s REG_CHG_IRQ1 = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_CHG_IRQ2, &buf ,1))
		printk("KB: %s REG_CHG_IRQ2 = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_CHG_IRQ1_MASK, &buf ,1))
		printk("KB: %s REG_CHG_IRQ1_MASK = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_CHG_IRQ2_MASK, &buf ,1))
		printk("KB: %s REG_CHG_IRQ2_MASK = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_CHG_STAT, &buf ,1))
		printk("KB: %s REG_CHG_STAT = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_BBATTCNFG, &buf ,1))
		printk("KB: %s REG_BBATTCNFG = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_LBCNFG, &buf ,1))
		printk("KB: %s REG_LBCNFG = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_ON_OFF_IRQ, &buf ,1))
		printk("KB: %s REG_ON_OFF_IRQ = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_ON_OFF_IRQ_MASK, &buf ,1))
		printk("KB: %s REG_ON_OFF_IRQ_MASK = 0x%02x\n", __func__, buf);
	if (Get_MAX8906_PM_ADDR(REG_ON_OFF_STAT, &buf ,1))
		printk("KB: %s REG_ON_OFF_STAT = 0x%02x\n", __func__, buf);
#endif
	if (Get_MAX8906_PM_REG(VAC_OK, &vac))
		printk("KB: %s VAC_OK = 0x%02x\n", __func__, vac);
	if (Get_MAX8906_PM_REG(VBUS_OK, &vbus))
		printk("KB: %s VBUS_OK = 0x%02x\n", __func__, vbus);
	if (Get_MAX8906_PM_REG(CHG_MODE, &charge_mode))
		printk("KB: %s CHG_MODE = 0x%02x\n", __func__, charge_mode);
	if (Get_MAX8906_PM_REG(MBATTLOW, &bat_low))
		printk("KB: %s MBATTLOW = 0x%02x\n", __func__, bat_low);

	/* Check for external power supply connection */
//	is_plugged = gpio_get_value(pdata->gpio_pok) ^ pdata->gpio_pok_inverted;
	is_plugged = vac | vbus;

	/* We're going to access shared data */
	mutex_lock(&bat->mutex);

	//printk("KB: %s bat->supply = %d\n", __func__, bat->supply);
	//printk("KB: %s bat->health = %d\n", __func__, bat->health);
	//printk("KB: %s bat->chg_enable = %d\n", __func__, bat->chg_enable);
	type = bat->supply;
	if (type == JET_BATTERY_NONE)
		is_plugged = 0;

	for (i = 0; i < JET_BATTERY_NUM; ++i)
		bat->online[i] = (type == i);

	is_healthy = (bat->health == POWER_SUPPLY_HEALTH_GOOD);
	if (!is_healthy) {
		if (bat->health == POWER_SUPPLY_HEALTH_OVERHEAT)
			dev_warn(bat->dev,
				 "Battery overheated, disabling charger.\n");

		if (bat->health == POWER_SUPPLY_HEALTH_COLD);
			dev_warn(bat->dev,
				 "Battery temperature too low, disabling charger.\n");
	}

	/* Update charging status and polling interval */
	chg_enable = is_plugged && is_healthy;

	if (bat->chg_enable == chg_enable)
		goto no_change;

	if (chg_enable) {
#ifdef CONFIG_HAS_WAKELOCK
		wake_lock(&bat->chg_wakelock);
#endif
		/* Enable the charger */
//		gpio_set_value(pdata->gpio_en, !pdata->gpio_en_inverted);

		bat->fault = 0;
#ifdef CONFIG_HAS_WAKELOCK
		wake_unlock(&bat->fault_wakelock);
#endif
	} else {
		bat->status = POWER_SUPPLY_STATUS_DISCHARGING;

		/* Disable the charger */
//		gpio_set_value(pdata->gpio_en, pdata->gpio_en_inverted);

		/* Enable battery fault interrupt */
//		jet_battery_set_fault_enable(1);
#ifdef CONFIG_HAS_WAKELOCK
		wake_lock_timeout(&bat->chg_wakelock, HZ / 2);
#endif
	}

	bat->chg_enable = chg_enable;

no_change:
	if (chg_enable) {
		if (charge_mode != CHARGE_DONE)
			bat->status = POWER_SUPPLY_STATUS_CHARGING;
		else
			bat->status = POWER_SUPPLY_STATUS_FULL;
	}

	/* We're no longer accessing shared data */
	mutex_unlock(&bat->mutex);

	/* Update the values and spin the polling loop */
	jet_battery_poll(&bat->poll_work.work);

	/* Notify anyone interested */
	power_supply_changed(&bat->bat);
	for (i = 0; i < JET_BATTERY_NUM; ++i)
		power_supply_changed(&bat->psy[i]);
#ifdef CONFIG_HAS_WAKELOCK
	wake_unlock(&bat->wakelock);
#endif
}

static void jet_battery_supply_notify(struct platform_device *pdev,
						enum jet_battery_supply type)
{
	struct jet_battery *bat = platform_get_drvdata(pdev);

	mutex_lock(&bat->mutex);

	bat->supply = type;

	mutex_unlock(&bat->mutex);
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock(&bat->wakelock);
#endif
	queue_work(bat->workqueue, &bat->work);
}

/*
 * Battery specific code
 */

static ssize_t dummy_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	return -EINVAL;
}

static ssize_t dummy_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return -EINVAL;
}

static ssize_t compensation_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	mutex_lock(&bat->mutex);

	val = bat->compensation;

	mutex_unlock(&bat->mutex);

	return sprintf(buf, "%d\n", val);
}

static DEVICE_ATTR(compensation, S_IRUGO, compensation_show, dummy_store);

static ssize_t compensation_flags_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	mutex_lock(&bat->mutex);

	val = bat->compensation_flags;

	mutex_unlock(&bat->mutex);

	return sprintf(buf, "%x\n", val);
}

static DEVICE_ATTR(compensation_flags, S_IRUGO,
					compensation_flags_show, dummy_store);

static ssize_t data_call_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	mutex_lock(&bat->mutex);

	if (!val != !(bat->compensation_flags & DATA_CALL_FLAG)) {
		if (!val) {
			bat->compensation -= DATA_CALL_COMPENSATION;
			bat->compensation_flags &= ~DATA_CALL_FLAG;
		} else {
			bat->compensation += DATA_CALL_COMPENSATION;
			bat->compensation_flags |= DATA_CALL_FLAG;
		}
	}

	mutex_unlock(&bat->mutex);

	return count;
}

static DEVICE_ATTR(data_call, S_IWUGO, dummy_show, data_call_store);

static ssize_t talk_wcdma_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	mutex_lock(&bat->mutex);

	if (!val != !(bat->compensation_flags & TALK_WCDMA_FLAG)) {
		if (!val) {
			bat->compensation -= TALK_WCDMA_COMPENSATION;
			bat->compensation_flags &= ~TALK_WCDMA_FLAG;
		} else {
			bat->compensation += TALK_WCDMA_COMPENSATION;
			bat->compensation_flags |= TALK_WCDMA_FLAG;
		}
	}

	mutex_unlock(&bat->mutex);

	return count;
}

static DEVICE_ATTR(talk_wcdma, S_IWUGO, dummy_show, talk_wcdma_store);

static ssize_t talk_gsm_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	mutex_lock(&bat->mutex);

	if (!val != !(bat->compensation_flags & TALK_GSM_FLAG)) {
		if (!val) {
			bat->compensation -= TALK_GSM_COMPENSATION;
			bat->compensation_flags &= ~TALK_GSM_FLAG;
		} else {
			bat->compensation += TALK_GSM_COMPENSATION;
			bat->compensation_flags |= TALK_GSM_FLAG;
		}
	}

	mutex_unlock(&bat->mutex);

	return count;
}

static DEVICE_ATTR(talk_gsm, S_IWUGO, dummy_show, talk_gsm_store);

static ssize_t charging_source_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int source;

	mutex_lock(&bat->mutex);

	source = bat->supply;

	mutex_unlock(&bat->mutex);

	return sprintf(buf, "%d\n", source + 1);
}

static DEVICE_ATTR(charging_source, S_IRUGO, charging_source_show, dummy_store);

static ssize_t batt_vol_adc_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	mutex_lock(&bat->mutex);

	val = bat->vol_adc;

	mutex_unlock(&bat->mutex);

	return sprintf(buf, "%d\n", val);
}

static DEVICE_ATTR(batt_vol_adc, S_IRUGO, batt_vol_adc_show, dummy_store);

static ssize_t batt_temp_adc_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	mutex_lock(&bat->mutex);

	val = bat->temp_adc;

	mutex_unlock(&bat->mutex);

	return sprintf(buf, "%d\n", val);
}

static DEVICE_ATTR(batt_temp_adc, S_IRUGO, batt_temp_adc_show, dummy_store);

static ssize_t batt_vol_adc_cal_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	mutex_lock(&bat->mutex);

	val = bat->calibration;

	mutex_unlock(&bat->mutex);

	return sprintf(buf, "%d\n", val);
}

static ssize_t batt_vol_adc_cal_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	mutex_lock(&bat->mutex);

	dev_info(bat->dev, "Setting battery calibration value to %d. (Was %d.)\n",
							val, bat->calibration);

	bat->compensation += bat->calibration - val;
	bat->calibration = val;

	mutex_unlock(&bat->mutex);

	return count;
}

static DEVICE_ATTR(batt_vol_adc_cal, S_IRUGO | S_IWUGO,
				batt_vol_adc_cal_show, batt_vol_adc_cal_store);

static struct device_attribute *battery_attrs[] = {
	&dev_attr_data_call,
	&dev_attr_talk_wcdma,
	&dev_attr_talk_gsm,
	&dev_attr_charging_source,
	&dev_attr_batt_vol_adc,
	&dev_attr_batt_temp_adc,
	&dev_attr_batt_vol_adc_cal,
	&dev_attr_compensation,
	&dev_attr_compensation_flags,
};

/* Gets a property */
static int jet_battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct jet_battery *bat = dev_get_drvdata(psy->dev->parent);
	int ret = 0;

	mutex_lock(&bat->mutex);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = bat->status;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = 100000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_EMPTY_DESIGN:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		if (bat->fault)
			val->intval = 0;
		else if (bat->percent_value == 100000
		    && bat->status == POWER_SUPPLY_STATUS_CHARGING)
			val->intval = 99999;
		else
			val->intval = bat->percent_value;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = bat->volt_value;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = bat->temp_value / 100;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (bat->fault)
			val->intval = POWER_SUPPLY_HEALTH_DEAD;
		else
			val->intval = bat->health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_ONLINE:
		/* Battery is assumed to be present. */
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = bat->pdata->technology;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (bat->fault)
			val->intval = 0;
		else if (bat->percent_value == 100000
		    && bat->status == POWER_SUPPLY_STATUS_CHARGING)
			val->intval = 99;
		else
			val->intval = bat->percent_value / 1000;
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&bat->mutex);

	return ret;
}

/* Properties which we support */
static enum power_supply_property jet_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_EMPTY_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY
};

static const struct power_supply jet_bat_template = {
	.name			= "battery",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= jet_battery_props,
	.num_properties		= ARRAY_SIZE(jet_battery_props),
	.get_property		= jet_battery_get_property,
};

/*
 * Charger specific code
 */
#ifdef CONFIG_HAS_WAKELOCK
static ssize_t suspend_lock_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct jet_battery *bat = dev_get_drvdata(dev->parent);
	int val;

	if (sscanf(buf, "%d\n", &val) != 1)
		return -EINVAL;

	if (val)
		wake_lock(&bat->suspend_lock);
	else
		wake_lock_timeout(&bat->suspend_lock, HZ / 2);

	return count;
}

static ssize_t suspend_lock_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return 0;
}

static DEVICE_ATTR(suspend_lock, S_IRUGO | S_IWUGO,
					suspend_lock_show, suspend_lock_store);
#endif
/* State change irq */
static irqreturn_t jet_charger_irq(int irq, void *dev_id)
{
	struct jet_battery *bat = dev_id;
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock(&bat->wakelock);
#endif
	queue_work(bat->workqueue, &bat->work);

	return IRQ_HANDLED;
}

/* Gets a property */
static int jet_charger_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct jet_battery *bat = dev_get_drvdata(psy->dev->parent);
	int id = psy - bat->psy;
	int ret = 0;

	mutex_lock(&bat->mutex);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		/* Battery is assumed to be present. */
		val->intval = bat->online[id];
		break;
	default:
		ret = -EINVAL;
	}

	mutex_unlock(&bat->mutex);

	return ret;
}

static enum power_supply_property jet_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply jet_chg_templates[JET_BATTERY_NUM] = {
	[JET_BATTERY_USB] = {
		.properties		= jet_charger_props,
		.num_properties		= ARRAY_SIZE(jet_charger_props),
		.get_property		= jet_charger_get_property,
		.name			= "usb",
		.type			= POWER_SUPPLY_TYPE_USB,
	},
	[JET_BATTERY_AC] = {
		.properties		= jet_charger_props,
		.num_properties		= ARRAY_SIZE(jet_charger_props),
		.get_property		= jet_charger_get_property,
		.name			= "ac",
		.type			= POWER_SUPPLY_TYPE_MAINS,
	}
};

/*
 * Platform driver
 */

static int jet_battery_probe(struct platform_device *pdev)
{
	struct s3c_adc_client	*client;
	struct jet_battery_pdata *pdata = pdev->dev.platform_data;
	struct jet_battery *bat;
	int ret, i, irq;

	/* Platform data is required */
	if (!pdata) {
		dev_err(&pdev->dev, "no platform data supplied\n");
		return -ENODEV;
	}

	/* Check GPIOs */
	if (!gpio_is_valid(pdata->gpio_pok)) {
		dev_err(&pdev->dev, "Invalid gpio pin for POK line\n");
		return -EINVAL;
	}

#if 0
	if (!gpio_is_valid(pdata->gpio_chg)) {
		dev_err(&pdev->dev, "Invalid gpio pin for CHG line\n");
		return -EINVAL;
	}

	if (!gpio_is_valid(pdata->gpio_en)) {
		dev_err(&pdev->dev, "Invalid gpio pin for EN line\n");
		return -EINVAL;
	}
#endif

	if (!pdata->supply_detect_init) {
		dev_err(&pdev->dev, "Supply detection is required\n");
		return -EINVAL;
	}

#if 1
	/* Register ADC client */
	client = s3c_adc_register(pdev, NULL, NULL, 0);
	if (IS_ERR(client)) {
		dev_err(&pdev->dev, "could not register adc\n");
		return PTR_ERR(client);
	}
#endif

	/* Allocate driver data */
	bat = kzalloc(sizeof(struct jet_battery), GFP_KERNEL);
	if (!bat) {
		dev_err(&pdev->dev, "could not allocate driver data\n");
		ret = -ENOMEM;
		goto err_free_adc;
	}

	/* Claim and setup GPIOs */
	ret = gpio_request(pdata->gpio_pok, dev_name(&pdev->dev));
	if (ret) {
		dev_err(&pdev->dev, "Failed to request POK pin: %d\n", ret);
		goto err_free;
	}

	ret = gpio_direction_input(pdata->gpio_pok);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set POK to input: %d\n", ret);
		goto err_gpio_pok_free;
	}

#if 0
	ret = gpio_request(pdata->gpio_chg, dev_name(&pdev->dev));
	if (ret) {
		dev_err(&pdev->dev, "Failed to request CHG pin: %d\n", ret);
		goto err_gpio_pok_free;
	}

	ret = gpio_direction_input(pdata->gpio_chg);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set CHG to input: %d\n", ret);
		goto err_gpio_chg_free;
	}

	ret = gpio_request(pdata->gpio_en, dev_name(&pdev->dev));
	if (ret) {
		dev_err(&pdev->dev, "Failed to request EN pin: %d\n", ret);
		goto err_gpio_chg_free;
	}

	ret = gpio_direction_output(pdata->gpio_en, pdata->gpio_en_inverted);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set EN to output: %d\n", ret);
		goto err_gpio_en_free;
	}
#endif

	platform_set_drvdata(pdev, bat);

	bat->dev = &pdev->dev;
	bat->client = client;
	bat->pdata = pdata;
	bat->status = POWER_SUPPLY_STATUS_DISCHARGING;
	bat->health = POWER_SUPPLY_HEALTH_GOOD;
	bat->supply = JET_BATTERY_NONE;
	bat->interval = BAT_POLL_INTERVAL;
	bat->calibration = pdata->calibration;

#if 0
	ret = create_lookup_table(pdata->percent_lut,
				pdata->percent_lut_cnt, &bat->percent_lookup);
	if (ret) {
		dev_err(&pdev->dev, "could not get create percentage lookup table");
		goto err_gpio_en_free;
	}

	ret = create_lookup_table(pdata->volt_lut,
				pdata->volt_lut_cnt, &bat->volt_lookup);
	if (ret) {
		dev_err(&pdev->dev, "could not get create voltage lookup table");
		goto err_percent_free;
	}
#endif

	ret = create_lookup_table(pdata->temp_lut,
				pdata->temp_lut_cnt, &bat->temp_lookup);
	if (ret) {
		dev_err(&pdev->dev, "could not get create temperature lookup table");
		goto err_volt_free;
	}

	INIT_WORK(&bat->work, jet_battery_work);
	INIT_DELAYED_WORK(&bat->poll_work, jet_battery_poll);
	mutex_init(&bat->mutex);
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_init(&bat->wakelock, WAKE_LOCK_SUSPEND, "battery");
	wake_lock_init(&bat->chg_wakelock, WAKE_LOCK_SUSPEND, "charger");
	wake_lock_init(&bat->fault_wakelock,
					WAKE_LOCK_SUSPEND, "battery fault");
	wake_lock_init(&bat->suspend_lock, WAKE_LOCK_SUSPEND, "suspend_lock");
#endif
#ifdef CONFIG_RTC_INTF_ALARM
	alarm_init(&bat->alarm, ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP,
							jet_battery_alarm);
#endif

#if 1
	/* Get some initial data for averaging */
	for (i = 0; i < NUM_SAMPLES; ++i) {
		int sample;
#if 0
		/* Get a voltage sample from the ADC */
		sample = s3c_adc_read(bat->client, bat->pdata->volt_channel);
		if (sample < 0) {
			dev_warn(&pdev->dev, "Failed to get ADC sample.\n");
			continue;
		}
		sample += bat->compensation;
		bat->vol_adc = sample;
		/* Put the sample and get the new average */
		bat->volt_value = put_sample_get_avg(&bat->volt_avg, sample);
#endif
		/* Get a temperature sample from the ADC */
		sample = s3c_adc_read(bat->client, bat->pdata->temp_channel);
		if (sample < 0) {
			dev_warn(&pdev->dev, "Failed to get ADC sample.\n");
			continue;
		}
		printk("KB: %s VF adc read value = %d\n", __func__, sample);
		bat->temp_adc = sample;
		/* Put the sample and get the new average */
		bat->temp_value = put_sample_get_avg(&bat->temp_avg, sample);
	}
#endif

	bat->percent_value = 70000; //lookup_value(&bat->percent_lookup, bat->volt_value);
	bat->volt_value = 4100000; //lookup_value(&bat->volt_lookup, bat->volt_value);
	bat->temp_value = lookup_value(&bat->temp_lookup, bat->temp_value);
	//printk("KB: %s lookup temp value = %d\n", __func__, bat->temp_value);

	/* Register the power supplies */
	for (i = 0; i < JET_BATTERY_NUM; ++i) {
		bat->psy[i] = jet_chg_templates[i];
		ret = power_supply_register(&pdev->dev, &bat->psy[i]);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"Failed to register power supply %s (%d)\n",
							bat->psy[i].name, ret);
			break;
		}
	}

	/* Undo the loop on error */
	if (i-- != JET_BATTERY_NUM) {
		for (; i >= 0; --i)
			power_supply_unregister(&bat->psy[i]);
		goto err_temp_free;
	}
#ifdef CONFIG_HAS_WAKELOCK
	ret = device_create_file(bat->psy[JET_BATTERY_AC].dev,
							&dev_attr_suspend_lock);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Failed to register device attribute.\n");
		goto err_psy_unreg;
	}
#endif
	/* Register the battery */
	bat->bat = jet_bat_template;
	ret = power_supply_register(&pdev->dev, &bat->bat);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Failed to register battery power supply: %d\n", ret);
		goto err_attr_unreg;
	}

	for (i = 0; i < ARRAY_SIZE(battery_attrs); ++i) {
		ret = device_create_file(bat->bat.dev, battery_attrs[i]);
		if (ret < 0)
			break;
	}

	if (ret < 0) {
		for (; i >= 0; --i)
			device_remove_file(bat->bat.dev, battery_attrs[i]);
		goto err_bat_unreg;
	}

	bat->workqueue = create_freezable_workqueue(dev_name(&pdev->dev));
	if (!bat->workqueue) {
		dev_err(&pdev->dev, "Failed to create freezeable workqueue\n");
		ret = -ENOMEM;
		goto err_remove_bat_attr;
	}

	/* Claim IRQs */
#if 0
	irq = gpio_to_irq(pdata->gpio_pok);
	if (irq <= 0) {
		dev_err(&pdev->dev, "POK irq invalid.\n");
		goto err_destroy_workqueue;
	}
	bat->irq_pok = irq;

	ret = request_irq(irq, jet_charger_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				dev_name(&pdev->dev), bat);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request POK irq (%d)\n", ret);
		goto err_destroy_workqueue;
	}
#endif

#if 0
	irq = gpio_to_irq(pdata->gpio_chg);
	if (irq <= 0) {
		dev_err(&pdev->dev, "CHG irq invalid.\n");
		goto err_pok_irq_free;
	}
	bat->irq_chg = irq;

	ret = request_irq(irq, jet_charger_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				dev_name(&pdev->dev), bat);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request CHG irq (%d)\n", ret);
		goto err_pok_irq_free;
	}

	ret = request_irq(IRQ_BATF, jet_battery_fault_irq,
						0, dev_name(&pdev->dev), bat);
	if (ret) {
		dev_err(&pdev->dev,
			"Failed to request battery fault irq (%d)\n", ret);
		goto err_chg_irq_free;
	}
#endif

//	enable_irq_wake(bat->irq_pok);
	enable_irq_wake(bat->irq_chg);

//	jet_battery_set_fault_enable(1);

	/* Finish */
	dev_info(&pdev->dev, "successfully loaded\n");
	device_init_wakeup(&pdev->dev, 1);

	pdata->supply_detect_init(jet_battery_supply_notify);

	/* Schedule work to check current status */
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock(&bat->wakelock);
#endif
	queue_work(bat->workqueue, &bat->work);

	return 0;

err_chg_irq_free:
	free_irq(bat->irq_chg, bat);
err_pok_irq_free:
//	free_irq(bat->irq_pok, bat);
err_destroy_workqueue:
	destroy_workqueue(bat->workqueue);
err_remove_bat_attr:
	for (i = 0; i < ARRAY_SIZE(battery_attrs); ++i)
		device_remove_file(bat->bat.dev, battery_attrs[i]);
err_bat_unreg:
	power_supply_unregister(&bat->bat);
err_attr_unreg:
#ifdef CONFIG_HAS_WAKELOCK
	device_remove_file(bat->psy[JET_BATTERY_AC].dev,
							&dev_attr_suspend_lock);
err_psy_unreg:
#endif
	for (i = 0; i < JET_BATTERY_NUM; ++i)
		power_supply_unregister(&bat->psy[i]);
err_temp_free:
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_destroy(&bat->wakelock);
	wake_lock_destroy(&bat->chg_wakelock);
	wake_lock_destroy(&bat->fault_wakelock);
	wake_lock_destroy(&bat->suspend_lock);
#endif
	kfree(bat->temp_lookup.table);
err_volt_free:
	kfree(bat->volt_lookup.table);
err_percent_free:
	kfree(bat->percent_lookup.table);
err_gpio_en_free:
//	gpio_free(pdata->gpio_en);
err_gpio_chg_free:
	gpio_free(pdata->gpio_chg);
err_gpio_pok_free:
	gpio_free(pdata->gpio_pok);
err_free:
	kfree(bat);
err_free_adc:
//	s3c_adc_release(client);
	return ret;
}

static int jet_battery_remove(struct platform_device *pdev)
{
	struct jet_battery *bat = platform_get_drvdata(pdev);
	struct jet_battery_pdata *pdata = bat->pdata;
	int i;

	disable_irq_wake(bat->irq_pok);
	disable_irq_wake(bat->irq_chg);

	free_irq(IRQ_BATF, bat);
	free_irq(bat->irq_chg, bat);
	free_irq(bat->irq_pok, bat);

	if (pdata->supply_detect_cleanup)
		pdata->supply_detect_cleanup();

	for (i = 0; i < ARRAY_SIZE(battery_attrs); ++i)
		device_remove_file(bat->bat.dev, battery_attrs[i]);
	power_supply_unregister(&bat->bat);
#ifdef CONFIG_HAS_WAKELOCK
	device_remove_file(bat->psy[JET_BATTERY_AC].dev,
							&dev_attr_suspend_lock);
#endif
	for (i = 0; i < JET_BATTERY_NUM; ++i)
		power_supply_unregister(&bat->psy[i]);

	cancel_work_sync(&bat->work);
	cancel_delayed_work_sync(&bat->poll_work);
	destroy_workqueue(bat->workqueue);

	s3c_adc_release(bat->client);

	kfree(bat->temp_lookup.table);
	kfree(bat->volt_lookup.table);
	kfree(bat->percent_lookup.table);

	gpio_set_value(pdata->gpio_en, pdata->gpio_en_inverted);
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_destroy(&bat->fault_wakelock);
	wake_lock_destroy(&bat->chg_wakelock);
	wake_lock_destroy(&bat->wakelock);
	wake_lock_destroy(&bat->suspend_lock);
#endif
	kfree(bat);

	return 0;
}

static int jet_battery_prepare(struct device *dev)
{
	struct jet_battery *bat = dev_get_drvdata(dev);
	ktime_t now, start, end;
#ifdef CONFIG_RTC_INTF_ALARM
	now = alarm_get_elapsed_realtime();
	start = ktime_set(SUSPEND_INTERVAL - 10, 0);
	start = ktime_add(now, start);
	end = ktime_set(SUSPEND_INTERVAL + 10, 0);
	end = ktime_add(now, end);
	alarm_start_range(&bat->alarm, start, end);
#endif
	return 0;
}

static void jet_battery_complete(struct device *dev)
{
	struct jet_battery *bat = dev_get_drvdata(dev);
	int volt_value = -1, temp_value = -1;
	int i;
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock(&bat->wakelock);
#endif
#ifdef CONFIG_RTC_INTF_ALARM
	alarm_cancel(&bat->alarm);
#endif
	/* Get some initial data for averaging */
	for (i = 0; i < NUM_SAMPLES; ++i) {
		int sample;
		/* Get a voltage sample from the ADC */
		sample = s3c_adc_read(bat->client, bat->pdata->volt_channel);
		if (sample < 0) {
			dev_warn(dev, "Failed to get ADC sample.\n");
			continue;
		}
		sample += bat->compensation;
		bat->vol_adc = sample;
		/* Put the sample and get the new average */
		volt_value = put_sample_get_avg(&bat->volt_avg, sample);
		/* Get a temperature sample from the ADC */
		sample = s3c_adc_read(bat->client, bat->pdata->temp_channel);
		if (sample < 0) {
			dev_warn(dev, "Failed to get ADC sample.\n");
			continue;
		}
		bat->temp_adc = sample;
		/* Put the sample and get the new average */
		temp_value = put_sample_get_avg(&bat->temp_avg, sample);
	}

	if (volt_value > 0) {
		bat->percent_value = lookup_value(&bat->percent_lookup, volt_value);
		bat->volt_value = lookup_value(&bat->volt_lookup, volt_value);
	}

	if (temp_value > 0)
		bat->temp_value = lookup_value(&bat->temp_lookup, temp_value);

	/* Schedule timer to check current status */
	queue_work(bat->workqueue, &bat->work);
}

static struct dev_pm_ops jet_battery_pm_ops = {
	.prepare	= jet_battery_prepare,
	.complete	= jet_battery_complete,
};

static struct platform_driver jet_battery_driver = {
	.driver		= {
		.name	= "jet-battery",
		.pm	= &jet_battery_pm_ops,
	},
	.probe		= jet_battery_probe,
	.remove		= jet_battery_remove,
};

/*
 * Kernel module
 */

static int __init jet_battery_init(void)
{
	return platform_driver_register(&jet_battery_driver);
}
module_init(jet_battery_init);

static void __exit jet_battery_exit(void)
{
	platform_driver_unregister(&jet_battery_driver);
}
module_exit(jet_battery_exit);

MODULE_AUTHOR("Tomasz Figa <tomasz.figa at gmail.com>");
MODULE_DESCRIPTION("ADC-based battery driver for Samsung Spica");
MODULE_LICENSE("GPL");
