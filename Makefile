PROJECT_NAME := GPMon_v1
CHIP ?= bl602
BOARD ?= bl602dk
PORT ?= /dev/ttyUSB0
CROSS_COMPILE ?= riscv64-unknown-elf-

BL_SDK_BASE ?= /home/malay/bouffalo_sdk
TOOLCHAIN_BIN ?= /home/malay/thead_toolchain_min/bin
SDK_DEMO_PATH := $(CURDIR)
export BL_SDK_BASE
export PATH := $(TOOLCHAIN_BIN):$(PATH)

CONFIG_OVERRIDE_PTFILE = config/partition_cfg_4M.toml
include $(BL_SDK_BASE)/project.build
