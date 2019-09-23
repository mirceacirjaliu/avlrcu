

specialized-tree-y += main.o tree.o

CFLAGS_main.o += -O0 -fno-inline
CFLAGS_tree.o += -O0 -fno-inline

obj-m += specialized-tree.o

KERNEL = $(shell uname -r)

all:
	#make -C /lib/modules/$(KERNEL)/build M=$(PWD) modules
	make -C /home/mircea/host M=$(PWD) modules

clean:
	#make -C /lib/modules/$(KERNEL)/build M=$(PWD) clean
	make -C /home/mircea/host M=$(PWD) clean


.PHONY: all clean
