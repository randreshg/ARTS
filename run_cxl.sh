#!/bin/bash

clean_regions() {
    echo "Cleaning regions..."
    rapidutil remove -r "shared"
    rapidutil remove -r "fam_ranks"
}

trap clean_regions SIGINT

clean_regions
../../../script/run.py --exe /usr/bin/pwd
time ./"$1" "${@:2}"
clean_regions
