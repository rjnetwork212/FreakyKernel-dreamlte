/*
 *  sec_fuelgauge.c
 *  Samsung Mobile Fuel Gauge Driver
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define DEBUG

#include <linux/battery/sec_fuelgauge.h>
#include <linux/of_gpio.h>

static struct device_attribute sec_fg_attrs[] = {
	SEC_FG_ATTR(reg),
	SEC_FG_ATTR(data),
	SEC_FG_ATTR(regs),
};

static enum power_supply_property sec_fuelgauge_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_AMBIENT,
};

/* capacity is  0.1% unit */
static void sec_fg_get_scaled_capacity(
				struct sec_fuelgauge_info *fuelgauge,
				union power_supply_propval *val)
{
	val->intval = (val->intval < fuelgauge->pdata->capacity_min) ?
		0 : ((val->intval - fuelgauge->pdata->capacity_min) * 1000 /
		(fuelgauge->capacity_max - fuelgauge->pdata->capacity_min));

	dev_dbg(&fuelgauge->client->dev,
		"%s: scaled capacity (%d.%d)\n",
		__func__, val->intval/10, val->intval%10);
}

/* capacity is integer */
static void sec_fg_get_atomic_capacity(
				struct sec_fuelgauge_info *fuelgauge,
				union power_supply_propval *val)
{
	if (fuelgauge->pdata->capacity_calculation_type &
		SEC_FUELGAUGE_CAPACITY_TYPE_ATOMIC) {
	if (fuelgauge->capacity_old < val->intval)
		val->intval = fuelgauge->capacity_old + 1;
	else if (fuelgauge->capacity_old > val->intval)
		val->intval = fuelgauge->capacity_old - 1;
	}

	/* keep SOC stable in abnormal status */
	if (fuelgauge->pdata->capacity_calculation_type &
		SEC_FUELGAUGE_CAPACITY_TYPE_SKIP_ABNORMAL) {
		if (!fuelgauge->is_charging &&
			fuelgauge->capacity_old < val->intval) {
			dev_err(&fuelgauge->client->dev,
				"%s: capacity (old %d : new %d)\n",
				__func__, fuelgauge->capacity_old, val->intval);
			val->intval = fuelgauge->capacity_old;
		}
	}

	/* updated old capacity */
	fuelgauge->capacity_old = val->intval;
}

static int sec_fg_get_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct sec_fuelgauge_info *fuelgauge =
		container_of(psy, struct sec_fuelgauge_info, psy_fg);
	int soc_type = val->intval;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_CURRENT_AVG:
	case POWER_SUPPLY_PROP_ENERGY_NOW:
	case POWER_SUPPLY_PROP_CAPACITY:
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		if (!sec_hal_fg_get_property(fuelgauge->client, psp, val))
			return -EINVAL;
		if (psp == POWER_SUPPLY_PROP_CAPACITY) {
			if (soc_type == SEC_FUELGAUGE_CAPACITY_TYPE_RAW)
				break;

			if (fuelgauge->pdata->capacity_calculation_type &
				(SEC_FUELGAUGE_CAPACITY_TYPE_SCALE |
				 SEC_FUELGAUGE_CAPACITY_TYPE_DYNAMIC_SCALE))
				sec_fg_get_scaled_capacity(fuelgauge, val);

			/* capacity should be between 0% and 100%
			 * (0.1% degree)
			 */
			if (val->intval > 1000)
				val->intval = 1000;
			if (val->intval < 0)
				val->intval = 0;

			/* get only integer part */
			val->intval /= 10;

			/* check whether doing the wake_unlock */
			if ((val->intval > fuelgauge->pdata->fuel_alert_soc) &&
				fuelgauge->is_fuel_alerted) {
				wake_unlock(&fuelgauge->fuel_alert_wake_lock);
				sec_hal_fg_fuelalert_init(fuelgauge->client,
					fuelgauge->pdata->fuel_alert_soc);
			}
			
			/* (Only for atomic capacity)
			 * In initial time, capacity_old is 0.
			 * and in resume from sleep,
			 * capacity_old is too different from actual soc.
			 * should update capacity_old
			 * by val->intval in booting or resume.
			 */
			if (fuelgauge->initial_update_of_soc) {
				/* updated old capacity */
				fuelgauge->capacity_old = val->intval;
				fuelgauge->initial_update_of_soc = false;
				break;
			}

			if (fuelgauge->pdata->capacity_calculation_type &
				(SEC_FUELGAUGE_CAPACITY_TYPE_ATOMIC |
				 SEC_FUELGAUGE_CAPACITY_TYPE_SKIP_ABNORMAL))
				sec_fg_get_atomic_capacity(fuelgauge, val);
		}
		break;
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		return -ENODATA;
	default:
		return -EINVAL;
	}
	return 0;
}

