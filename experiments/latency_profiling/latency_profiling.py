#!/usr/bin/env python3
"""
Latency profiling experiment script for the Crete cluster.

For each node and each FAM device index, this script:
  1. Creates a per-node output directory.
  2. Generates a tailored arts.cfg (single-node, static CXL device = i).
  3. Copies the config to the build directory.
  4. Runs ./run_cxl.sh ./latency_bench, directing output to the per-node directory.
"""

import os
import re
import shutil
import subprocess
from pathlib import Path

# ==============================================================================
# CONFIGURATION
# ==============================================================================

node_names = ["ca-fcp0", "ca-fcp1"]

#NUM_FAM_DEVICES = 14
NUM_FAM_DEVICES = 2

# ==============================================================================
# PATHS
# ==============================================================================

# Absolute path to the project root (two levels up from this script's directory)
SCRIPT_DIR = Path(__file__).resolve().parent          # …/experiments/latency_profiling
PROJECT_ROOT_DIR = SCRIPT_DIR.parent.parent           # …/arts_cxl

SAMPLE_CFG = PROJECT_ROOT_DIR / "sample_configs" / "arts_crete.cfg"
BUILD_CPU_DIR = PROJECT_ROOT_DIR / "build" / "examples" / "cpu"

# ==============================================================================
# HELPERS
# ==============================================================================

def set_cfg_value(content: str, key: str, value: str) -> str:
    """Replace `key=<anything>` with `key=<value>` in cfg file content."""
    return re.sub(rf"^({re.escape(key)}=).*$", rf"\g<1>{value}", content, flags=re.MULTILINE)

# ==============================================================================
# MAIN EXPERIMENT LOOP
# ==============================================================================

def main() -> None:
    # Working directory is the directory that contains this script
    working_dir = SCRIPT_DIR

    for node_name in node_names:
        # ------------------------------------------------------------------
        # i)  Create per-node directory inside the working directory
        # ------------------------------------------------------------------
        node_dir = working_dir / node_name
        node_dir.mkdir(parents=True, exist_ok=True)

        # ------------------------------------------------------------------
        # ii) "Change directory" to node_name (track as current dir)
        # ------------------------------------------------------------------
        current_dir = node_dir

        for i in range(NUM_FAM_DEVICES):
            # --------------------------------------------------------------
            # Create per-device subdirectory  <node_name>/<i>/
            # --------------------------------------------------------------
            device_dir = current_dir / str(i)
            device_dir.mkdir(parents=True, exist_ok=True)

            # --------------------------------------------------------------
            # a) Copy sample config to device directory, rename to arts.cfg
            # --------------------------------------------------------------
            arts_cfg_path = device_dir / "arts.cfg"
            shutil.copy2(SAMPLE_CFG, arts_cfg_path)

            # --------------------------------------------------------------
            # b-e) Edit the copied arts.cfg
            # --------------------------------------------------------------
            cfg_content = arts_cfg_path.read_text()

            cfg_content = set_cfg_value(cfg_content, "cxl_db_allocation_device", str(i))
            cfg_content = set_cfg_value(cfg_content, "master_node", node_name)
            cfg_content = set_cfg_value(cfg_content, "node_count", "1")
            cfg_content = set_cfg_value(cfg_content, "nodes", node_name)

            # --------------------------------------------------------------
            # f) Save file; note the device directory for output
            # --------------------------------------------------------------
            arts_cfg_path.write_text(cfg_content)
            output_dir = device_dir  # noted working directory for -o flag

            # --------------------------------------------------------------
            # g) Copy arts.cfg to PROJECT_ROOT_DIR/build/examples/cpu/
            # --------------------------------------------------------------
            BUILD_CPU_DIR.mkdir(parents=True, exist_ok=True)
            shutil.copy2(arts_cfg_path, BUILD_CPU_DIR / "arts.cfg")

            # --------------------------------------------------------------
            # h) Change dir to PROJECT_ROOT_DIR/build/examples/cpu/
            # --------------------------------------------------------------
            run_dir = BUILD_CPU_DIR

            # --------------------------------------------------------------
            # i) Run ./run_cxl.sh ./latency_bench -o "<output_dir>"
            # --------------------------------------------------------------
            cmd = ["./run_cxl.sh", "./latency_bench", "-o", str(output_dir)]
            print(f"[{node_name}] device={i}: running {' '.join(cmd)} in {run_dir}")
            subprocess.run(cmd, cwd=str(run_dir), check=True)

            # --------------------------------------------------------------
            # j) Return to per-node directory
            # --------------------------------------------------------------
            current_dir = node_dir  # reset for next iteration (already there)

        print(f"[{node_name}] All {NUM_FAM_DEVICES} device runs complete.\n")


if __name__ == "__main__":
    main()
