#!/bin/bash

echo "**********************************"
echo "Running round-robin DB allocation mode"
cd build/examples/cpu
cp ../../../run_cxl.sh .
chmod +x run_cxl.sh
cp ../../../sample_configs/arts_crete_roundrobin.cfg arts.cfg
for i in {1..50}; do
    echo "Run $i"
    timeout 600s ./run_cxl.sh ./memory_stress_test_refactored 1000000 100000  &> memory_stress_test_refactored_v1_${i}.log
done
cd ../../../
python3 check_logs.py "PASSED"
