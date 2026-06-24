# SPDX-License-Identifier: Apache-2.0

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_m/scripts/linker.ld CACHE INTERNAL "SoC Linker Script")

add_subdirectory(common)

if(CONFIG_HAS_MSPM0_SDK)
  string(TOUPPER ${CONFIG_SOC} sdk_soc_select)
  zephyr_compile_definitions(__${sdk_soc_select}__)
endif()
