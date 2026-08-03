CC      := cc
ROOT    := ../..
TARGET  := device_manager_unit_test
STUBINC := include
COMP    := $(ROOT)/components/ulmk_device_manager
# Prefer apps policy headers; fall back to in-tree fixtures (CI has no apps).
CLASSES_INC := $(firstword \
	$(wildcard $(ROOT)/../ulmk_apps/ulmk_device_classes/include) \
	$(wildcard /ulmk_apps/ulmk_device_classes/include) \
	$(ROOT)/tests/device_manager_common/include)

CFLAGS := \
	-std=c99 \
	-Wall -Wextra \
	-Wno-unused-parameter \
	-DUL_UNIT_TEST=1 \
	-I$(STUBINC) \
	-I$(COMP)/include \
	-I$(COMP)/src \
	-I$(CLASSES_INC) \
	-g -O0

SRCS := \
	device_manager_unit_test.c \
	protocol_dummy.c \
	$(COMP)/src/registry.c

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@

run: $(TARGET)
	@echo "--- running device_manager unit tests ---"
	@./$(TARGET) && echo "DEVICE_MANAGER UNIT TEST: PASS" || \
	    { echo "DEVICE_MANAGER UNIT TEST: FAIL"; exit 1; }

clean:
	rm -f $(TARGET)
