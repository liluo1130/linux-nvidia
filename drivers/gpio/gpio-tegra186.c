/*
 * GPIO driver for NVIDIA Tegra186
 *
 * Copyright (c) 2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Suresh Mangipudi <smangipudi@nvidia.com>
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

#include <linux/err.h>
#include <linux/init.h>
#include <linux/platform_data/gpio-tegra.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/clk.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm.h>
#include <linux/syscore_ops.h>
#include <linux/tegra-soc.h>
#include <linux/irqchip/tegra.h>
#include <linux/clk/tegra.h>
#include <linux/reset.h>

#define GPIO_ENB_CONFIG_REG	0x00
#define GPIO_ENB_BIT		BIT(0)
#define GPIO_INOUT_BIT		BIT(1)
#define GPIO_TRG_TYPE_BIT(x)	(0x3 & (x))
#define GPIO_TRG_LVL_BIT	0x4
#define GPIO_DEB_FUNC_BIT	BIT(5)
#define GPIO_INT_FUNC_BIT	BIT(6)
#define GPIO_TIMESTMP_FUNC_BIT	0x7

#define GPIO_DBC_THRES_REG	0x04
#define GPIO_DBC_THRES_BIT(val)	((val) & 0xFF)
#define GPIO_INPUT_REG		0x08
#define GPIO_OUT_CTRL_REG	0x0c
#define GPIO_OUT_VAL_REG	0x10
#define GPIO_INT_CLEAR_REG	0x14

#define GPIO_REG_DIFF		0x20

#define GPIO_SCR_REG		0x04
#define GPIO_SCR_DIFF		0x08
#define GPIO_SCR_BASE_DIFF	0x40

#define GPIO_CONTROLLERS_DIFF	0x1000
#define GPIO_SCR_SEC_WEN	BIT(28)
#define GPIO_SCR_SEC_REN	BIT(27)
#define GPIO_SCR_SEC_G1R	BIT(1)
#define GPIO_SCR_SEC_G1W	BIT(9)

#define DEFAULT_IRQ_OFFSET	32

#define GPIO_INT_LVL_NO_TRIGGER			0x0
#define GPIO_INT_LVL_LEVEL_TRIGGER		0x1
#define GPIO_INT_LVL_SINGLE_EDGE_TRIGGER	0x2
#define GPIO_INT_LVL_BOTH_EDGE_TRIGGER		0x3

#define TRIGGER_LEVEL_LOW		0x0
#define TRIGGER_LEVEL_HIGH		0x1

#define GPIO_INT_STATUS_OFFSET		0x100
#define GPIO_STATUS_G1			0x04

#define GPIO_FULL_ACCESS	(GPIO_SCR_SEC_WEN | GPIO_SCR_SEC_REN |	\
					GPIO_SCR_SEC_G1R | GPIO_SCR_SEC_G1W)

#define MAX_GPIO_CONTROLLERS		7
#define MAX_GPIO_PORTS			8
#define MAX_GPIO_CAR_CTRL		6

#define MAX_PORTS			32
#define MAX_PINS_PER_PORT		8

#define GPIO_PORT(g)			((g) >> 3)
#define GPIO_PIN(g)			((g) & 0x7)

struct tegra_gpio_controller {
	int controller;
	int irq;
	spinlock_t lvl_lock[4];
	u32 cnf[MAX_PORTS * MAX_PINS_PER_PORT];
	u32 dbc[MAX_PORTS * MAX_PINS_PER_PORT];
	u32 out_ctrl[MAX_PORTS * MAX_PINS_PER_PORT];
	u32 out_val[MAX_PORTS * MAX_PINS_PER_PORT];
};

struct tegra_gpio {
	struct device *dev;
	const struct tegra_pinctrl_soc_data *soc;

	int nbanks;
	void __iomem **regs;
	int *regs_size;
	unsigned int *reg_base;
	struct clk *gpio_clk[MAX_GPIO_CAR_CTRL];
	struct reset_control *gpio_rst[MAX_GPIO_CAR_CTRL];
};

static struct tegra_gpio *tegra_gpio;

/*
 * Below table is mapping for contoller and ports contained
 * in each port of the give gpio controller
 */
