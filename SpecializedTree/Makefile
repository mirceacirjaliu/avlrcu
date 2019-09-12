

specialized-tree-y += main.o tree.o

obj-m += specialized-tree.o

KERNEL = $(shell uname -r)

all:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) clean

.PHONY: all clean
