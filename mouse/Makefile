obj-m += simple_usb_mouse.o

PWD := $(CURDIR)

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	mkdir -p build
	mv -f *.o *.ko *.mod *.mod.c Module.symvers modules.order build/ 2>/dev/null || true
	find . -maxdepth 1 -name '.*.cmd' -exec mv {} build/ \; 2>/dev/null || true
	find . -maxdepth 1 -name '.*.o' -exec mv {} build/ \; 2>/dev/null || true
	[ -d .tmp_versions ] && mv .tmp_versions build/ || true

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -rf build

install:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules_install
	depmod -a

uninstall:
	rm -f /lib/modules/$(shell uname -r)/kernel/drivers/usb/input/simple_usb_mouse.ko
	depmod -a
