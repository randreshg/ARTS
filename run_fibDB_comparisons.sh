#!/bin/bash

echo "**********************************"
echo "Running regular CXL mode"
cd build/examples/cpu
cp ../../../run_cxl.sh .
chmod +x run_cxl.sh
cp ../../../sample_configs/arts_crete_roundrobin.cfg arts.cfg
#for i in {5..25}; do
for i in {30..30}; do
    echo "Running Fib $i"
    #timeout 180s ./run_cxl.sh ./fibDB $i &> fibDB_${i}.log
    ./run_cxl.sh ./fibDB $i &> fibDB_${i}.log
    sleep 30
done
cd ../../../
cd build_noCXL/examples/cpu
cp ../../../sample_configs/arts_crete_roundrobin.cfg arts.cfg
#for i in {5..25}; do
#for i in {26..30}; do
for i in {28..30}; do
    echo "Running Fib $i"
    #timeout 180s ./fibDB $i &> fibDB_nonCXL_${i}.log
    ./fibDB $i &> fibDB_nonCXL_${i}.log
    sleep 30
done
cd ../../../