static int tegra186_gpio_map[MAX_GPIO_CONTROLLERS][MAX_GPIO_PORTS] = {
	{13, 14, 16, 19, 8, 17, -1, -1}, /* gpio cntrlr 0 */
	{7, 11, 23, 24, -1, -1, -1, -1}, /* gpio cntrlr 1 */
	{0, 4, 5, 27, -1, -1, -1, -1,}, /* gpio cntrlr 2 */
	{1, 2, 3, -1, -1, -1, -1, -1,}, /* gpio cntrlr 3 */
	{15, 6, -1, -1, -1, -1, -1, -1},/* gpio cntrlr 4 */
	{9, 10, 28, 12, -1, -1, -1, -1},/* gpio cntrlr 5 */
	{31, 18, 20, 30, 21, 22, 26, 25},/* AON gpio cntrlr */
};

static u32 address_map[32][2];
static u32 tegra_gpio_bank_count;
static struct tegra_gpio_controller
		tegra_gpio_controllers[MAX_GPIO_CONTROLLERS];

static struct irq_domain *irq_domain;

static inline u32 controller_index(u32 gpio)
{
	int i, j;
	u32 gp = GPIO_PORT(gpio);

	for (i = 0; i < MAX_GPIO_CONTROLLERS; i++) {
		for (j = 0; j < MAX_GPIO_PORTS; j++) {
			if (tegra186_gpio_map[i][j] == gp)
				return i;
		}
	}
	return -1;
}

static inline u32 tegra_gpio_readl(u32 gpio, u32 reg_offset)
{
	int port = GPIO_PORT(gpio);
	int pin = GPIO_PIN(gpio);
	u32 addr;

	addr = address_map[port][1] + (GPIO_REG_DIFF * pin) + reg_offset;
	return __raw_readl((tegra_gpio->regs[address_map[port][0]]) + addr);
}

static inline void tegra_gpio_writel(u32 val, u32 gpio, u32 reg_offset)
{
	int port = GPIO_PORT(gpio);
	int pin = GPIO_PIN(gpio);
	u32 addr;

	addr = address_map[port][1] + (GPIO_REG_DIFF * pin) + reg_offset;
	__raw_writel(val, (tegra_gpio->regs[address_map[port][0]]) + addr);
}

static inline void tegra_gpio_update(u32 gpio, u32 reg_offset,
		u32 mask, u32 val)
{
	int port = GPIO_PORT(gpio);
	int pin = GPIO_PIN(gpio);
	u32 addr;
	u32 rval;

	addr = address_map[port][1] + (GPIO_REG_DIFF * pin) + reg_offset;
	rval = __raw_readl((tegra_gpio->regs[address_map[port][0]]) + addr);
	rval = (rval & ~mask) | (val & mask);
	__raw_writel(rval, (tegra_gpio->regs[address_map[port][0]]) + addr);
}

/*
 * This function will return if the GPIO is accessible by CPU
 */
static inline bool is_gpio_accessible(u32 offset)
{
	u32 controller = controller_index(offset);
	int port = GPIO_PORT(offset);
	int pin = GPIO_PIN(offset);
	u32 val;
	u32 i, j;
	bool found = false;

	if (controller == -1) {
		pr_err("Invalid gpio number %d\n", offset);
		return false;
	}

	for (i = 0; i < MAX_GPIO_CONTROLLERS; i++) {
		for (j = 0; j < MAX_GPIO_PORTS; j++) {
			if (tegra186_gpio_map[i][j] == port) {
				found = true;
				break;
			}
		}
		if (found)
			break;
	}

	if (!found)
		return false;

	i = (controller == (MAX_GPIO_CONTROLLERS - 1)) ? 1 : 0;
	if (i == 1)
		controller = 0; /*AON offset is same as base*/

	val = __raw_readl(tegra_gpio->regs[i] +
		(controller * GPIO_CONTROLLERS_DIFF) +
		(j * GPIO_SCR_BASE_DIFF) + (pin * GPIO_SCR_DIFF) +
			GPIO_SCR_REG);

	if ((val & GPIO_FULL_ACCESS) == GPIO_FULL_ACCESS)
		return true;

	return false;
}

