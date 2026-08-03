# WS is defined by sdk_case.mk; use recursive expansion.
# Compiles device_manager sources into the case ELF for now.
# Once the SDK is built, the same symbols are also available as
# -lulmk_comp_ulmk_device_manager_$(TAG) with
# -I$(SDK)/include/component/ulmk_device_manager.
CASE_NAME := device_manager
CASE_SRCS = root_thread.c \
	$(WS)/components/ulmk_device_manager/src/registry.c \
	$(WS)/components/ulmk_device_manager/src/client.c
EXTRA_CFLAGS = -I$(WS)/components/ulmk_device_manager/include \
	-I$(WS)/components/ulmk_device_manager/src \
	-I$(firstword $(wildcard $(WS)/../ulmk_apps/ulmk_device_classes/include) \
		$(wildcard /ulmk_apps/ulmk_device_classes/include))
SENTINELS := "device_manager: PASS"
FAIL_SENTINEL := "device_manager: FAIL"
QEMU_TIMEOUT := 40

include ../sdk_case.mk
