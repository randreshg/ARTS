#!/bin/bash

echo "Running static DB allocation mode"
cd build/examples/cpu
cp ../../../run_cxl.sh .
chmod +x run_cxl.sh
cp ../../../sample_configs/arts_crete.cfg arts.cfg
./run_cxl.sh ./inhibitor_raw_mem -d 4 -p s -s 1000 -w 0 -t 0

echo "**********************************"
echo "Running round-robin DB allocation mode"
cp ../../../sample_configs/arts_crete_roundrobin.cfg arts.cfg
./run_cxl.sh ./inhibitor_raw_mem -d 4 -p s -s 1000 -w 0 -t 0