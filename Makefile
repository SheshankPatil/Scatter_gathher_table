# Add these at the top of the Makefile
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

obj-m += sg_mem_drv.o

# Pointing to the local linux-xlnx installation
KDIR ?= $(PWD)/linux-xlnx
PWD  := $(shell pwd)

all: module userspace

module:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) modules

userspace: sg_mem_test

sg_mem_test: sg_mem_test.c sg_mem_ioctl.h
	$(CROSS_COMPILE)gcc -Wall -O2 -o sg_mem_test sg_mem_test.c

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$(PWD) clean
	rm -f sg_mem_test

.PHONY: all module userspace clean