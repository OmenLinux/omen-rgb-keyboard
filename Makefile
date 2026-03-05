install:
	dkms install .

uninstall:
	dkms remove omen-rgb-keyboard/1.3 --all

all: install
