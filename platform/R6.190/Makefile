MODEL = R6
FW_VERSION = 190

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120

FIRMWARE_ID = 0x80000453
MAIN_FIRMWARE_ADDR = 0xE0100000

# user_mem_start from cstart struct. Align to 0x10 up
RESTARTSTART = 0x1900A0 # 0x190098 in ROM (cstart 0xE0100146: r5 + 0x800)

# Cortex A9, binaries loaded as Thumb
CFLAGS_PLATFORM = -mthumb -mlong-calls
PLATFORM_ARCH = armv7-a

ML_BOOT_OBJ = boot-d678.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/function_overrides.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/lvprobe.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/refresh_guard.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_layer.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_overlays.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_faceyuv.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_gdraw.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_interval.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_grid.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_motion.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_propw.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_ettr.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_astro.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_tlog.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_acore.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_aclock.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_temp.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_modsup.o
# M16.1 (spec m16/m161 2, 5.3 MB-7): the script facade, the LAST R6 object: its menu resolution (.init_funcs in link order) runs
# after every R6 menu_add
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/r6_api.o

# M16.0 (spec m16/m160 3.3): in R6_190.sym, task_create points to r6_task_try_create (the R6 task model for modules);
# without r6_task_try_create in the ELF the build fails
ML_MODULES_SYM_POST = $(PYTHON) r6_symalias.py --require $(BUILD_DIR)/$(ML_MODULES_SYM_NAME).tmp

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