static int sec_fg_calculate_dynamic_scale(
				struct sec_fuelgauge_info *fuelgauge)
{
	union power_supply_propval raw_soc_val;

	raw_soc_val.intval = SEC_FUELGAUGE_CAPACITY_TYPE_RAW;
	if (!sec_hal_fg_get_property(fuelgauge->client,
		POWER_SUPPLY_PROP_CAPACITY,
		&raw_soc_val))
		return -EINVAL;
	raw_soc_val.intval /= 10;

	if (raw_soc_val.intval <
		fuelgauge->pdata->capacity_max -
		fuelgauge->pdata->capacity_max_margin) {
		fuelgauge->capacity_max =
			fuelgauge->pdata->capacity_max -
			fuelgauge->pdata->capacity_max_margin;
		dev_dbg(&fuelgauge->client->dev, "%s: capacity_max (%d)",
			__func__, fuelgauge->capacity_max);
	} else {
		fuelgauge->capacity_max =
			(raw_soc_val.intval >
			fuelgauge->pdata->capacity_max +
			fuelgauge->pdata->capacity_max_margin) ?
			(fuelgauge->pdata->capacity_max +
			fuelgauge->pdata->capacity_max_margin) :
			raw_soc_val.intval;
		dev_dbg(&fuelgauge->client->dev, "%s: raw soc (%d)",
			__func__, fuelgauge->capacity_max);
	}

	fuelgauge->capacity_max =
		(fuelgauge->capacity_max * 99 / 100);

	/* update capacity_old for sec_fg_get_atomic_capacity algorithm */
	fuelgauge->capacity_old = 100;

	dev_info(&fuelgauge->client->dev, "%s: %d is used for capacity_max\n",
		__func__, fuelgauge->capacity_max);

	return fuelgauge->capacity_max;
}

static int sec_fg_set_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    const union power_supply_propval *val)
{
	struct sec_fuelgauge_info *fuelgauge =
		container_of(psy, struct sec_fuelgauge_info, psy_fg);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (val->intval == POWER_SUPPLY_STATUS_FULL)
			sec_hal_fg_full_charged(fuelgauge->client);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (val->intval == POWER_SUPPLY_TYPE_BATTERY) {
			if (fuelgauge->pdata->capacity_calculation_type &
				SEC_FUELGAUGE_CAPACITY_TYPE_DYNAMIC_SCALE)
				sec_fg_calculate_dynamic_scale(fuelgauge);
		}
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		fuelgauge->cable_type = val->intval;
		if (val->intval == POWER_SUPPLY_TYPE_BATTERY)
			fuelgauge->is_charging = false;
		else
			fuelgauge->is_charging = true;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (val->intval == SEC_FUELGAUGE_CAPACITY_TYPE_RESET) {
			fuelgauge->initial_update_of_soc = true;
			if (!sec_hal_fg_reset(fuelgauge->client))
				return -EINVAL;
			else
				break;
		}
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		if (!sec_hal_fg_set_property(fuelgauge->client, psp, val))
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void sec_fg_isr_work(struct work_struct *work)
{
	struct sec_fuelgauge_info *fuelgauge =
		container_of(work, struct sec_fuelgauge_info, isr_work.work);

	/* process for fuel gauge chip */
	sec_hal_fg_fuelalert_process(fuelgauge, fuelgauge->is_fuel_alerted);

	/* process for others */
	if (fuelgauge->pdata->fuelalert_process != NULL)
		fuelgauge->pdata->fuelalert_process(fuelgauge->is_fuel_alerted);
}

static irqreturn_t sec_fg_irq_thread(int irq, void *irq_data)
{
	struct sec_fuelgauge_info *fuelgauge = irq_data;
	bool fuel_alerted;

	if (fuelgauge->pdata->fuel_alert_soc >= 0) {
		fuel_alerted =
			sec_hal_fg_is_fuelalerted(fuelgauge->client);

		dev_info(&fuelgauge->client->dev,
			"%s: Fuel-alert %salerted!\n",
			__func__, fuel_alerted ? "" : "NOT ");

		if (fuel_alerted == fuelgauge->is_fuel_alerted) {
			if (!fuelgauge->pdata->repeated_fuelalert) {
				dev_dbg(&fuelgauge->client->dev,
					"%s: Fuel-alert Repeated (%d)\n",
					__func__, fuelgauge->is_fuel_alerted);
				return IRQ_HANDLED;
			}
		}

		if (fuel_alerted)
			wake_lock(&fuelgauge->fuel_alert_wake_lock);
		else
			wake_unlock(&fuelgauge->fuel_alert_wake_lock);

		queue_delayed_work(system_power_efficient_wq, &fuelgauge->isr_work, 0);

		fuelgauge->is_fuel_alerted = fuel_alerted;
	}

	return IRQ_HANDLED;
}

static int sec_fg_create_attrs(struct device *dev)
{
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(sec_fg_attrs); i++) {
		rc = device_create_file(dev, &sec_fg_attrs[i]);
		if (rc)
			goto create_attrs_failed;
	}
	goto create_attrs_succeed;

create_attrs_failed:
	dev_err(dev, "%s: failed (%d)\n", __func__, rc);
	while (i--)
		device_remove_file(dev, &sec_fg_attrs[i]);
create_attrs_succeed:
	return rc;
}

