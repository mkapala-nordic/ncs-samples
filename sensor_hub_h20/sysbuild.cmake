#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

if(SB_CONFIG_SOC_NRF54H20)
  set(PPR_SOURCE_DIR ${APP_DIR}/ppr_core)

  ExternalZephyrProject_Add(
    APPLICATION ppr_main
    SOURCE_DIR ${PPR_SOURCE_DIR}
    BOARD ${SB_CONFIG_BOARD}/${SB_CONFIG_SOC}/cpuppr
    BOARD_REVISION ${BOARD_REVISION}
  )
endif()
