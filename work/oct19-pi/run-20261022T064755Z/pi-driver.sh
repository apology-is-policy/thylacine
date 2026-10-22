cd ~/projects/thylacine-corona || exit 9
echo $$ > driver.pid
SMP_KEEP_LOGS=1 BOOT_TIMEOUT=1500 bash tools/smp-multiboot.sh pi-default-smp4 4 10  > pi-default-smp4.out 2>&1; echo $? > pi-default-smp4.rc
SMP_KEEP_LOGS=1 BOOT_TIMEOUT=2400 bash tools/smp-multiboot.sh pi-ubsan-smp4 4 10 ubsan > pi-ubsan-smp4.out 2>&1; echo $? > pi-ubsan-smp4.rc
date -u +%Y-%m-%dT%H:%M:%SZ > DONE
