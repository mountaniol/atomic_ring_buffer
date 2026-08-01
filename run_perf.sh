#/bin/bash

perf record $@
perf report
