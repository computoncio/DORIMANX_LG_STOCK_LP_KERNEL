/*
 * Generic GPIO card-detect helper
 *
 * Copyright (C) 2011, Guennadi Liakhovetski <g.liakhovetski@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mmc/host.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/module.h>
#include <linux/slab.h>

#if defined(CONFIG_LGE_MMC_DYNAMIC_LOG)
#include <linux/mmc/debug_log.h>
#endif

struct mmc_gpio {
	unsigned int cd_gpio;
	bool status;
	char cd_label[0];
};

#ifdef CONFIG_MACH_LGE
/* LGE_CHANGE
 * Change it global-function for usage from others.
 * 2014-01-16, B2-BSP-FS@lge.com
 */
int mmc_cd_get_status(struct mmc_host *host)
#else
static int mmc_cd_get_status(struct mmc_host *host)
#endif
{
	int ret = -ENOSYS;
	struct mmc_gpio *ctx = host->hotplug.handler_priv;

	if (!ctx || !gpio_is_valid(ctx->cd_gpio))
		goto out;

	ret = !gpio_get_value_cansleep(ctx->cd_gpio) ^
		!!(host->caps2 & MMC_CAP2_CD_ACTIVE_HIGH);
out:
	return ret;
}

static irqreturn_t mmc_gpio_cd_irqt(int irq, void *dev_id)
{
	struct mmc_host *host = dev_id;
	struct mmc_gpio *ctx = host->hotplug.handler_priv;
	int status;

	status = mmc_cd_get_status(host);
	if (unlikely(status < 0))
		goto out;

	if (status ^ ctx->status) {
		pr_info("%s: slot status change detected (%d -> %d), GPIO_ACTIVE_%s\n",
				mmc_hostname(host), ctx->status, status,
				(host->caps2 & MMC_CAP2_CD_ACTIVE_HIGH) ?
				"HIGH" : "LOW");
		ctx->status = status;

		/* Schedule a card detection after a debounce timeout */
		#ifdef CONFIG_MACH_LGE
		/* LGE_CHANGE
		 * Reduce debounce time to make it more sensitive
		 * 2014-01-16, B2-BSP-FS@lge.com
		 */
		mmc_detect_change(host, 0);
		#else
		mmc_detect_change(host, msecs_to_jiffies(100));
		#endif
	}
out:
	return IRQ_HANDLED;
}

int mmc_gpio_request_cd(struct mmc_host *host, unsigned int gpio)
{
	size_t len = strlen(dev_name(host->parent)) + 4;
	struct mmc_gpio *ctx;
	int irq = gpio_to_irq(gpio);
	int ret;

	if (irq < 0)
		return irq;

	ctx = kmalloc(sizeof(*ctx) + len, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	snprintf(ctx->cd_label, len, "%s cd", dev_name(host->parent));

	ret = gpio_request_one(gpio, GPIOF_DIR_IN, ctx->cd_label);
	if (ret < 0)
		goto egpioreq;

	ret = mmc_cd_get_status(host);
	if (ret < 0)
		goto eirqreq;

	ctx->status = ret;

	ret = request_threaded_irq(irq, NULL, mmc_gpio_cd_irqt,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			ctx->cd_label, host);
	if (ret < 0)
		goto eirqreq;

	ctx->cd_gpio = gpio;
	host->slot.cd_irq = irq;
	host->slot.handler_priv = ctx;

	return 0;

eirqreq:
	gpio_free(gpio);
egpioreq:
	kfree(ctx);
	return ret;
}
EXPORT_SYMBOL(mmc_gpio_request_cd);

void mmc_gpio_free_cd(struct mmc_host *host)
{
	struct mmc_gpio *ctx = host->slot.handler_priv;

	if (!ctx || !gpio_is_valid(ctx->cd_gpio))
		return;

	free_irq(host->slot.cd_irq, host);
	gpio_free(ctx->cd_gpio);
	ctx->cd_gpio = -EINVAL;
	kfree(ctx);
}
EXPORT_SYMBOL(mmc_gpio_free_cd);