static void tegra_gpio_enable(int gpio)
{
	tegra_gpio_update(gpio, GPIO_ENB_CONFIG_REG, 0x1, 0x1);
}

static void tegra_gpio_disable(int gpio)
{
	tegra_gpio_update(gpio, GPIO_ENB_CONFIG_REG, 0x1, 0x0);
}

static int tegra_gpio_request(struct gpio_chip *chip, unsigned offset)
{
	bool accessible;

	accessible = is_gpio_accessible(offset);
	if (accessible)
		return pinctrl_request_gpio(chip->base + offset);
	return -EBUSY;
}

static void tegra_gpio_free(struct gpio_chip *chip, unsigned offset)
{
	pinctrl_free_gpio(chip->base + offset);
	tegra_gpio_disable(offset);
}

static void tegra_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	u32 val = (value) ? 0x1 : 0x0;

	tegra_gpio_writel(val, offset, GPIO_OUT_VAL_REG);
	tegra_gpio_writel(0, offset, GPIO_OUT_CTRL_REG);
}

static int tegra_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	u32 val;

	val = tegra_gpio_readl(offset, GPIO_ENB_CONFIG_REG);
	if (val & GPIO_INOUT_BIT)
		return tegra_gpio_readl(offset, GPIO_OUT_VAL_REG) & 0x1;

	return tegra_gpio_readl(offset, GPIO_INPUT_REG) & 0x1;
}

static void set_gpio_direction_mode(unsigned offset, bool mode)
{
	u32 val;

	val = tegra_gpio_readl(offset, GPIO_ENB_CONFIG_REG);
	if (mode)
		val |= GPIO_INOUT_BIT;
	else
		val &= ~GPIO_INOUT_BIT;
	tegra_gpio_writel(val, offset, GPIO_ENB_CONFIG_REG);
}

static int tegra_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	int ret;

	set_gpio_direction_mode(offset, 0);
	tegra_gpio_enable(offset);
	ret = pinctrl_gpio_direction_input(chip->base + offset);
	if (ret < 0)
		dev_err(chip->dev,
			"Tegra gpio input: pinctrl input failed: %d\n", ret);
	return 0;
}

static int tegra_gpio_direction_output(struct gpio_chip *chip, unsigned offset,
					int value)
{
	int ret;

	tegra_gpio_set(chip, offset, value);
	set_gpio_direction_mode(offset, 1);
	tegra_gpio_enable(offset);
	ret = pinctrl_gpio_direction_output(chip->base + offset);
	if (ret < 0)
		dev_err(chip->dev,
			"Tegra gpio output: pinctrl output failed: %d\n", ret);
	return 0;
}

static int tegra_gpio_set_debounce(struct gpio_chip *chip, unsigned offset,
				unsigned debounce)
{
	u32 val;
	unsigned max_dbc;
	unsigned debounce_ms = DIV_ROUND_UP(debounce, 1000);

	val = tegra_gpio_readl(offset, GPIO_ENB_CONFIG_REG);
	if (val & GPIO_ENB_BIT) {
		val |= GPIO_DEB_FUNC_BIT;
		tegra_gpio_writel(val, offset, GPIO_ENB_CONFIG_REG);

		debounce_ms = max(debounce_ms, 255U);
		max_dbc = tegra_gpio_readl(offset, GPIO_DBC_THRES_REG);
		max_dbc = (max_dbc < debounce_ms) ? debounce_ms : max_dbc;

		tegra_gpio_writel(max_dbc, offset, GPIO_DBC_THRES_REG);
		return 0;
	}
	return -ENOSYS;
}

