#!/bin/bash

echo "**********************************"
echo "Running round-robin DB allocation mode"
cd build/examples/cpu
cp random_access/random_access RANDOM_ACCESS
cp ../../../run_cxl.sh .
chmod +x run_cxl.sh
cp ../../../sample_configs/arts_crete_roundrobin.cfg arts.cfg
for i in {1..30}; do
    ./run_cxl.sh ./RANDOM_ACCESS &> random_access_${i}.log
done
cd ../../../
python3 check_logs_random_access.py