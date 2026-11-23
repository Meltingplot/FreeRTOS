# Builds the QEMU mask test for one port and one kernel, see run.sh.
#   make PORT=cm4f|cm7|cm0|cm33 [VARIANT=modified|control] [KERNEL=<kernel src dir>]
# Compiler flags follow Makefiles/SAME51.mk (cm4f), SAME70.mk (cm7), SAMC21.mk (cm0) and STM32H5.mk (cm33),
# without -fstack-usage/-fdump-rtl-expand, which only write extra files.

CROSS_COMPILE ?= arm-none-eabi-
CC := $(CROSS_COMPILE)gcc
OBJDUMP := $(CROSS_COMPILE)objdump

PORT ?= cm4f
VARIANT ?= modified
KERNEL ?= ../../src
BUILD ?= build/$(VARIANT)-$(PORT)

COMMON_CFLAGS := -std=gnu99 -mthumb -fno-math-errno -ffunction-sections -fdata-sections -nostdlib \
	-Wall -Wundef -Wdouble-promotion -fsingle-precision-constant -O2 -g -Dnoexcept=
STRICT_CFLAGS := -Werror -Wwrite-strings -Werror=return-type

ifeq ($(PORT),cm4f)
PORT_DIR := portable/GCC/ARM_CM4F
PORT_SRCS := port.c
CPU_FLAGS := -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CHIP := -D__SAME51N19A__ -DPORT_CM4F
PRIO_BITS := 3
LDS := mps2.ld
SYSCLK := 25000000
EXTRA_CFLAGS := $(STRICT_CFLAGS)
else ifeq ($(PORT),cm7)
PORT_DIR := portable/GCC/ARM_CM7/r0p1
PORT_SRCS := port.c
CPU_FLAGS := -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mno-unaligned-access
CHIP := -D__SAME70Q20B__ -DPORT_CM7
PRIO_BITS := 3
LDS := mps2.ld
SYSCLK := 25000000
EXTRA_CFLAGS := $(STRICT_CFLAGS)
else ifeq ($(PORT),cm0)
PORT_DIR := portable/GCC/ARM_CM0
PORT_SRCS := port.c portasm.c mpu_wrappers_v2_asm.c
CPU_FLAGS := -mcpu=cortex-m0plus
CHIP := -D__SAMC21G18A__ -DPORT_CM0
PRIO_BITS := 2
LDS := microbit.ld
SYSCLK := 16000000
EXTRA_CFLAGS := $(STRICT_CFLAGS)
else ifeq ($(PORT),cm33)
PORT_DIR := portable/GCC/ARM_CM33_NTZ/non_secure
PORT_SRCS := port.c portasm.c mpu_wrappers_v2_asm.c
CPU_FLAGS := -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mno-unaligned-access
# mps2-an505 starts in the secure state and this test does not switch to the non-secure one, so the
# port runs secure-only (only changes the EXC_RETURN of the first task, port.c portINITIAL_EXC_RETURN).
CHIP := -DSTM32H523xx -DPORT_CM33 -DconfigRUN_FREERTOS_SECURE_ONLY=1
PRIO_BITS := 4
LDS := an505.ld
SYSCLK := 25000000
EXTRA_CFLAGS :=
else
$(error unknown PORT $(PORT))
endif

# __NVIC_PRIO_BITS selects configPRIO_BITS (src/include/FreeRTOSConfig.h). PRIO_BITS overrides it
# (run.sh documents why the ARMv7-M/ARMv8-M builds use the value of the real chip, not QEMU's 8).
ifeq ($(VARIANT),control)
VARIANT_FLAGS := -DCONTROL=1
else
VARIANT_FLAGS :=
endif

CFLAGS := $(COMMON_CFLAGS) $(CPU_FLAGS) $(CHIP) $(EXTRA_CFLAGS) $(VARIANT_FLAGS) \
	-D__NVIC_PRIO_BITS=$(PRIO_BITS) -DSYSCLK_HZ=$(SYSCLK) -DPORT_NAME=\"$(PORT)\" \
	-I$(KERNEL)/include -I$(KERNEL) -I$(KERNEL)/$(PORT_DIR) -I.

SRCS := $(KERNEL)/tasks.c $(KERNEL)/queue.c $(KERNEL)/list.c $(addprefix $(KERNEL)/$(PORT_DIR)/,$(PORT_SRCS)) mask_test.c startup.c
OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
vpath %.c $(KERNEL) $(KERNEL)/$(PORT_DIR) .

ELF := $(BUILD)/mask_test.elf

.PHONY: all
all: $(ELF) $(BUILD)/mask_test.dis

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(ELF): $(OBJS) ld/$(LDS) ld/common.ld
	$(CC) $(CPU_FLAGS) -mthumb -nostartfiles -nostdlib -Wl,--gc-sections -Wl,-Map,$(BUILD)/mask_test.map \
		-Lld -T ld/$(LDS) -o $@ $(OBJS) -lc -lgcc

$(BUILD)/mask_test.dis: $(ELF)
	$(OBJDUMP) -d $< > $@

# The NVIC probe (no FreeRTOS)
$(BUILD)/probe.elf: probe.c startup.c semihost.h ld/$(LDS) ld/common.ld
	@mkdir -p $(@D)
	$(CC) $(CPU_FLAGS) -mthumb -O2 -nostartfiles -nostdlib -I. -Lld -T ld/$(LDS) -o $@ probe.c startup.c -lc -lgcc

.PHONY: probe
probe: $(BUILD)/probe.elf

-include $(OBJS:.o=.d)