static int tegra_gpio_to_irq(struct gpio_chip *chip, unsigned offset)
{
	return irq_find_mapping(irq_domain, offset);
}

static void tegra_gpio_irq_ack(struct irq_data *d)
{
	int gpio = d->hwirq;

	tegra_gpio_writel(1, gpio, GPIO_INT_CLEAR_REG);
}

static void tegra_gpio_irq_mask(struct irq_data *d)
{
	int gpio = d->hwirq;

	tegra_gpio_update(gpio, GPIO_ENB_CONFIG_REG, GPIO_INT_FUNC_BIT, 0);
}

static void tegra_gpio_irq_unmask(struct irq_data *d)
{
	int gpio = d->hwirq;

	tegra_gpio_update(gpio, GPIO_ENB_CONFIG_REG, GPIO_INT_FUNC_BIT,
				GPIO_INT_FUNC_BIT);
}

static int tegra_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	int gpio = d->hwirq;
	u32 lvl_type = 0;
	u32 trg_type = 0;
	u32 val;
	u32 wake = tegra_gpio_to_wake(d->hwirq);

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		trg_type = TRIGGER_LEVEL_HIGH;
		lvl_type = GPIO_INT_LVL_SINGLE_EDGE_TRIGGER;
		break;

	case IRQ_TYPE_EDGE_FALLING:
		trg_type = TRIGGER_LEVEL_LOW;
		lvl_type = GPIO_INT_LVL_SINGLE_EDGE_TRIGGER;
		break;

	case IRQ_TYPE_EDGE_BOTH:
		lvl_type = GPIO_INT_LVL_BOTH_EDGE_TRIGGER;
		break;

	case IRQ_TYPE_LEVEL_HIGH:
		trg_type = TRIGGER_LEVEL_HIGH;
		lvl_type = GPIO_INT_LVL_LEVEL_TRIGGER;
		break;

	case IRQ_TYPE_LEVEL_LOW:
		trg_type = TRIGGER_LEVEL_LOW;
		lvl_type = GPIO_INT_LVL_LEVEL_TRIGGER;
		break;

	default:
		return -EINVAL;
	}

	trg_type = trg_type << 0x4;
	lvl_type = lvl_type << 0x2;

	/* TBD: Program the values */
	val = tegra_gpio_readl(gpio, GPIO_ENB_CONFIG_REG);
	val |= trg_type | lvl_type;
	tegra_gpio_writel(val, gpio, GPIO_ENB_CONFIG_REG);

	tegra_gpio_enable(gpio);

	if (type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
		__irq_set_handler_locked(d->irq, handle_level_irq);
	else if (type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING))
		__irq_set_handler_locked(d->irq, handle_edge_irq);

	tegra_pm_irq_set_wake_type(wake, type);

	return 0;
}

static struct gpio_chip tegra_gpio_chip = {
	.label			= "tegra-gpio",
	.request		= tegra_gpio_request,
	.free			= tegra_gpio_free,
	.direction_input	= tegra_gpio_direction_input,
	.get			= tegra_gpio_get,
	.direction_output	= tegra_gpio_direction_output,
	.set			= tegra_gpio_set,
	.set_debounce		= tegra_gpio_set_debounce,
	.to_irq			= tegra_gpio_to_irq,
	.base			= 0,
};

static struct irq_chip tegra_gpio_irq_chip = {
	.name		= "GPIO",
	.irq_ack	= tegra_gpio_irq_ack,
	.irq_mask	= tegra_gpio_irq_mask,
	.irq_unmask	= tegra_gpio_irq_unmask,
	.irq_set_type	= tegra_gpio_irq_set_type,
	.flags		= IRQCHIP_MASK_ON_SUSPEND,
};

