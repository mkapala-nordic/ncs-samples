#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ppr_main)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_sources(app PRIVATE src/main.c)

if (CONFIG_APP_PROVIDE_PM_HOOKS)
  target_sources(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/tests/benchmarks/multicore/common/power_off.c)
endif()
