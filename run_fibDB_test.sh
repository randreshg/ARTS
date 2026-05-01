#!/bin/bash

echo "**********************************"
echo "Running round-robin DB allocation mode"
cd build/examples/cpu
cp ../../../run_cxl.sh .
chmod +x run_cxl.sh
cp ../../../sample_configs/arts_crete_roundrobin.cfg arts.cfg
for i in {1..50}; do
    echo "Run $i"
    ./run_cxl.sh ./fibDB 22 &> fibDB_${i}.log
done
cd ../../../
python3 check_logs_fibDB.py