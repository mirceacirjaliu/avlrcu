

specialized-tree-y += main.o tree.o prealloc.o

# += -O1 -fno-inline
CFLAGS_tree.o += -Wno-implicit-fallthrough
CFLAGS_prealloc.o += -Wno-implicit-fallthrough

obj-m += specialized-tree.o

KERNEL = $(shell uname -r)

all:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KERNEL)/build M=$(PWD) clean

install:
	make -c /lib/modules/$(KERNEL)/build M=$(PWD) install

.PHONY: all clean install
