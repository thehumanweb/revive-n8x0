# mbxemu: the emulator core (libmbxemu.a), built for the host.
# The QEMU device is in qemu/.
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter
CORE = core.o ta.o vgp.o render.o rcptab.o texture.o twod.o pb.o

all: libmbxemu.a

# mbxtri: a triangle on the host, no kernel (tools/)
mbxtri: tools/mbxtri.c tools/tri_frame.h lib/mbx_contract.h libmbxemu.a
	$(CC) $(CFLAGS) -o $@ tools/mbxtri.c libmbxemu.a -lm

libmbxemu.a: $(CORE)
	$(AR) rcs $@ $^

$(CORE): emu.h mbxemu.h

clean:
	rm -f *.o *.a mbxtri mbxtri-drm tests/t_vgp tests/t_ta tests/t_pb tests/t_contract

# mbxtri-drm: the same triangle through the render node (cross-built for the guest: make CC=arm-... mbxtri-drm CFLAGS="-I<kernel uapi>")
mbxtri-drm: tools/mbxtri_drm.c tools/tri_frame.h drm/mbx_drm.h
	$(CC) $(CFLAGS) -I. $(EXTRA) -o $@ tools/mbxtri_drm.c -lm

# tests (host): the VGP encoder against the model's interpreter
tests/t_vgp: tests/t_vgp.c lib/mbx_vgp.h libmbxemu.a
	$(CC) $(CFLAGS) -I. -o $@ tests/t_vgp.c libmbxemu.a -lm

tests/t_ta: tests/t_ta.c tools/tri_frame.h libmbxemu.a
	$(CC) $(CFLAGS) -I. -o $@ tests/t_ta.c libmbxemu.a -lm

tests/t_pb: tests/t_pb.c tools/tri_frame.h lib/mbx_contract.h libmbxemu.a
	$(CC) $(CFLAGS) -I. -o $@ tests/t_pb.c libmbxemu.a -lm

tests/t_contract: tests/t_contract.c tools/tri_frame.h lib/mbx_contract.h libmbxemu.a
	$(CC) $(CFLAGS) -I. -o $@ tests/t_contract.c libmbxemu.a -lm

test: tests/t_vgp tests/t_ta tests/t_pb tests/t_contract
	tests/t_vgp
	tests/t_ta
	tests/t_pb
	tests/t_contract

.PHONY: all clean test
