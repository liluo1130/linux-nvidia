/*
 * pwm_fan.c fan driver that is controlled by pwm
 *
 * Copyright (c) 2013-2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Anshul Jain <anshulj@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/therm_est.h>
#include <linux/slab.h>
#include <linux/platform_data/pwm_fan.h>
#include <linux/thermal.h>
#include <linux/mutex.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/pwm.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/of_device.h>
#include <linux/of.h>

struct fan_dev_data {
	int next_state;
	int active_steps;
	int *fan_rpm;
	int *fan_pwm;
	int *fan_rru;
	int *fan_rrd;
	int *fan_state_cap_lookup;
	struct workqueue_struct *workqueue;
	int fan_temp_control_flag;
	struct pwm_device *pwm_dev;
	int fan_cap_pwm;
	int fan_cur_pwm;
	int next_target_pwm;
	struct thermal_cooling_device *cdev;
	struct delayed_work fan_ramp_work;
	int step_time;
	int precision_multiplier;
	struct mutex fan_state_lock;
	int pwm_period;
	int fan_pwm_max;
	struct device *dev;
	int tach_gpio;
	int tach_irq;
	int tach_enabled;
	int fan_state_cap;
	int pwm_gpio;
	int pwm_id;
	const char *name;
};

#ifdef CONFIG_DEBUG_FS
static struct dentry *fan_debugfs_root;

static void fan_update_target_pwm(struct fan_dev_data *fan_data, int val)
{
	if (fan_data) {
		fan_data->next_target_pwm = min(val, fan_data->fan_cap_pwm);

		if (fan_data->next_target_pwm != fan_data->fan_cur_pwm)
			queue_delayed_work(fan_data->workqueue,
					&fan_data->fan_ramp_work,
					msecs_to_jiffies(fan_data->step_time));
	}
}

static int fan_target_pwm_show(void *data, u64 *val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;

	if (!fan_data)
		return -EINVAL;
	mutex_lock(&fan_data->fan_state_lock);
	*val = ((struct fan_dev_data *)data)->next_target_pwm;
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int fan_target_pwm_set(void *data, u64 val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;
	int target_pwm;

	if (!fan_data)
		return -EINVAL;

	if (val >= fan_data->fan_pwm_max)
		val = fan_data->fan_pwm_max - 1;

	mutex_lock(&fan_data->fan_state_lock);
	target_pwm = (int)val;
	fan_update_target_pwm(fan_data, target_pwm);
	mutex_unlock(&fan_data->fan_state_lock);

	return 0;
}

static int fan_temp_control_show(void *data, u64 *val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;

	if (!fan_data)
		return -EINVAL;
	mutex_lock(&fan_data->fan_state_lock);
	*val = fan_data->fan_temp_control_flag;
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int fan_temp_control_set(void *data, u64 val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;

	if (!fan_data)
		return -EINVAL;

	mutex_lock(&fan_data->fan_state_lock);
	fan_data->fan_temp_control_flag = val > 0 ? 1 : 0;
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int fan_tach_enabled_show(void *data, u64 *val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;

	if (!fan_data)
		return -EINVAL;

	*val = fan_data->tach_enabled;

	return 0;
}

static int fan_tach_enabled_set(void *data, u64 val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;

	if (!fan_data)
		return -EINVAL;
	if (fan_data->tach_gpio < 0)
		return -EPERM;

	if (val == 1 && !fan_data->tach_enabled) {
		enable_irq(fan_data->tach_irq);
		fan_data->tach_enabled = val;
	} else if (val == 0 && fan_data->tach_enabled &&
				fan_data->tach_gpio != -1) {
		disable_irq(fan_data->tach_irq);
		fan_data->tach_enabled = val;
	}

	return 0;
}

static int fan_cap_pwm_set(void *data, u64 val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;
	int target_pwm;

	if (!fan_data)
		return -EINVAL;

	if (val >= fan_data->fan_pwm_max)
		val = fan_data->fan_pwm_max - 1;
	mutex_lock(&fan_data->fan_state_lock);
	fan_data->fan_cap_pwm = (int)val;
	target_pwm = min(fan_data->fan_cap_pwm, fan_data->next_target_pwm);
	fan_update_target_pwm(fan_data, target_pwm);
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int fan_cap_pwm_show(void *data, u64 *val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;

	if (!fan_data)
		return -EINVAL;
	mutex_lock(&fan_data->fan_state_lock);
	*val = fan_data->fan_cap_pwm;
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int fan_step_time_set(void *data, u64 val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;

	if (!fan_data)
		return -EINVAL;
	mutex_lock(&fan_data->fan_state_lock);
	fan_data->step_time = val;
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int fan_cur_pwm_show(void *data, u64 *val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;

	if (!fan_data)
		return -EINVAL;
	mutex_lock(&fan_data->fan_state_lock);
	*val = fan_data->fan_cur_pwm;
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int fan_step_time_show(void *data, u64 *val)
{
	struct fan_dev_data *fan_data = (struct fan_dev_data *)data;

	if (!fan_data)
		return -EINVAL;
	mutex_lock(&fan_data->fan_state_lock);
	*val = fan_data->step_time;
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int fan_debugfs_show(struct seq_file *s, void *data)
{
	int i;
	struct fan_dev_data *fan_data = s->private;

	if (!fan_data)
		return -EINVAL;
	seq_puts(s, "(Index, RPM, PWM, RRU, RRD)\n");
	for (i = 0; i < fan_data->active_steps; i++) {
		seq_printf(s, "(%d, %d, %d, %d, %d)\n", i, fan_data->fan_rpm[i],
			fan_data->fan_pwm[i],
			fan_data->fan_rru[i],
			fan_data->fan_rrd[i]);
	}
	return 0;
}

static int fan_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, fan_debugfs_show, inode->i_private);
}

static const struct file_operations fan_rpm_table_fops = {
	.open		= fan_debugfs_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

DEFINE_SIMPLE_ATTRIBUTE(fan_cap_pwm_fops,
			fan_cap_pwm_show,
			fan_cap_pwm_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(fan_temp_control_fops,
			fan_temp_control_show,
			fan_temp_control_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(fan_target_pwm_fops,
			fan_target_pwm_show,
			fan_target_pwm_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(fan_cur_pwm_fops,
			fan_cur_pwm_show,
			NULL, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(fan_tach_enabled_fops,
			fan_tach_enabled_show,
			fan_tach_enabled_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(fan_step_time_fops,
			fan_step_time_show,
			fan_step_time_set, "%llu\n");

static int pwm_fan_debug_init(struct fan_dev_data *fan_data)
{
	fan_debugfs_root = debugfs_create_dir("tegra_fan", 0);

	if (!fan_debugfs_root)
		return -ENOMEM;

	if (!debugfs_create_file("target_pwm", 0644, fan_debugfs_root,
		(void *)fan_data,
		&fan_target_pwm_fops))
		goto err_out;

	if (!debugfs_create_file("temp_control", 0644, fan_debugfs_root,
		(void *)fan_data,
		&fan_temp_control_fops))
		goto err_out;

	if (!debugfs_create_file("pwm_cap", 0644, fan_debugfs_root,
		(void *)fan_data,
		&fan_cap_pwm_fops))
		goto err_out;

	if (!debugfs_create_file("pwm_rpm_table", 0444, fan_debugfs_root,
		(void *)fan_data,
		&fan_rpm_table_fops))
		goto err_out;

	if (!debugfs_create_file("step_time", 0644, fan_debugfs_root,
		(void *)fan_data,
		&fan_step_time_fops))
		goto err_out;

	if (!debugfs_create_file("cur_pwm", 0444, fan_debugfs_root,
		(void *)fan_data,
		&fan_cur_pwm_fops))
		goto err_out;

	if (!debugfs_create_file("tach_enable", 0644, fan_debugfs_root,
		(void *)fan_data,
		&fan_tach_enabled_fops))
		goto err_out;
	return 0;

err_out:
	debugfs_remove_recursive(fan_debugfs_root);
	return -ENOMEM;
}
#else
static inline int pwm_fan_debug_init(struct fan_dev_data *fan_data)
{
	return 0;
}
#endif /* DEBUG_FS*/

