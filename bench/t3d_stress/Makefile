# Builds with libdragon's toolchain against a tiny3d checkout: make T3D_INST=/path/to/tiny3d
BUILD_DIR=build
T3D_INST ?= $(shell realpath ../../../tiny3d)

include $(N64_INST)/include/n64.mk
include $(T3D_INST)/t3d.mk

N64_CFLAGS += -std=gnu2x

PROJECT_NAME=t3d_stress

all: $(PROJECT_NAME).z64

$(BUILD_DIR)/$(PROJECT_NAME).elf: $(BUILD_DIR)/main.o

$(PROJECT_NAME).z64: N64_ROM_TITLE="T3D STRESS"

clean:
	rm -rf $(BUILD_DIR) *.z64

-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean
