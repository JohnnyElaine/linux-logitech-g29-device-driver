obj-m += g29_media_usb.o

PWD := $(CURDIR)

.PHONY: all clean install uninstall

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	mkdir -p build
	mv -f *.ko build/ 2>/dev/null || true

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -rf build

install:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules_install
	depmod -a

uninstall:
	@modpath=$$(modinfo -n g29_media_usb 2>/dev/null); \
	if [ -z "$$modpath" ]; then \
		echo "Module g29_media_usb not found via modinfo (not installed?)"; \
		exit 1; \
	fi; \
	echo "Removing $$modpath"; \
	rm -f "$$modpath"; \
	depmod -a
