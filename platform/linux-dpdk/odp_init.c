/* Copyright (c) 2013-2018, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include "config.h"

#include <odp_posix_extensions.h>
#include <odp/api/init.h>
#include <odp_debug_internal.h>
#include <odp/api/debug.h>
#include <unistd.h>
#include <odp_internal.h>
#include <odp_schedule_if.h>
#include <odp_libconfig_internal.h>
#include <odp_shm_internal.h>
#include <string.h>
#include <stdio.h>
#include <linux/limits.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <pwd.h>
#include <ctype.h>

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_string_fns.h>

#define MEMPOOL_OPS(hdl) extern void mp_hdlr_init_##hdl(void)
MEMPOOL_OPS(ops_mp_mc);
MEMPOOL_OPS(ops_sp_sc);
MEMPOOL_OPS(ops_mp_sc);
MEMPOOL_OPS(ops_sp_mc);
MEMPOOL_OPS(ops_stack);

#ifndef RTE_BUILD_SHARED_LIB
/*
 * This function is not called from anywhere, it's only purpose is to make sure
 * that if ODP and DPDK are statically linked to an application, the GCC
 * constructors of mempool handlers are linked as well. Otherwise the linker
 * would omit them. It's not an issue with dynamic linking. */
void refer_constructors(void);
void refer_constructors(void)
{
	mp_hdlr_init_ops_mp_mc();
	mp_hdlr_init_ops_sp_sc();
	mp_hdlr_init_ops_mp_sc();
	mp_hdlr_init_ops_sp_mc();
	mp_hdlr_init_ops_stack();
}
#endif

static int odp_init_dpdk(const char *cmdline)
{
	char **dpdk_argv;
	int dpdk_argc;
	char *full_cmdline;
	int i, cmdlen;
	odp_cpumask_t mask;
	char mask_str[ODP_CPUMASK_STR_SIZE];
	int32_t masklen;
	cpu_set_t original_cpuset;

	if (cmdline == NULL) {
		cmdline = getenv("ODP_PLATFORM_PARAMS");
		if (cmdline == NULL)
			cmdline = "";
	}

	CPU_ZERO(&original_cpuset);
	i = pthread_getaffinity_np(pthread_self(),
				   sizeof(original_cpuset), &original_cpuset);
	if (i != 0) {
		ODP_ERR("Failed to read thread affinity: %d\n", i);
		return -1;
	}

	odp_cpumask_zero(&mask);
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &original_cpuset)) {
			odp_cpumask_set(&mask, i);
			break;
		}
	}
	masklen = odp_cpumask_to_str(&mask, mask_str, ODP_CPUMASK_STR_SIZE);

	if (masklen < 0) {
		ODP_ERR("CPU mask error: d\n", masklen);
		return -1;
	}

	/* masklen includes the terminating null as well */
	full_cmdline = calloc(1, strlen("odpdpdk -c ") + masklen +
			      strlen(" ") + strlen(cmdline));

	/* first argument is facility log, simply bind it to odpdpdk for now.*/
	cmdlen = sprintf(full_cmdline, "odpdpdk -c %s %s", mask_str, cmdline);

	for (i = 0, dpdk_argc = 1; i < cmdlen; ++i) {
		if (isspace(full_cmdline[i]))
			++dpdk_argc;
	}
	dpdk_argv = malloc(dpdk_argc * sizeof(char *));

	dpdk_argc = rte_strsplit(full_cmdline, strlen(full_cmdline), dpdk_argv,
				 dpdk_argc, ' ');
	for (i = 0; i < dpdk_argc; ++i)
		ODP_DBG("arg[%d]: %s\n", i, dpdk_argv[i]);
	fflush(stdout);

	i = rte_eal_init(dpdk_argc, dpdk_argv);
	free(dpdk_argv);
	free(full_cmdline);
	if (i < 0) {
		ODP_ERR("Cannot init the Intel DPDK EAL!\n");
		return -1;
	} else if (i + 1 != dpdk_argc) {
		ODP_DBG("Some DPDK args were not processed!\n");
		ODP_DBG("Passed: %d Consumed %d\n", dpdk_argc, i + 1);
	}
	ODP_DBG("rte_eal_init OK\n");

	i = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t),
				   &original_cpuset);
	if (i)
		ODP_ERR("Failed to reset thread affinity: %d\n", i);

	return 0;
}

struct odp_global_data_s odp_global_data;

void odp_init_param_init(odp_init_t *param)
{
	memset(param, 0, sizeof(odp_init_t));
}

