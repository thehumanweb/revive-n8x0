# Runs on the N810 (as root, through n810-net.sh): a fresh module load, then two jobs, each with its write log
chmod +x /tmp/mbxtri-drm-arm; dmesg -c >/dev/null; rmmod mbx; insmod /tmp/mbx.ko
echo 1 > /sys/module/mbx/parameters/trace
dmesg -c >/dev/null
/tmp/mbxtri-drm-arm -D -P /tmp/pb1.bin -R /tmp/j1 -o /tmp/j1.ppm > /tmp/j1.txt 2>&1
dmesg > /tmp/trace1.txt; dmesg -c >/dev/null
/tmp/mbxtri-drm-arm -D -P /tmp/pb2.bin -R /tmp/j2 -o /tmp/j2.ppm > /tmp/j2.txt 2>&1
dmesg > /tmp/trace2.txt
echo 0 > /sys/module/mbx/parameters/trace
uname -a > /tmp/uname.txt; cat /sys/module/mbx/version 2>/dev/null > /tmp/modver.txt
cd /tmp && tar cf - trace1.txt trace2.txt j1.txt j2.txt j1.before j1.after j2.before j2.after pb1.bin pb2.bin j1.ppm j2.ppm uname.txt