ssize_t sec_fg_show_attrs(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	const ptrdiff_t offset = attr - sec_fg_attrs;
	int i = 0;

	switch (offset) {
	case FG_REG:
	case FG_DATA:
	case FG_REGS:
		i = sec_hal_fg_show_attrs(dev, offset, buf);
		break;
	default:
		i = -EINVAL;
		break;
	}

	return i;
}

ssize_t sec_fg_store_attrs(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	const ptrdiff_t offset = attr - sec_fg_attrs;
	int ret = 0;

	switch (offset) {
	case FG_REG:
	case FG_DATA:
		ret = sec_hal_fg_store_attrs(dev, offset, buf, count);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

#ifdef CONFIG_OF
static int fuelgauge_parse_dt(struct device *dev,
			struct sec_fuelgauge_info *fuelgauge)
{
	struct device_node *np = dev->of_node;
	sec_battery_platform_data_t *pdata = fuelgauge->pdata;
	int ret;
#if 0
	int ta_int_gpio;
	sec_battery_platform_data_t sec_battery_pdata;
#endif

	/* reset, irq gpio info */
	if (np == NULL) {
		pr_err("%s np NULL\n", __func__);
	} else {
		pdata->fg_irq = of_get_named_gpio(np, "fuelgauge,fuel_int", 0);
		if (pdata->fg_irq < 0) {
			pr_err("%s error reading fg_irq = %d\n", __func__, pdata->fg_irq);
			pdata->fg_irq = 0;
		}

		ret = of_property_read_u32(np, "fuelgauge,capacity_max",
				&pdata->capacity_max);
		if (ret < 0)
			pr_err("%s error reading capacity_max %d\n", __func__, ret);

		ret = of_property_read_u32(np, "fuelgauge,capacity_max_margin",
				&pdata->capacity_max_margin);
		if (ret < 0)
			pr_err("%s error reading capacity_max_margin %d\n", __func__, ret);

		ret = of_property_read_u32(np, "fuelgauge,capacity_min",
				&pdata->capacity_min);
		if (ret < 0)
			pr_err("%s error reading capacity_min %d\n", __func__, ret);

		ret = of_property_read_u32(np, "fuelgauge,capacity_calculation_type",
				&pdata->capacity_calculation_type);
		if (ret < 0)
			pr_err("%s error reading capacity_calculation_type %d\n",
					__func__, ret);
		ret = of_property_read_u32(np, "fuelgauge,fuel_alert_soc",
				&pdata->fuel_alert_soc);
		if (ret < 0)
			pr_err("%s error reading pdata->fuel_alert_soc %d\n",
					__func__, ret);
		pdata->repeated_fuelalert = of_property_read_bool(np,
				"fuelgaguge,repeated_fuelalert");

		pr_info("%s fg_irq: %d, capacity_max: %d\n"
			"cpacity_max_margin: %d, capacity_min: %d\n"
			"calculation_type: 0x%x, fuel_alert_soc: %d,\n"
			"repeated_fuelalert: %d\n",
			__func__, pdata->fg_irq,
			pdata->capacity_max, pdata->capacity_max_margin,
			pdata->capacity_min, pdata->capacity_calculation_type,
			pdata->fuel_alert_soc, pdata->repeated_fuelalert);
	}
	return 0;
}
#endif

static int __devinit sec_fuelgauge_probe(struct i2c_client *client,
						const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct sec_fuelgauge_info *fuelgauge;
	sec_battery_platform_data_t *pdata = NULL;
	struct battery_data_t *battery_data = NULL;
	int ret = 0;
	union power_supply_propval raw_soc_val;

	dev_info(&client->dev,
		"%s: SEC Fuelgauge Driver Loading\n", __func__);

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE))
		return -EIO;

	fuelgauge = kzalloc(sizeof(*fuelgauge), GFP_KERNEL);
	if (!fuelgauge)
		return -ENOMEM;

	mutex_init(&fuelgauge->fg_lock);

	fuelgauge->client = client;
	
	if (client->dev.of_node) {
		int error;
		pdata = devm_kzalloc(&client->dev,
			sizeof(sec_battery_platform_data_t),
				GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			ret = -ENOMEM;
			goto err_free;
		}
		battery_data = devm_kzalloc(&client->dev,
			sizeof(struct battery_data_t),
				GFP_KERNEL);
		if (!battery_data) {
			dev_err(&client->dev, "Failed to allocate memory\n");
			devm_kfree(&client->dev, pdata);
			ret = -ENOMEM;
			goto err_free;
		}
		pdata->battery_data = (void *)battery_data;
		fuelgauge->pdata = pdata;
		error = fuelgauge_parse_dt(&client->dev, fuelgauge);
		if (error) {
			dev_err(&client->dev,
				"%s: Failed to get fuel_int\n", __func__);
		}
	} else	{
		dev_err(&client->dev,
			"%s: Failed to get of_node\n", __func__);
		fuelgauge->pdata = client->dev.platform_data;
	}

	i2c_set_clientdata(client, fuelgauge);

	if (!sec_hal_fg_init(fuelgauge->client)) {
		dev_err(&client->dev,
			"%s: Failed to Initialize Fuelgauge\n", __func__);
		goto err_free;
	}

	fuelgauge->psy_fg.name		= "sec-fuelgauge";
	fuelgauge->psy_fg.type		= POWER_SUPPLY_TYPE_UNKNOWN;
	fuelgauge->psy_fg.get_property	= sec_fg_get_property;
	fuelgauge->psy_fg.set_property	= sec_fg_set_property;
	fuelgauge->psy_fg.properties	= sec_fuelgauge_props;
	fuelgauge->psy_fg.num_properties =
		ARRAY_SIZE(sec_fuelgauge_props);
	fuelgauge->capacity_max = fuelgauge->pdata->capacity_max;
	raw_soc_val.intval = SEC_FUELGAUGE_CAPACITY_TYPE_RAW;
	sec_hal_fg_get_property(fuelgauge->client,
			POWER_SUPPLY_PROP_CAPACITY, &raw_soc_val);
	raw_soc_val.intval /= 10;
	if(raw_soc_val.intval > fuelgauge->pdata->capacity_max)
		sec_fg_calculate_dynamic_scale(fuelgauge);
/*
	if (!fuelgauge->pdata->fg_gpio_init()) {
		dev_err(&client->dev,
			"%s: Failed to Initialize GPIO\n", __func__);
		goto err_free;
	}
*/
	ret = power_supply_register(&client->dev, &fuelgauge->psy_fg);
	if (ret) {
		dev_err(&client->dev,
			"%s: Failed to Register psy_fg\n", __func__);
		goto err_free;
	}

	if (fuelgauge->pdata->fg_irq) {
		fuelgauge->fg_irq = gpio_to_irq(fuelgauge->pdata->fg_irq);
		INIT_DELAYED_WORK(
			&fuelgauge->isr_work, sec_fg_isr_work);

		ret = request_threaded_irq(fuelgauge->fg_irq,
				NULL, sec_fg_irq_thread,
			        IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				"fuelgauge-irq", fuelgauge);
		if (ret) {
			dev_err(&client->dev,
				"%s: Failed to Reqeust IRQ\n", __func__);
			goto err_supply_unreg;
		}

		ret = enable_irq_wake(fuelgauge->fg_irq);
		if (ret < 0)
			dev_err(&client->dev,
				"%s: Failed to Enable Wakeup Source(%d)\n",
				__func__, ret);
	}

	fuelgauge->is_fuel_alerted = false;
	if (fuelgauge->pdata->fuel_alert_soc >= 0) {
		if (sec_hal_fg_fuelalert_init(fuelgauge->client,
			fuelgauge->pdata->fuel_alert_soc))
			wake_lock_init(&fuelgauge->fuel_alert_wake_lock,
				WAKE_LOCK_SUSPEND, "fuel_alerted");
		else {
			dev_err(&client->dev,
				"%s: Failed to Initialize Fuel-alert\n",
				__func__);
			goto err_irq;
		}
	}

	fuelgauge->initial_update_of_soc = true;

	ret = sec_fg_create_attrs(fuelgauge->psy_fg.dev);
	if (ret) {
		dev_err(&client->dev,
			"%s : Failed to create_attrs\n", __func__);
		goto err_irq;
	}

	dev_info(&client->dev,
		"%s: SEC Fuelgauge Driver Loaded\n", __func__);
	return 0;

err_irq:
	if (fuelgauge->fg_irq)
		free_irq(fuelgauge->fg_irq, fuelgauge);
	wake_lock_destroy(&fuelgauge->fuel_alert_wake_lock);
err_supply_unreg:
	power_supply_unregister(&fuelgauge->psy_fg);
err_free:
	mutex_destroy(&fuelgauge->fg_lock);
	kfree(fuelgauge);

	return ret;
}

