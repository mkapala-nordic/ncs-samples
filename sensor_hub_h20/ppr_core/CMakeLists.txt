#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ppr_main)

if(CONFIG_APP_MULTITHREADING)
  target_sources(app PRIVATE src/main.c)
else()
  target_sources(app PRIVATE src/main_no_multithreading.c)
endif()

if (CONFIG_APP_PROVIDE_PM_HOOKS)
  target_sources(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/tests/benchmarks/multicore/common/power_off.c)
endif()

if (CONFIG_APP_MANUAL_VPR_PM_CUSTOM)
  message(WARNING "Manual VPR PM custom implementation is enabled.")
endif()

if (CONFIG_APP_MANUAL_VPR_PM_HACKED_PM)
  message(WARNING "Manual VPR PM hacked PM implementation is enabled.")
endif()
