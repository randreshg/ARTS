#!/bin/bash

rm -f lulesh_profiling.tar.gz
cd build/examples/cpu
cp lulesh/lulesh_arts lulesh_arts
cp ../../../run_cxl.sh .
chmod +x run_cxl.sh
cd ../../../experiments/lulesh
rm -r */
python3 lulesh_experiments.py
cd ../../
tar -zcvf lulesh_profiling.tar.gz experiments/lulesh