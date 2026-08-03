CC      := cc
ROOT    := ../..
TARGET  := device_manager_unit_test
STUBINC := include
COMP    := $(ROOT)/components/ulmk_device_manager
# Host: sibling ../ulmk_apps.  Container (dev.py): mounted at /ulmk_apps.
CLASSES := $(firstword \
	$(wildcard $(ROOT)/../ulmk_apps/ulmk_device_classes) \
	$(wildcard /ulmk_apps/ulmk_device_classes))

ifeq ($(CLASSES),)
$(error ulmk_device_classes not found (expected ../ulmk_apps or /ulmk_apps))
endif

CFLAGS := \
	-std=c99 \
	-Wall -Wextra \
	-Wno-unused-parameter \
	-DUL_UNIT_TEST=1 \
	-I$(STUBINC) \
	-I$(COMP)/include \
	-I$(COMP)/src \
	-I$(CLASSES)/include \
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