static int __devexit sec_fuelgauge_remove(
						struct i2c_client *client)
{
	struct sec_fuelgauge_info *fuelgauge = i2c_get_clientdata(client);

	if (fuelgauge->pdata->fuel_alert_soc >= 0)
		wake_lock_destroy(&fuelgauge->fuel_alert_wake_lock);

	return 0;
}

static int sec_fuelgauge_suspend(struct device *dev)
{
	struct sec_fuelgauge_info *fuelgauge = dev_get_drvdata(dev);

	if (!sec_hal_fg_suspend(fuelgauge->client))
		dev_err(&fuelgauge->client->dev,
			"%s: Failed to Suspend Fuelgauge\n", __func__);

	return 0;
}

static int sec_fuelgauge_resume(struct device *dev)
{
	struct sec_fuelgauge_info *fuelgauge = dev_get_drvdata(dev);

	if (!sec_hal_fg_resume(fuelgauge->client))
		dev_err(&fuelgauge->client->dev,
			"%s: Failed to Resume Fuelgauge\n", __func__);

	fuelgauge->initial_update_of_soc = true;

	return 0;
}

static void sec_fuelgauge_shutdown(struct i2c_client *client)
{
}

static const struct i2c_device_id sec_fuelgauge_id[] = {
	{"sec-fuelgauge", 0},
	{}
};