static void tegra_gpio_irq_handler(unsigned int irq, struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int map_index;
	int pin;
	u32 i;
	unsigned long val;
	u32 gpio;
	u32 temp;
	u32 reg;

	switch (irq) {
	case DEFAULT_IRQ_OFFSET + 47:
		map_index = 0;
		break;
	case DEFAULT_IRQ_OFFSET + 50:
		map_index = 1;
		break;
	case DEFAULT_IRQ_OFFSET + 53:
		map_index = 2;
		break;
	case DEFAULT_IRQ_OFFSET + 56:
		map_index = 3;
		break;
	case DEFAULT_IRQ_OFFSET + 59:
		map_index = 4;
		break;
	case DEFAULT_IRQ_OFFSET + 61:
		map_index = 6;
		break;
	case DEFAULT_IRQ_OFFSET + 180:
		map_index = 5;
		break;
	default:
		return;
	}

	chained_irq_enter(chip, desc);
	for (i = 0; i < MAX_GPIO_PORTS; i++) {
		if (tegra186_gpio_map[map_index][i] == -1)
			continue;

		temp = address_map[tegra186_gpio_map[map_index][i]][1];
		reg = tegra186_gpio_map[map_index][i];
		val = __raw_readl(tegra_gpio->regs[address_map[reg][0]] +
				temp + GPIO_INT_STATUS_OFFSET + GPIO_STATUS_G1);
		gpio = tegra186_gpio_map[map_index][i] * 8;
		for_each_set_bit(pin, &val, 8)
			generic_handle_irq(gpio_to_irq(gpio + pin));
	}

	chained_irq_exit(chip, desc);
}

static struct of_device_id tegra_gpio_of_match[] = {
	{ .compatible = "nvidia,tegra186-gpio", NULL },
	{ },
};

static void gpio_clk_reset_enable(struct platform_device *pdev)
{
	int ret;
	int i;

	for (i = 0; i < MAX_GPIO_CAR_CTRL; ++i) {
		if (tegra_gpio->gpio_rst[i])
			reset_control_reset(tegra_gpio->gpio_rst[i]);
	}

	for (i = 0; i < MAX_GPIO_CAR_CTRL; ++i) {
		if (!tegra_gpio->gpio_clk[i])
			continue;

		ret = clk_prepare_enable(tegra_gpio->gpio_clk[i]);
		if (ret < 0)
			dev_err(&pdev->dev, "GPIO %d clock enable failed: %d\n",
				i, ret);
	}
}

static void read_gpio_mapping_data(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	u32 pval;
	int nstates;
	int i;

	if (!np)
		return;
	nstates = of_property_count_u32(np, "nvidia,gpio_mapping");
	nstates = nstates / 2;
	for (i = 0; i < nstates; i++) {
		of_property_read_u32_index(np, "nvidia,gpio_mapping",
					i * 2, &pval);
		address_map[i][0] = pval;
		of_property_read_u32_index(np, "nvidia,gpio_mapping",
					i * 2 + 1, &pval);
		address_map[i][1] = pval;
	}
}

static int tegra_gpio_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct tegra_gpio_controller *tg_cont;
	void __iomem *base;
	u32 i, j;
	int gpio;
	u32 ret;

	read_gpio_mapping_data(pdev);

	for (tegra_gpio_bank_count = 0;; tegra_gpio_bank_count++) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ,
					tegra_gpio_bank_count);
		if (!res)
			break;
	}

	if (!tegra_gpio_bank_count) {
		dev_err(&pdev->dev, "No GPIO Controller found\n");
		return -ENODEV;
	}

	tegra_gpio = devm_kzalloc(&pdev->dev, sizeof(*tegra_gpio), GFP_KERNEL);
	if (!tegra_gpio) {
		dev_err(&pdev->dev, "Can't alloc tegra_gpio\n");
		return -ENOMEM;
	}
	tegra_gpio->dev = &pdev->dev;

	/* Bypass the control of reset and clock as this
	 * is enabled by default on BL and not to disable
	 * throughout of system.
	 */
