# mbxemu: the emulator core (libmbxemu.a), built for the host.
# The QEMU device is in qemu/.
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter
CORE = core.o ta.o vgp.o render.o rcptab.o texture.o twod.o pb.o

all: libmbxemu.a

libmbxemu.a: $(CORE)
	$(AR) rcs $@ $^

$(CORE): emu.h mbxemu.h

clean:
	rm -f *.o *.a

.PHONY: all clean
