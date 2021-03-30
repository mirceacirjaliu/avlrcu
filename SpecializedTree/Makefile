

specialized-tree-y += main.o tree.o prealloc.o

CFLAGS_main.o += -O0 -fno-inline
CFLAGS_tree.o += -O0 -fno-inline -Wno-implicit-fallthrough
CFLAGS_prealloc.o += -O0 -fno-inline -Wno-implicit-fallthrough

obj-m += specialized-tree.o

KERNEL = $(shell uname -r)

all:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) clean

install:
	make -c /lib/modules/$(KERNEL)/build M=$(PWD) install

.PHONY: all clean install