#if 0
	for (i = 0; i < MAX_GPIO_CAR_CTRL; ++i) {
		char car_name[32];
		snprintf(car_name, 32, "gpio%d", i);
		tegra_gpio->gpio_rst[i] = devm_reset_control_get(
						&pdev->dev, car_name);
		if (IS_ERR(tegra_gpio->gpio_rst[i])) {
			ret = PTR_ERR(tegra_gpio->gpio_rst[i]);
			dev_err(&pdev->dev, "Reset control for %s failed: %d\n",
				car_name, ret);
			return ret;
		}
		snprintf(car_name, 32, "gpio%d_clk", i);
		tegra_gpio->gpio_clk[i] = devm_clk_get(&pdev->dev, car_name);
		if (IS_ERR(tegra_gpio->gpio_clk[i])) {
			ret = PTR_ERR(tegra_gpio->gpio_clk[i]);
			dev_err(&pdev->dev, "Clock control for %s failed: %d\n",
				car_name, ret);
			return ret;
		}
	}
#endif

	for (i = 0;; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res)
			break;
	}
	tegra_gpio->nbanks = i;

	tegra_gpio->regs = devm_kzalloc(&pdev->dev, tegra_gpio->nbanks *
				sizeof(*tegra_gpio->regs), GFP_KERNEL);
	if (!tegra_gpio->regs) {
		dev_err(&pdev->dev, "Can't alloc regs pointer\n");
		return -ENODEV;
	}

	tegra_gpio->reg_base = devm_kzalloc(&pdev->dev, tegra_gpio->nbanks *
				sizeof(*tegra_gpio->reg_base), GFP_KERNEL);
	if (!tegra_gpio->reg_base) {
		dev_err(&pdev->dev, "Can't alloc reg_base pointer\n");
		return -ENOMEM;
	}

	for (i = 0; i < tegra_gpio->nbanks; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			dev_err(&pdev->dev, "Missing MEM resource\n");
			return -ENODEV;
		}

		base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(base)) {
			ret = PTR_ERR(base);
			dev_err(&pdev->dev,
				"memregion/iomap address request failed: %d\n",
				ret);
			return ret;
		}
		tegra_gpio->reg_base[i] = res->start;
		tegra_gpio->regs[i] = base;
	}

	for (i = 0; i < tegra_gpio_bank_count; i++) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ, i);
		if (!res) {
			dev_err(&pdev->dev, "Missing IRQ resource\n");
			return -ENODEV;
		}
		tg_cont = &tegra_gpio_controllers[i];
		tg_cont->controller = i;
		tg_cont->irq = res->start;
	}

	gpio_clk_reset_enable(pdev);

	tegra_gpio_chip.dev = &pdev->dev;
	tegra_gpio_chip.of_node = pdev->dev.of_node;
	tegra_gpio_chip.ngpio = MAX_PORTS * MAX_PINS_PER_PORT;

	irq_domain = irq_domain_add_linear(pdev->dev.of_node,
			   tegra_gpio_chip.ngpio, &irq_domain_simple_ops, NULL);
	if (!irq_domain)
		return -ENODEV;

	for (gpio = 0; gpio < tegra_gpio_chip.ngpio; gpio++) {
		int irq = irq_create_mapping(irq_domain, gpio);

		tg_cont = &tegra_gpio_controllers[controller_index(gpio)];

		irq_set_chip_data(irq, tg_cont);
		irq_set_chip_and_handler(irq, &tegra_gpio_irq_chip,
					 handle_simple_irq);
		set_irq_flags(irq, IRQF_VALID);
	}

	for (i = 0; i < tegra_gpio_bank_count; i++) {
		tg_cont = &tegra_gpio_controllers[i];

		for (j = 0; j < 4; j++)
			spin_lock_init(&tg_cont->lvl_lock[j]);

		irq_set_handler_data(tg_cont->irq, tg_cont);
		irq_set_chained_handler(tg_cont->irq, tegra_gpio_irq_handler);
	}

	gpiochip_add(&tegra_gpio_chip);
	return 0;
}

