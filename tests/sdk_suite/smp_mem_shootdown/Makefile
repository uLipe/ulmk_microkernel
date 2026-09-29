CASE_NAME := smp_mem_shootdown
CASE_SRCS := root_thread.c
SENTINELS := "smp_mem_shootdown: revoke ok" "smp_mem_shootdown: kill ok" "smp_mem_shootdown: PASS"
FAIL_SENTINEL := "smp_mem_shootdown: FAIL"
QEMU_TIMEOUT := 60
SMP := 1

include ../sdk_case.mk
