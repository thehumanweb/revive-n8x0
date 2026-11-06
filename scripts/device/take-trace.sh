#!/bin/sh
# Take one triangle job's trace on the N810 (through the jig host) and build docs/n810-trace.txt:
# every register write and TA word the driver makes (module parameter trace), the registers
# before and after (REG_DUMP), the parameter buffer (PB_DUMP), the region array, the picture;
# and the emulator's writes for the same job. Needs the cross-built tool at build/mbxtri-drm-arm
# and the host tool build/mbxtri-drm-x86 (see JOURNEY.md), the module already on the unit
# (/tmp/mbx.ko) and the jig host reachable.
set -eu
cd "$(dirname "$0")/../.."
S=carsten@storage.netbird.cloud
scp -q -o BatchMode=yes build/mbxtri-drm-arm $S:n810/mbx-test/
ssh -o BatchMode=yes $S 'cd ~/n810 && sh n810-net.sh "cat > /tmp/mbxtri-drm-arm" < mbx-test/mbxtri-drm-arm'
scp -q -o BatchMode=yes scripts/device/trace-on-unit.sh $S:n810/mbx-test/
ssh -o BatchMode=yes $S 'cd ~/n810 && timeout 170 sh n810-net.sh "sh -s" < mbx-test/trace-on-unit.sh > mbx-test/trace.tar'
scp -q -o BatchMode=yes $S:n810/mbx-test/trace.tar build/trace.tar
rm -rf build/trace; mkdir -p build/trace; tar xf build/trace.tar -C build/trace
MBX_SIM_TRACE=1 mesa/scripts/run-sim.sh ./build/mbxtri-drm-x86 -D -P build/trace/sim-pb.bin -R build/trace/sim -o build/trace/sim.ppm \
	> build/trace/sim.txt 2>&1
python3 -I scripts/device/trace-doc.py build/trace docs/n810-trace.txt
echo "wrote docs/n810-trace.txt"
