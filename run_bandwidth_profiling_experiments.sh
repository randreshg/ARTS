#!/bin/bash

rm -f bandwidth_profiling.tar.gz
cd build/examples/cpu
cp stream/stream STREAM
cp ../../../run_cxl.sh .
chmod +x run_cxl.sh
cd ../../../experiments/bandwidth_profiling
rm -r */
python3 bandwidth_profiling.py
cd ../../
tar -zcvf bandwidth_profiling.tar.gz experiments/bandwidth_profiling