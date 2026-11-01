# SPDX-License-Identifier: Apache-2.0
zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/../include)

if(CONFIG_ARCH_POSIX)
  zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/../tests/sim)
endif()

zephyr_library_sources_ifdef(CONFIG_UART_QCOM_UARTDM
  ${CMAKE_CURRENT_LIST_DIR}/../drivers/serial/uart_qcom_uartdm.c)

zephyr_library_sources_ifdef(CONFIG_DISPLAY_QCOM_MDP5_CONT_SPLASH
  ${CMAKE_CURRENT_LIST_DIR}/../drivers/display/display_qcom_mdp5.c)

zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/../lib/fdt_min.c)
