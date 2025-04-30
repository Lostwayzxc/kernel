// SPDX-License-Identifier: GPL-2.0-only
#include <linux/rpal.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/pkeys.h>
#include <net/af_unix.h>
#include <asm/paravirt_types.h>
#include <linux/mmu_context.h>

#include "internal.h"

bool rpal_inited;

static bool check_hardware_features(void)
{
	if (!boot_cpu_has(X86_FEATURE_FSGSBASE)) {
		rpal_err("no fsgsbase feature\n");
		return false;
	}

	return true;
}

int __init rpal_init(void)
{
	int ret = 0;

	if (!check_hardware_features())
		goto fail;

	ret = rpal_service_init();
	if (ret)
		goto fail;

	rpal_inited = true;
	return 0;

fail:
	rpal_err("rpal init fail\n");
	return -1;
}

subsys_initcall(rpal_init);
