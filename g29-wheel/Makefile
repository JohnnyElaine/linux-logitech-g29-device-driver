
.PHONY: all clean install uninstall

obj-m += g29_usb.o
PWD := $(CURDIR)

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	mkdir -p build
	-mv -f -- *.ko *.mod.c *.o .*.o *.mod modules.order .*.cmd *.symvers build/

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -rf build
