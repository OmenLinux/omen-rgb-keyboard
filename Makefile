install:
	dkms install .

uninstall:
	dkms remove omen-rgb-keyboard/1.2 --all

all: install
