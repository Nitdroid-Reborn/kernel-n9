/*
 * TI's dspbridge platform device registration
 *
 * Copyright (C) 2005-2006 Texas Instruments, Inc.
 * Copyright (C) 2009 Nokia Corporation
 *
 * Written by Hiroshi DOYU <Hiroshi.DOYU@nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>
//#include <linux/lmb.h>
#include <linux/bootmem.h>

#include "prm.h"
#include "cm.h"
#ifdef CONFIG_BRIDGE_DVFS
#include <plat/omap-pm.h>
#endif

#include "../../../drivers/staging/tidspbridge/include/dspbridge/host_os.h"

static struct platform_device *dspbridge_pdev;

static struct dspbridge_platform_data dspbridge_pdata __initdata = {
#ifdef CONFIG_BRIDGE_DVFS
	.dsp_set_min_opp = omap_pm_dsp_set_min_opp,
	.dsp_get_opp	 = omap_pm_dsp_get_opp,
	.cpu_set_freq	 = omap_pm_cpu_set_freq,
	.cpu_get_freq	 = omap_pm_cpu_get_freq,
#endif
	.dsp_prm_read	= prm_read_mod_reg,
	.dsp_prm_write	= prm_write_mod_reg,
	.dsp_prm_rmw_bits = prm_rmw_mod_reg_bits,
	.dsp_cm_read	= cm_read_mod_reg,
	.dsp_cm_write	= cm_write_mod_reg,
	.dsp_cm_rmw_bits = cm_rmw_mod_reg_bits,
};

static unsigned long dspbridge_phys_mempool_base;

void __init dspbridge_reserve_sdram(void)
{
//	unsigned long va, size = CONFIG_TIDSPBRIDGE_MEMPOOL_SIZE; // LMB
	void *va;
	unsigned long size = CONFIG_TIDSPBRIDGE_MEMPOOL_SIZE;

	if (!size)
		return;

//	va = lmb_alloc(size, SZ_1M);		//LMB
	va = __alloc_bootmem_nopanic(size, SZ_1M, 0);
	if (!va) {
		pr_err("%s: Failed to bootmem allocation(%lu bytes)\n",
			__func__, size);
		return;
	}

//	dspbridge_phys_mempool_base = va;		//LMB
	dspbridge_phys_mempool_base = virt_to_phys(va);
}

static int __init dspbridge_init(void)
{
	struct platform_device *pdev;
	int err = -ENOMEM;
	struct dspbridge_platform_data *pdata = &dspbridge_pdata;

	pdata->phys_mempool_base = dspbridge_phys_mempool_base;
	if (pdata->phys_mempool_base) {
		pdata->phys_mempool_size = CONFIG_TIDSPBRIDGE_MEMPOOL_SIZE;
		pr_info("%s: %x bytes @ %x\n", __func__,
			pdata->phys_mempool_size, pdata->phys_mempool_base);
	}

	pdev = platform_device_alloc("C6410", -1);
	if (!pdev)
		goto err_out;

	err = platform_device_add_data(pdev, pdata, sizeof(*pdata));
	if (err)
		goto err_out;

	err = platform_device_add(pdev);
	if (err)
		goto err_out;

	dspbridge_pdev = pdev;
	return 0;

err_out:
	platform_device_put(pdev);
	return err;
}
module_init(dspbridge_init);

static void __exit dspbridge_exit(void)
{
	platform_device_unregister(dspbridge_pdev);
}
module_exit(dspbridge_exit);

MODULE_AUTHOR("Hiroshi DOYU");
MODULE_DESCRIPTION("TI's dspbridge platform device registration");
MODULE_LICENSE("GPL v2");