static int pwm_fan_get_cur_state(struct thermal_cooling_device *cdev,
						unsigned long *cur_state)
{
	struct fan_dev_data *fan_data = cdev->devdata;

	if (!fan_data)
		return -EINVAL;

	mutex_lock(&fan_data->fan_state_lock);
	*cur_state = fan_data->next_state;
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int pwm_fan_set_cur_state(struct thermal_cooling_device *cdev,
						unsigned long cur_state)
{
	struct fan_dev_data *fan_data = cdev->devdata;
	int target_pwm;

	if (!fan_data)
		return -EINVAL;

	mutex_lock(&fan_data->fan_state_lock);

	if (!fan_data->fan_temp_control_flag) {
		mutex_unlock(&fan_data->fan_state_lock);
		return 0;
	}

	fan_data->next_state = cur_state;

	if (fan_data->next_state <= 0)
		target_pwm = 0;
	else
		target_pwm = fan_data->fan_pwm[cur_state];

	target_pwm = min(fan_data->fan_cap_pwm, target_pwm);
	fan_update_target_pwm(fan_data, target_pwm);

	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int pwm_fan_get_max_state(struct thermal_cooling_device *cdev,
						unsigned long *max_state)
{
	struct fan_dev_data *fan_data = cdev->devdata;

	*max_state = fan_data->active_steps;
	return 0;
}

static struct thermal_cooling_device_ops pwm_fan_cooling_ops = {
	.get_max_state = pwm_fan_get_max_state,
	.get_cur_state = pwm_fan_get_cur_state,
	.set_cur_state = pwm_fan_set_cur_state,
};

static int fan_get_rru(int pwm, struct fan_dev_data *fan_data)
{
	int i;

	for (i = 0; i < fan_data->active_steps - 1 ; i++) {
		if ((pwm >= fan_data->fan_pwm[i]) &&
				(pwm < fan_data->fan_pwm[i + 1])) {
			return fan_data->fan_rru[i];
		}
	}
	return fan_data->fan_rru[fan_data->active_steps - 1];
}

static int fan_get_rrd(int pwm, struct fan_dev_data *fan_data)
{
	int i;

	for (i = 0; i < fan_data->active_steps - 1 ; i++) {
		if ((pwm >= fan_data->fan_pwm[i]) &&
				(pwm < fan_data->fan_pwm[i + 1])) {
			return fan_data->fan_rrd[i];
		}
	}
	return fan_data->fan_rrd[fan_data->active_steps - 1];
}

static void set_pwm_duty_cycle(int pwm, struct fan_dev_data *fan_data)
{
	int duty;

	if (fan_data != NULL && fan_data->pwm_dev != NULL) {
		duty = (fan_data->fan_pwm_max - pwm)
				* fan_data->precision_multiplier;
		pwm_config(fan_data->pwm_dev,
			duty, fan_data->pwm_period);
		pwm_enable(fan_data->pwm_dev);
	} else {
		dev_err(fan_data->dev,
				"FAN:PWM device or fan data is null\n");
	}
}

static int get_next_higher_pwm(int pwm, struct fan_dev_data *fan_data)
{
	int i;

	for (i = 0; i < fan_data->active_steps; i++)
		if (pwm < fan_data->fan_pwm[i])
			return fan_data->fan_pwm[i];

	return fan_data->fan_pwm[fan_data->active_steps - 1];
}

static int get_next_lower_pwm(int pwm, struct fan_dev_data *fan_data)
{
	int i;

	for (i = fan_data->active_steps - 1; i >= 0; i--)
		if (pwm > fan_data->fan_pwm[i])
			return fan_data->fan_pwm[i];

	return fan_data->fan_pwm[0];
}

static void fan_ramping_work_func(struct work_struct *work)
{
	int rru, rrd;
	int cur_pwm, next_pwm;
	struct delayed_work *dwork = container_of(work, struct delayed_work,
									work);
	struct fan_dev_data *fan_data = container_of(dwork, struct
						fan_dev_data, fan_ramp_work);

	if (!fan_data) {
		dev_err(fan_data->dev, "Fan data is null\n");
		return;
	}
	mutex_lock(&fan_data->fan_state_lock);
	cur_pwm = fan_data->fan_cur_pwm;
	rru = fan_get_rru(cur_pwm, fan_data);
	rrd = fan_get_rrd(cur_pwm, fan_data);
	next_pwm = cur_pwm;

	if (fan_data->next_target_pwm > fan_data->fan_cur_pwm) {
		fan_data->fan_cur_pwm = fan_data->fan_cur_pwm + rru;
		next_pwm = min(
				get_next_higher_pwm(cur_pwm, fan_data),
				fan_data->fan_cur_pwm);
		next_pwm = min(fan_data->next_target_pwm, next_pwm);
		next_pwm = min(fan_data->fan_cap_pwm, next_pwm);
	} else if (fan_data->next_target_pwm < fan_data->fan_cur_pwm) {
		fan_data->fan_cur_pwm = fan_data->fan_cur_pwm - rrd;
		next_pwm = max(get_next_lower_pwm(cur_pwm, fan_data),
							fan_data->fan_cur_pwm);
		next_pwm = max(next_pwm, fan_data->next_target_pwm);
		next_pwm = max(0, next_pwm);
	}
	set_pwm_duty_cycle(next_pwm, fan_data);
	fan_data->fan_cur_pwm = next_pwm;
	if (fan_data->next_target_pwm != next_pwm)
		queue_delayed_work(fan_data->workqueue,
				&(fan_data->fan_ramp_work),
				msecs_to_jiffies(fan_data->step_time));
	mutex_unlock(&fan_data->fan_state_lock);
}


static ssize_t show_fan_pwm_cap_sysfs(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct fan_dev_data *fan_data = dev_get_drvdata(dev);
	int ret;

	if (!fan_data)
		return -EINVAL;
	mutex_lock(&fan_data->fan_state_lock);
	ret = sprintf(buf, "%d\n", fan_data->fan_cap_pwm);
	mutex_unlock(&fan_data->fan_state_lock);
	return ret;
}

static ssize_t set_fan_pwm_cap_sysfs(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct fan_dev_data *fan_data = dev_get_drvdata(dev);
	long val;
	int ret;
	int target_pwm;

	ret = kstrtol(buf, 10, &val);

	if (ret < 0)
		return -EINVAL;

	if (!fan_data)
		return -EINVAL;

	mutex_lock(&fan_data->fan_state_lock);

	if (val < 0)
		val = 0;
	else if (val >= fan_data->fan_pwm_max)
		val = fan_data->fan_pwm_max - 1;

	fan_data->fan_cap_pwm = val;
	target_pwm = min(fan_data->fan_cap_pwm, fan_data->next_target_pwm);
	fan_update_target_pwm(fan_data, target_pwm);
	mutex_unlock(&fan_data->fan_state_lock);
	return count;
}

/*State cap sysfs fops*/
static ssize_t show_fan_state_cap_sysfs(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct fan_dev_data *fan_data = dev_get_drvdata(dev);
	int ret;

	if (!fan_data)
		return -EINVAL;
	mutex_lock(&fan_data->fan_state_lock);
	ret = sprintf(buf, "%d\n", fan_data->fan_state_cap);
	mutex_unlock(&fan_data->fan_state_lock);
	return ret;
}

static ssize_t set_fan_state_cap_sysfs(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct fan_dev_data *fan_data = dev_get_drvdata(dev);
	long val;
	int ret;
	int target_pwm;

	ret = kstrtol(buf, 10, &val);

	if (ret < 0 || !fan_data)
		goto error;

	mutex_lock(&fan_data->fan_state_lock);
	if (val < 0)
		val = 0;
	else if (val >= fan_data->active_steps)
		val = fan_data->active_steps - 1;

	fan_data->fan_state_cap = val;
	fan_data->fan_cap_pwm =
		fan_data->fan_pwm[fan_data->fan_state_cap_lookup[val]];
	target_pwm = min(fan_data->fan_cap_pwm, fan_data->next_target_pwm);
	fan_update_target_pwm(fan_data, target_pwm);
	mutex_unlock(&fan_data->fan_state_lock);
	return count;

error:
	dev_err(dev, "%s, fan_data is null or wrong input\n", __func__);
	return -EINVAL;
}


static ssize_t set_fan_pwm_state_map_sysfs(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct fan_dev_data *fan_data = dev_get_drvdata(dev);
	int ret, index, pwm_val;
	int target_pwm;

	ret = sscanf(buf, "%d %d", &index, &pwm_val);

	if (ret < 0)
		return -EINVAL;

	if (!fan_data)
		return -EINVAL;

	dev_dbg(dev, "index=%d, pwm_val=%d", index, pwm_val);

	if ((index < 0) || (index > fan_data->active_steps))
		return -EINVAL;

	mutex_lock(&fan_data->fan_state_lock);

	if ((pwm_val < 0) || (pwm_val >= fan_data->fan_pwm_max)) {
		mutex_unlock(&fan_data->fan_state_lock);
		return -EINVAL;
	}

	fan_data->fan_pwm[index] = pwm_val;

	if (index == fan_data->next_state)
		if (fan_data->next_target_pwm !=
			fan_data->fan_pwm[index]) {
			target_pwm = fan_data->fan_pwm[index];
			target_pwm = min(fan_data->fan_cap_pwm,
						target_pwm);

			fan_update_target_pwm(fan_data, target_pwm);
		}
	mutex_unlock(&fan_data->fan_state_lock);

	return count;
}

static DEVICE_ATTR(pwm_cap, S_IWUSR | S_IRUGO, show_fan_pwm_cap_sysfs,
							set_fan_pwm_cap_sysfs);
static DEVICE_ATTR(state_cap, S_IWUSR | S_IRUGO,
			show_fan_state_cap_sysfs, set_fan_state_cap_sysfs);
static DEVICE_ATTR(pwm_state_map, S_IWUSR | S_IRUGO,
					NULL, set_fan_pwm_state_map_sysfs);

static struct attribute *pwm_fan_attributes[] = {
	&dev_attr_pwm_cap.attr,
	&dev_attr_state_cap.attr,
	&dev_attr_pwm_state_map.attr,
	NULL
};

static const struct attribute_group pwm_fan_group = {
	.attrs = pwm_fan_attributes,
};

static int add_sysfs_entry(struct device *dev)
{
	return sysfs_create_group(&dev->kobj, &pwm_fan_group);
}

static void remove_sysfs_entry(struct device *dev)
{
	sysfs_remove_group(&dev->kobj, &pwm_fan_group);
}

irqreturn_t fan_tach_isr(int irq, void *data)
{
	return IRQ_HANDLED;
}


static int pwm_fan_probe(struct platform_device *pdev)
{
	int i;
	struct fan_dev_data *fan_data = NULL;
	int *rpm_data;
	int *rru_data;
	int *rrd_data;
	int *lookup_data;
	int *pwm_data;
	int err = 0;
	int of_err = 0;
	struct device_node *node = NULL;
	struct device_node *data_node = NULL;
	u32 value;
	int pwm_fan_gpio;
	int gpio_free_flag = 0;

	if (!pdev)
		return -EINVAL;

	node = pdev->dev.of_node;
	if (!node) {
		pr_err("FAN: dev of_node NULL\n");
		return -EINVAL;
	}

	data_node = of_parse_phandle(node, "shared_data", 0);
	if (!data_node) {
		pr_err("PWM shared data node NULL, parse phandle failed\n");
		return -EINVAL;
	}

	fan_data = devm_kzalloc(&pdev->dev,
				sizeof(struct fan_dev_data), GFP_KERNEL);
	if (!fan_data)
		return -ENOMEM;

	fan_data->dev = &pdev->dev;

	of_err |= of_property_read_string(node, "name", &fan_data->name);
	pr_info("FAN dev name: %s\n", fan_data->name);

	of_err |= of_property_read_u32(data_node, "pwm_gpio", &value);
	pwm_fan_gpio = (int)value;

	err = gpio_request(pwm_fan_gpio, "pwm-fan");
	if (err < 0) {
		pr_err("FAN:gpio request failed\n");
		err = -EINVAL;
		goto gpio_request_fail;
	} else {
		pr_info("FAN:gpio request success.\n");
	}

	of_err |= of_property_read_u32(data_node, "active_steps", &value);
	fan_data->active_steps = (int)value;

	of_err |= of_property_read_u32(data_node, "pwm_period", &value);
	fan_data->pwm_period = (int)value;

	of_err |= of_property_read_u32(data_node, "pwm_id", &value);
	fan_data->pwm_id = (int)value;

	of_err |= of_property_read_u32(data_node, "step_time", &value);
	fan_data->step_time = (int)value;

	of_err |= of_property_read_u32(data_node, "active_pwm_max", &value);
	fan_data->fan_pwm_max = (int)value;

	of_err |= of_property_read_u32(data_node, "state_cap", &value);
	fan_data->fan_state_cap = (int)value;

	of_err |= of_property_read_u32(data_node, "tach_gpio", &value);
	fan_data->tach_gpio = (int)value;

	fan_data->pwm_gpio = pwm_fan_gpio;

	if (of_err) {
		err = -ENXIO;
		goto rpm_alloc_fail;
	}

	/* rpm array */
	rpm_data = devm_kzalloc(&pdev->dev,
			sizeof(int) * (fan_data->active_steps), GFP_KERNEL);
	if (!rpm_data) {
		err = -ENOMEM;
		goto rpm_alloc_fail;
	}
	of_err |= of_property_read_u32_array(data_node, "active_rpm", rpm_data,
		(size_t) fan_data->active_steps);
	fan_data->fan_rpm = rpm_data;

	/* rru array */
	rru_data = devm_kzalloc(&pdev->dev,
			sizeof(int) * (fan_data->active_steps), GFP_KERNEL);
	if (!rru_data) {
		err = -ENOMEM;
		goto rru_alloc_fail;
	}
	of_err |= of_property_read_u32_array(data_node, "active_rru", rru_data,
		(size_t) fan_data->active_steps);
	fan_data->fan_rru = rru_data;

	/* rrd array */
	rrd_data = devm_kzalloc(&pdev->dev,
			sizeof(int) * (fan_data->active_steps), GFP_KERNEL);
	if (!rrd_data) {
		err = -ENOMEM;
		goto rrd_alloc_fail;
	}
	of_err |= of_property_read_u32_array(data_node, "active_rrd", rrd_data,
		(size_t) fan_data->active_steps);
	fan_data->fan_rrd = rrd_data;

	/* state_cap_lookup array */
	lookup_data = devm_kzalloc(&pdev->dev,
			sizeof(int) * (fan_data->active_steps), GFP_KERNEL);
	if (!lookup_data) {
		err = -ENOMEM;
		goto lookup_alloc_fail;
	}
	of_err |= of_property_read_u32_array(data_node, "state_cap_lookup",
		lookup_data, (size_t) fan_data->active_steps);
	fan_data->fan_state_cap_lookup = lookup_data;

	/* pwm array */
	pwm_data = devm_kzalloc(&pdev->dev,
			sizeof(int) * (fan_data->active_steps), GFP_KERNEL);
	if (!pwm_data) {
		err = -ENOMEM;
		goto pwm_alloc_fail;
	}
	of_err |= of_property_read_u32_array(node, "active_pwm", pwm_data,
		(size_t) fan_data->active_steps);
	fan_data->fan_pwm = pwm_data;

	if (of_err) {
		err = -ENXIO;
		goto workqueue_alloc_fail;
	}

	mutex_init(&fan_data->fan_state_lock);
	fan_data->workqueue = alloc_workqueue(dev_name(&pdev->dev),
				WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!fan_data->workqueue) {
		err = -ENOMEM;
		goto workqueue_alloc_fail;
	}

	INIT_DELAYED_WORK(&(fan_data->fan_ramp_work), fan_ramping_work_func);

	fan_data->fan_cap_pwm = fan_data->fan_pwm[fan_data->fan_state_cap];
	fan_data->precision_multiplier =
			fan_data->pwm_period / fan_data->fan_pwm_max;
	dev_info(&pdev->dev, "cap state:%d, cap pwm:%d\n",
			fan_data->fan_state_cap, fan_data->fan_cap_pwm);

	fan_data->cdev =
		thermal_cooling_device_register("pwm-fan",
					fan_data, &pwm_fan_cooling_ops);

	if (IS_ERR_OR_NULL(fan_data->cdev)) {
		dev_err(&pdev->dev, "Failed to register cooling device\n");
		err = -EINVAL;
		goto cdev_register_fail;
	}

	fan_data->pwm_dev = pwm_request(fan_data->pwm_id, dev_name(&pdev->dev));
	if (IS_ERR_OR_NULL(fan_data->pwm_dev)) {
		dev_err(&pdev->dev, "unable to request PWM for fan\n");
		err = -ENODEV;
		goto pwm_req_fail;
	} else {
		dev_info(&pdev->dev, "got pwm for fan\n");
	}

	fan_data->tach_enabled = 0;
	gpio_free(fan_data->pwm_gpio);
	gpio_free_flag = 1;
	if (fan_data->tach_gpio != -1) {
		/* init fan tach */
		fan_data->tach_irq = gpio_to_irq(fan_data->tach_gpio);
		err = gpio_request(fan_data->tach_gpio, "pwm-fan-tach");
		if (err < 0) {
			dev_err(&pdev->dev, "fan tach gpio request failed\n");
			goto tach_gpio_request_fail;
		}

		gpio_free_flag = 0;
		err = gpio_direction_input(fan_data->tach_gpio);
		if (err < 0) {
			dev_err(&pdev->dev, "fan tach set gpio direction input failed\n");
			goto tach_request_irq_fail;
		}

		err = gpio_sysfs_set_active_low(fan_data->tach_gpio, 1);
		if (err < 0) {
			dev_err(&pdev->dev, "fan tach set gpio active low failed\n");
			goto tach_request_irq_fail;
		}

		err = request_irq(fan_data->tach_irq, fan_tach_isr,
			IRQF_TRIGGER_FALLING , "pwm-fan-tach", NULL);
		if (err < 0)
			goto tach_request_irq_fail;
		disable_irq_nosync(fan_data->tach_irq);
	}

	platform_set_drvdata(pdev, fan_data);

	/*turn temp control on*/
	fan_data->fan_temp_control_flag = 1;
	set_pwm_duty_cycle(fan_data->fan_pwm[0], fan_data);

	if (add_sysfs_entry(&pdev->dev) < 0) {
		dev_err(&pdev->dev, "FAN:Can't create syfs node");
		err = -ENOMEM;
		goto sysfs_fail;
	}

	if (pwm_fan_debug_init(fan_data) < 0) {
		dev_err(&pdev->dev, "FAN:Can't create debug fs nodes");
		/*Just continue without debug fs*/
	}

	/* print out initialized info */
	for (i = 0; i < fan_data->active_steps; i++) {
		dev_info(&pdev->dev,
			"index %d: pwm=%d, rpm=%d, rru=%d, rrd=%d, state:%d\n",
			i,
			fan_data->fan_pwm[i],
			fan_data->fan_rpm[i],
			fan_data->fan_rru[i],
			fan_data->fan_rrd[i],
			fan_data->fan_state_cap_lookup[i]);
	}
	return err;

sysfs_fail:
	free_irq(fan_data->tach_irq, NULL);
tach_request_irq_fail:
tach_gpio_request_fail:
	pwm_free(fan_data->pwm_dev);
pwm_req_fail:
	thermal_cooling_device_unregister(fan_data->cdev);
cdev_register_fail:
	destroy_workqueue(fan_data->workqueue);
workqueue_alloc_fail:
	devm_kfree(&pdev->dev, (void *)pwm_data);
pwm_alloc_fail:
	devm_kfree(&pdev->dev, (void *)lookup_data);
lookup_alloc_fail:
	devm_kfree(&pdev->dev, (void *)rrd_data);
rrd_alloc_fail:
	devm_kfree(&pdev->dev, (void *)rru_data);
rru_alloc_fail:
	devm_kfree(&pdev->dev, (void *)rpm_data);
rpm_alloc_fail:
	if (!gpio_free_flag)
		gpio_free(fan_data->pwm_gpio);
gpio_request_fail:
	devm_kfree(&pdev->dev, (void *)fan_data);
	if (err == -ENXIO)
		pr_err("FAN: of_property_read failed\n");
	else if (err == -ENOMEM)
		pr_err("FAN: memery allocation failed\n");
	return err;
}

static int pwm_fan_remove(struct platform_device *pdev)
{
	struct fan_dev_data *fan_data = platform_get_drvdata(pdev);

	if (!fan_data)
		return -EINVAL;
	debugfs_remove_recursive(fan_debugfs_root);
	free_irq(fan_data->tach_irq, NULL);
	gpio_free(fan_data->tach_gpio);
	pwm_config(fan_data->pwm_dev, 0, fan_data->pwm_period);
	pwm_disable(fan_data->pwm_dev);
	pwm_free(fan_data->pwm_dev);
	thermal_cooling_device_unregister(fan_data->cdev);
	remove_sysfs_entry(&pdev->dev);
	return 0;
}

#if CONFIG_PM
static int pwm_fan_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct fan_dev_data *fan_data = platform_get_drvdata(pdev);
	int err;

	mutex_lock(&fan_data->fan_state_lock);
	cancel_delayed_work(&fan_data->fan_ramp_work);
	/*Turn the fan off*/
	fan_data->fan_cur_pwm = 0;
	fan_data->next_target_pwm = 0;

	set_pwm_duty_cycle(0, fan_data);
	pwm_disable(fan_data->pwm_dev);
	pwm_free(fan_data->pwm_dev);

	err = gpio_request(fan_data->pwm_gpio, "pwm-fan");
	if (err < 0) {
		dev_err(&pdev->dev, "%s:gpio request failed %d\n",
			__func__, fan_data->pwm_gpio);
	}

	gpio_direction_output(fan_data->pwm_gpio, 1);

	/*Stop thermal control*/
	fan_data->fan_temp_control_flag = 0;
	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}

static int pwm_fan_resume(struct platform_device *pdev)
{
	struct fan_dev_data *fan_data = platform_get_drvdata(pdev);

	/*Sanity check, want to make sure fan is off when the driver resumes*/
	mutex_lock(&fan_data->fan_state_lock);

	gpio_free(fan_data->pwm_gpio);
	fan_data->pwm_dev = pwm_request(fan_data->pwm_id, dev_name(&pdev->dev));
	if (IS_ERR_OR_NULL(fan_data->pwm_dev)) {
		dev_err(&pdev->dev, " %s: unable to request PWM for fan\n",
		__func__);
		mutex_unlock(&fan_data->fan_state_lock);
		return -ENODEV;
	} else {
		dev_info(&pdev->dev, " %s, got pwm for fan\n", __func__);
	}

	set_pwm_duty_cycle(0, fan_data);
	/*Start thermal control*/
	fan_data->fan_temp_control_flag = 1;

	mutex_unlock(&fan_data->fan_state_lock);
	return 0;
}
#endif


static const struct of_device_id of_pwm_fan_match[] = {
	{ .compatible = "loki-pwm-fan", },
	{ .compatible = "foster-pwm-fan", },
	{},
};
MODULE_DEVICE_TABLE(of, of_pwm_fan_match);

static struct platform_driver pwm_fan_driver = {
	.driver = {
		.name	= "pwm_fan_driver",
		.owner = THIS_MODULE,
		.of_match_table = of_pwm_fan_match,
	},
	.probe = pwm_fan_probe,
	.remove = pwm_fan_remove,
#if CONFIG_PM
	.suspend = pwm_fan_suspend,
	.resume = pwm_fan_resume,
#endif
};

module_platform_driver(pwm_fan_driver);

MODULE_DESCRIPTION("pwm fan driver");
MODULE_AUTHOR("Anshul Jain <anshulj@nvidia.com>");
MODULE_LICENSE("GPL v2");
