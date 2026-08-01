#!/bin/bash

#su root -c 'sh cd /sys/devices/system/cpu ;  echo performance | tee cpu*/cpufreq/scaling_governor'
sudo su root -c 'cd /sys/devices/system/cpu ;  echo performance | tee cpu*/cpufreq/scaling_governor ; echo "-1" /proc/sys/kernel/perf_event_paranoid'
# AFL_INST_LIBS=1 LD_LIBRARY_PATH=$(pwd) afl-fuzz -Q -i in -o out -- ./ring_buf_afl.out
AFL_MAP_SIZE=10000000 afl-fuzz -i ./afl_input -m 1000 -o ./afl_output ./ring_buf_afl.out

