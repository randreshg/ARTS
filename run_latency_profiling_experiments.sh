#!/bin/bash

cd build/example/cpu
cp ../../../run_cxl.sh .
chmod +x run_cxl.sh
cd ../../../experiments/latency_profling
python3 latency_profiling.py
cd ../../
tar -zcvf latency_profiling.tar.gz experiments/latency_profiling