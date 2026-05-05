#!/bin/bash

echo "**********************************"
echo "Running round-robin DB allocation mode"
cd build/examples/cpu
cp cholesky/cholesky_arts_native .
cp ../../../run_cxl.sh .
chmod +x run_cxl.sh
cp ../../../sample_configs/arts_crete_roundrobin.cfg arts.cfg
for i in {1..100}; do
    echo "Run $i"
    ./run_cxl.sh ./cholesky_arts_native --ds 30400 --ts 40 &> cholesky_${i}.log
    cd ../../../
    python3 check_logs_cholesky.py
    cd build/examples/cpu
done