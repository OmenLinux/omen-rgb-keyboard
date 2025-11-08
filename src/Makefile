# normal makefile
KERNELDIR ?= /lib/modules/`uname -r`/build
PWD := $(shell pwd)
BUILD_DIR := $(PWD)/build

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD)

build:
	@echo "Building hp-wmi module to $(BUILD_DIR)..."
	@mkdir -p $(BUILD_DIR)
	$(MAKE) -C $(KERNELDIR) M=$(PWD) O=$(BUILD_DIR) modules

# Module specific targets
genbin:
	echo "X" > hp-wmi_bin.o_shipped

clean:
	-$(RM) -f *.a *.ko *.o *.mod *.mod.c *.order *.symvers
	rm -rf $(BUILD_DIR)/*

.PHONY: clean genbin default build
