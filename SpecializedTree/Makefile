# If KERNELRELEASE is defined, we've been invoked from the
# kernel build system and can use its language.
ifneq ($(KERNELRELEASE),)
	obj-m += avlrcu.o
	avlrcu-objs += test.o tree.o prealloc.o

	ccflags-y := -DAVLRCU_DEBUG
	# TODO: also need a build flag that enables the test interface: AVLRCU_TEST
	# TODO: AVLRCU_TEST enables AVLRCU_DEBUG

	# += -O1 -fno-inline

# Otherwise we were called directly from the command
# line; invoke the kernel build system.
else
	KERNEL = $(shell uname -r)
	KERNELDIR ?= /lib/modules/$(KERNEL)/build
	PWD := $(shell pwd)

.PHONY: all clean install

all:
	make -C $(KERNELDIR) M=$(PWD) modules

clean:
	make -C $(KERNELDIR) M=$(PWD) clean

install:
	make -c $(KERNELDIR) M=$(PWD) install

endif
