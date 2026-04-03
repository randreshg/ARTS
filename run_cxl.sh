#!/bin/bash

clean_regions() {
    echo "Cleaning regions..."
    rapidutil -d -r "shared"
    rapidutil -d -r "fam_ranks"
}

trap clean_regions SIGINT

clean_regions
../../../script/run.py --exe /usr/bin/pwd
time ./"$1" "${@:2}"
clean_regions