int odp_init_global(odp_instance_t *instance,
		    const odp_init_t *params,
		    const odp_platform_init_t *platform_params)
{
	enum init_stage stage = NO_INIT;

	memset(&odp_global_data, 0, sizeof(struct odp_global_data_s));
	odp_global_data.main_pid = getpid();

	odp_global_data.log_fn = odp_override_log;
	odp_global_data.abort_fn = odp_override_abort;

	if (params != NULL) {
		if (params->log_fn != NULL)
			odp_global_data.log_fn = params->log_fn;
		if (params->abort_fn != NULL)
			odp_global_data.abort_fn = params->abort_fn;
	}

	if (_odp_libconfig_init_global()) {
		ODP_ERR("ODP runtime config init failed.\n");
		goto init_failed;
	}
	stage = LIBCONFIG_INIT;

	if (odp_cpumask_init_global(params)) {
		ODP_ERR("ODP cpumask init failed.\n");
		goto init_failed;
	}
	stage = CPUMASK_INIT;

	if (odp_init_dpdk((const char *)platform_params)) {
		ODP_ERR("ODP dpdk init failed.\n");
		return -1;
	}

	if (odp_time_init_global()) {
		ODP_ERR("ODP time init failed.\n");
		goto init_failed;
	}
	stage = TIME_INIT;

	if (odp_system_info_init()) {
		ODP_ERR("ODP system_info init failed.\n");
		goto init_failed;
	}
	stage = SYSINFO_INIT;

	if (_odp_shm_init_global(params)) {
		ODP_ERR("ODP shm init failed.\n");
		goto init_failed;
	}
	stage = ISHM_INIT;

	if (odp_thread_init_global()) {
		ODP_ERR("ODP thread init failed.\n");
		goto init_failed;
	}
	stage = THREAD_INIT;

	if (odp_pool_init_global()) {
		ODP_ERR("ODP pool init failed.\n");
		goto init_failed;
	}
	stage = POOL_INIT;

	if (_odp_queue_init_global()) {
		ODP_ERR("ODP queue init failed.\n");
		goto init_failed;
	}
	stage = QUEUE_INIT;

	if (_odp_schedule_init_global()) {
		ODP_ERR("ODP schedule init failed.\n");
		goto init_failed;
	}
	stage = SCHED_INIT;

	if (odp_pktio_init_global()) {
		ODP_ERR("ODP packet io init failed.\n");
		goto init_failed;
	}
	stage = PKTIO_INIT;

	if (odp_timer_init_global(params)) {
		ODP_ERR("ODP timer init failed.\n");
		goto init_failed;
	}
	stage = TIMER_INIT;

	if (odp_crypto_init_global()) {
		ODP_ERR("ODP crypto init failed.\n");
		goto init_failed;
	}
	stage = CRYPTO_INIT;

	if (odp_classification_init_global()) {
		ODP_ERR("ODP classification init failed.\n");
		goto init_failed;
	}
	stage = CLASSIFICATION_INIT;

	if (odp_tm_init_global()) {
		ODP_ERR("ODP traffic manager init failed\n");
		goto init_failed;
	}
	stage = TRAFFIC_MNGR_INIT;

	if (_odp_int_name_tbl_init_global()) {
		ODP_ERR("ODP name table init failed\n");
		goto init_failed;
	}
	stage = NAME_TABLE_INIT;

	if (_odp_ipsec_events_init_global()) {
		ODP_ERR("ODP IPsec events init failed.\n");
		goto init_failed;
	}
	stage = IPSEC_EVENTS_INIT;

	if (_odp_ipsec_sad_init_global()) {
		ODP_ERR("ODP IPsec SAD init failed.\n");
		goto init_failed;
	}
	stage = IPSEC_SAD_INIT;

	if (_odp_ipsec_init_global()) {
		ODP_ERR("ODP IPsec init failed.\n");
		goto init_failed;
	}
	stage = IPSEC_INIT;

	/* Dummy support for single instance */
	*instance = (odp_instance_t)odp_global_data.main_pid;

	return 0;

init_failed:
	_odp_term_global(stage);
	return -1;
}

int odp_term_global(odp_instance_t instance)
{
	if (instance != (odp_instance_t)odp_global_data.main_pid) {
		ODP_ERR("Bad instance.\n");
		return -1;
	}
	return _odp_term_global(ALL_INIT);
}

