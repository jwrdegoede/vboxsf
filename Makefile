# Makefile for vboxsf shared-folder driver

BUILDDIR=/lib/modules/`uname -r`/build

vboxsf-objs := dir.o file.o utils.o vboxsf_wrappers.o super.o

obj-m += vboxsf.o

all:
	make -C $(BUILDDIR) M=`pwd` modules

modules_install:
	make -C $(BUILDDIR) M=`pwd` modules_install

clean:
	make -C $(BUILDDIR) M=`pwd` clean
