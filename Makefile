# Keep the DKMS module identity in one place: dkms.conf is what dkms itself
# reads, so deriving these here stops the Makefile from naming a version that
# was never registered.
DKMS_CONF := $(dir $(lastword $(MAKEFILE_LIST)))dkms.conf
PACKAGE_NAME := $(shell sed -n 's/^PACKAGE_NAME="\?\([^"]*\)"\?$$/\1/p' $(DKMS_CONF))
PACKAGE_VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\?\([^"]*\)"\?$$/\1/p' $(DKMS_CONF))
MODULE_NAME := $(shell sed -n 's/^BUILT_MODULE_NAME="\?\([^"]*\)"\?$$/\1/p' $(DKMS_CONF))
DKMS_MODULE := $(PACKAGE_NAME)/$(PACKAGE_VERSION)

install:
	dkms install .

uninstall:
	dkms remove $(DKMS_MODULE) --all

# dkms refuses to add a module/version combo it already tracks, so reinstalling
# the same version after a code change needs the old registration dropped first.
# Then unload the running module and load the one just installed.
reinstall:
	-dkms remove $(DKMS_MODULE) --all
	dkms install .
	-modprobe -r $(MODULE_NAME)
	modprobe $(MODULE_NAME)

all: install

.PHONY: install uninstall reinstall all