static const struct dev_pm_ops sec_fuelgauge_pm_ops = {
	.suspend = sec_fuelgauge_suspend,
	.resume  = sec_fuelgauge_resume,
};

MODULE_DEVICE_TABLE(i2c, sec_fuelgauge_id);

#if defined(CONFIG_OF)
static struct of_device_id sec_fuelgauge_i2c_dt_ids[] = {
	{ .compatible = "sec-fuelgauge,i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, sec_fuelgauge_i2c_dt_ids);
#endif /* CONFIG_OF */

static struct i2c_driver sec_fuelgauge_driver = {
	.driver = {
		   .name = "sec-fuelgauge",
		   .owner = THIS_MODULE,
#ifdef CONFIG_PM
			.pm = &sec_fuelgauge_pm_ops,
#endif
#if defined(CONFIG_OF)
		.of_match_table	= sec_fuelgauge_i2c_dt_ids,
#endif /* CONFIG_OF */
		   },
	.probe	= sec_fuelgauge_probe,
	.remove	= __devexit_p(sec_fuelgauge_remove),
	.shutdown   = sec_fuelgauge_shutdown,
	.id_table   = sec_fuelgauge_id,
};

static int __init sec_fuelgauge_init(void)
{
	dev_info(0, "%s: \n", __func__);
	return i2c_add_driver(&sec_fuelgauge_driver);
}

static void __exit sec_fuelgauge_exit(void)
{
	i2c_del_driver(&sec_fuelgauge_driver);
}

module_init(sec_fuelgauge_init);
module_exit(sec_fuelgauge_exit);

MODULE_DESCRIPTION("Samsung Fuel Gauge Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
