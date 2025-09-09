CROSS   ?= riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

CFLAGS  := -Wall -O2 -nostdlib -nostartfiles -ffreestanding -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany
LDFLAGS := -T kernel/linker.ld

SRC_ASM := kernel/entry.S
SRC_C   := kernel/start.c
OBJS    := $(SRC_ASM:.S=.o) $(SRC_C:.c=.o)

BUILD   := build
ELF     := $(BUILD)/kernel.elf

.PHONY: all run qemu clean

all: $(ELF)

$(BUILD):
	mkdir -p $(BUILD)

%.o: %.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

run: $(ELF)
	qemu-system-riscv64 -machine virt -nographic -bios none -kernel $(ELF)

# qemu 作为 run 的别名
qemu: run

clean:
	rm -rf $(BUILD) kernel/*.o
