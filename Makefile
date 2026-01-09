obj-m += usb_bootmouse.o

KVER := $(shell uname -r)

# Most distros expose headers at /lib/modules/$(KVER)/build, but Arch/Manjaro
# often uses /usr/lib/modules/$(KVER)/build.
KDIR ?= $(firstword \
	$(wildcard /lib/modules/$(KVER)/build) \
	$(wildcard /usr/lib/modules/$(KVER)/build))

PWD := $(shell pwd)

OUT := $(PWD)/out

all:
	@if [ -z "$(KDIR)" ]; then \
		echo "ERROR: kernel build dir not found for $(KVER). Install kernel headers (e.g. linux-headers)"; \
		exit 2; \
	fi
	mkdir -p $(OUT)
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	-mv -f -- *.ko *.mod.c *.o *.mod modules.order .*.cmd $(OUT) 2>/dev/null || true
	@echo "Build outputs moved to $(OUT)"

clean:
	@if [ -z "$(KDIR)" ]; then \
		echo "ERROR: kernel build dir not found for $(KVER)."; \
		exit 2; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	@rm -rf $(OUT)