int _odp_term_global(enum init_stage stage)
{
	int rc = 0;

	switch (stage) {
	case ALL_INIT:
	case IPSEC_INIT:
		if (_odp_ipsec_term_global()) {
			ODP_ERR("ODP IPsec term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case IPSEC_SAD_INIT:
		if (_odp_ipsec_sad_term_global()) {
			ODP_ERR("ODP IPsec SAD term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case IPSEC_EVENTS_INIT:
		if (_odp_ipsec_events_term_global()) {
			ODP_ERR("ODP IPsec events term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case NAME_TABLE_INIT:
		if (_odp_int_name_tbl_term_global()) {
			ODP_ERR("Name table term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case TRAFFIC_MNGR_INIT:
		if (odp_tm_term_global()) {
			ODP_ERR("TM term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case CLASSIFICATION_INIT:
		if (odp_classification_term_global()) {
			ODP_ERR("ODP classification term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case CRYPTO_INIT:
		if (odp_crypto_term_global()) {
			ODP_ERR("ODP crypto term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case TIMER_INIT:
		if (odp_timer_term_global()) {
			ODP_ERR("ODP timer term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case PKTIO_INIT:
		if (odp_pktio_term_global()) {
			ODP_ERR("ODP pktio term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case SCHED_INIT:
		if (_odp_schedule_term_global()) {
			ODP_ERR("ODP schedule term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case QUEUE_INIT:
		if (_odp_queue_term_global()) {
			ODP_ERR("ODP queue term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case POOL_INIT:
		if (odp_pool_term_global()) {
			ODP_ERR("ODP buffer pool term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case THREAD_INIT:
		if (odp_thread_term_global()) {
			ODP_ERR("ODP thread term failed.\n");
			rc = -1;
		}
		/* Fall through */

	/* Needed to prevent compiler warning */
	case FDSERVER_INIT:
	case ISHM_INIT:
		if (_odp_shm_term_global()) {
			ODP_ERR("ODP shm term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case SYSINFO_INIT:
		if (odp_system_info_term()) {
			ODP_ERR("ODP system info term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case TIME_INIT:
		if (odp_time_term_global()) {
			ODP_ERR("ODP time term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case CPUMASK_INIT:
		if (odp_cpumask_term_global()) {
			ODP_ERR("ODP cpumask term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case LIBCONFIG_INIT:
		if (_odp_libconfig_term_global()) {
			ODP_ERR("ODP runtime config term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case NO_INIT:
		;
	}

	return rc;
}

int odp_init_local(odp_instance_t instance, odp_thread_type_t thr_type)
{
	enum init_stage stage = NO_INIT;

	if (instance != (odp_instance_t)odp_global_data.main_pid) {
		ODP_ERR("Bad instance.\n");
		goto init_fail;
	}

	if (_odp_shm_init_local()) {
		ODP_ERR("ODP shm local init failed.\n");
		goto init_fail;
	}
	stage = ISHM_INIT;

	if (odp_thread_init_local(thr_type)) {
		ODP_ERR("ODP thread local init failed.\n");
		goto init_fail;
	}
	stage = THREAD_INIT;

	if (odp_pktio_init_local()) {
		ODP_ERR("ODP packet io local init failed.\n");
		goto init_fail;
	}
	stage = PKTIO_INIT;

	if (_odp_crypto_init_local()) {
		ODP_ERR("ODP crypto local init failed.\n");
		goto init_fail;
	}
	stage = CRYPTO_INIT;

	if (odp_pool_init_local()) {
		ODP_ERR("ODP pool local init failed.\n");
		goto init_fail;
	}
	stage = POOL_INIT;

	if (queue_fn->init_local()) {
		ODP_ERR("ODP queue local init failed.\n");
		goto init_fail;
	}
	stage = QUEUE_INIT;

	if (sched_fn->init_local()) {
		ODP_ERR("ODP schedule local init failed.\n");
		goto init_fail;
	}
	/* stage = SCHED_INIT; */

	return 0;

init_fail:
	_odp_term_local(stage);
	return -1;
}

int odp_term_local(void)
{
	return _odp_term_local(ALL_INIT);
}

int _odp_term_local(enum init_stage stage)
{
	int rc = 0;
	int rc_thd = 0;

	switch (stage) {
	case ALL_INIT:

	case SCHED_INIT:
		if (sched_fn->term_local()) {
			ODP_ERR("ODP schedule local term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case QUEUE_INIT:
		if (queue_fn->term_local()) {
			ODP_ERR("ODP queue local term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case CRYPTO_INIT:
		if (_odp_crypto_term_local()) {
			ODP_ERR("ODP crypto local term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case POOL_INIT:
		if (odp_pool_term_local()) {
			ODP_ERR("ODP buffer pool local term failed.\n");
			rc = -1;
		}
		/* Fall through */

	case THREAD_INIT:
		rc_thd = odp_thread_term_local();
		if (rc_thd < 0) {
			ODP_ERR("ODP thread local term failed.\n");
			rc = -1;
		} else {
			if (!rc)
				rc = rc_thd;
		}
		/* Fall through */

	case ISHM_INIT:
		if (_odp_shm_term_local()) {
			ODP_ERR("ODP shm local term failed.\n");
			rc = -1;
		}
		/* Fall through */

	default:
		break;
	}

	return rc;
}
