# Out-of-tree build for the NVP6324 CSI-2 source sub-device driver.
#
# Under Yocto this is driven by module.bbclass (recipes-kernel/nvp6324), which
# sets KERNEL_SRC and passes M=$(SRC); the all/modules_install/clean targets
# below are the conventional OE out-of-tree module interface.
#
# For a manual build against an installed kernel:
#   make KDIR=/path/to/kernel/build

obj-m := nvp6324.o

SRC := $(shell pwd)
KERNEL_SRC ?= $(KDIR)
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules

# Alias in case the module class invokes the `modules` target explicitly.
modules: all

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) clean

.PHONY: all modules_install clean
