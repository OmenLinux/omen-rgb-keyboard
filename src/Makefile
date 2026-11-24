# normal makefile
KERNELDIR ?= /lib/modules/`uname -r`/build
PWD := $(shell pwd)
BUILD_DIR := $(PWD)/build

# HDA mute LED support is built in by default and gated at load time: see Kbuild.
OMEN_HDA_LED ?= y

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) OMEN_HDA_LED=$(OMEN_HDA_LED)

build:
	@echo "Building hp-wmi module to $(BUILD_DIR)..."
	@mkdir -p $(BUILD_DIR)
	$(MAKE) -C $(KERNELDIR) M=$(PWD) O=$(BUILD_DIR) OMEN_HDA_LED=$(OMEN_HDA_LED) modules

# Module specific targets
genbin:
	echo "X" > hp-wmi_bin.o_shipped

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -rf $(BUILD_DIR)/*

.PHONY: clean genbin default build
