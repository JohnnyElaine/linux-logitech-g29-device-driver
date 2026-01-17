
.PHONY: all

obj-m += wasd.o
KVER := $(shell uname -r)
KDIR ?= $(firstword $(wildcard /lib/modules/$(KVER)/build) $(wildcard /usr/lib/modules/$(KVER)/build))
PWD := $(shell pwd)
OUT := $(PWD)/out

all:
	@if [ -z "$(KDIR)" ]; then \
		echo "ERROR: kernel build dir not found for $(KVER). Install kernel headers (e.g. linux-headers)"; \
		exit 2; \
	fi
	mkdir -p $(OUT)
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	-mv -f -- *.ko *.mod.c *.o .*.o *.mod modules.order .*.cmd *.symvers $(OUT)

clean:
	@if [ -z "$(KDIR)" ]; then \
		echo "ERROR: kernel build dir not found for $(KVER)."; \
		exit 2; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf $(OUT) *.cmd *.order *.mod *.o