static void tegra_gpio_resume(void)
{
	unsigned long flags;
	int pin;

	local_irq_save(flags);

	for (pin = 0; pin < tegra_gpio_chip.ngpio; pin++) {
		tegra_gpio_writel(tegra_gpio_controllers->cnf[pin], pin,
				GPIO_ENB_CONFIG_REG);
		tegra_gpio_writel(tegra_gpio_controllers->dbc[pin], pin,
				GPIO_DBC_THRES_REG);
		tegra_gpio_writel(tegra_gpio_controllers->out_ctrl[pin], pin,
				GPIO_OUT_CTRL_REG);
		tegra_gpio_writel(tegra_gpio_controllers->out_val[pin], pin,
				GPIO_OUT_VAL_REG);
	}
	local_irq_restore(flags);
}

static int tegra_gpio_suspend(void)
{
	unsigned long flags;
	int pin;

	local_irq_save(flags);
	for (pin = 0; pin < tegra_gpio_chip.ngpio; pin++) {
		tegra_gpio_controllers->cnf[pin] =
			tegra_gpio_readl(pin, GPIO_ENB_CONFIG_REG);
		tegra_gpio_controllers->dbc[pin] =
			tegra_gpio_readl(pin, GPIO_DBC_THRES_REG);
		tegra_gpio_controllers->out_ctrl[pin] =
			tegra_gpio_readl(pin, GPIO_OUT_CTRL_REG);
		tegra_gpio_controllers->out_val[pin] =
			tegra_gpio_readl(pin, GPIO_OUT_VAL_REG);
	}
	local_irq_restore(flags);

	of_gpiochip_suspend(&tegra_gpio_chip);
	return 0;
}

static struct syscore_ops tegra_gpio_syscore_ops = {
	.suspend = tegra_gpio_suspend,
	.resume = tegra_gpio_resume,
	.save = tegra_gpio_suspend,
	.restore = tegra_gpio_resume,
};

static struct platform_driver tegra_gpio_driver = {
	.driver		= {
		.name	= "tegra-gpio",
		.owner	= THIS_MODULE,
		.of_match_table = tegra_gpio_of_match,
	},
	.probe		= tegra_gpio_probe,
};

static int __init tegra_gpio_init(void)
{
	register_syscore_ops(&tegra_gpio_syscore_ops);
	return platform_driver_register(&tegra_gpio_driver);
}
postcore_initcall(tegra_gpio_init);

#ifdef	CONFIG_DEBUG_FS

#define TOTAL_GPIOS 253

#include <linux/debugfs.h>
#include <linux/seq_file.h>

static int dbg_gpio_show(struct seq_file *s, void *unused)
{
	int i;
	bool accessible;

	seq_puts(s, "Pin:ENB DBC IN OUT_CTRL OUT_VAL INT_CLR\n");

	for (i = 0; i < TOTAL_GPIOS; i++) {
		accessible = is_gpio_accessible(i);
		if (accessible) {
			seq_printf(s, "%d   0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
				i,
				tegra_gpio_readl(i, GPIO_ENB_CONFIG_REG),
				tegra_gpio_readl(i, GPIO_DBC_THRES_REG),
				tegra_gpio_readl(i, GPIO_INPUT_REG),
				tegra_gpio_readl(i, GPIO_OUT_CTRL_REG),
				tegra_gpio_readl(i, GPIO_OUT_VAL_REG),
				tegra_gpio_readl(i, GPIO_INT_CLEAR_REG));
		}
	}
	return 0;
}

static int dbg_gpio_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_gpio_show, &inode->i_private);
}

static const struct file_operations debug_fops = {
	.open		= dbg_gpio_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init tegra_gpio_debuginit(void)
{
	(void) debugfs_create_file("tegra_gpio", S_IRUGO,
					NULL, NULL, &debug_fops);
	return 0;
}
late_initcall(tegra_gpio_debuginit);
#endif

MODULE_AUTHOR("Suresh Mangipudi <smangipudi@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra186 GPIO driver");
MODULE_LICENSE("GPL v2");
