obj-m += simple_usb_mouse.o
obj-m += gesture_usb_mouse.o

PWD := $(CURDIR)
KDIR := /lib/modules/$(shell uname -r)/build

# Function to organize build artifacts
# Usage: $(call organize_build,pattern)
define organize_build
	mkdir -p build
	mv -f *.o *.ko *.mod *.mod.c Module.symvers modules.order build/ 2>/dev/null || true
	find . -maxdepth 1 -name '.*.cmd' -exec mv {} build/ \; 2>/dev/null || true
	find . -maxdepth 1 -name '.*.o' -exec mv {} build/ \; 2>/dev/null || true
	[ -d .tmp_versions ] && mv .tmp_versions build/ || true
endef

all:
	make -C $(KDIR) M=$(PWD) modules
	$(call organize_build)
simple:
	make -C $(KDIR) M=$(PWD) simple_usb_mouse.ko
	$(call organize_build)

gesture:
	make -C $(KDIR) M=$(PWD) gesture_usb_mouse.ko
	$(call organize_build)

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -rf build

install:
	make -C $(KDIR) M=$(PWD) modules_install
	depmod -a

uninstall:
	rm -f /lib/modules/$(shell uname -r)/kernel/drivers/usb/input/simple_usb_mouse.ko
	rm -f /lib/modules/$(shell uname -r)/kernel/drivers/usb/input/gesture_usb_mouse.ko
	depmod -a
