#!/usr/bin/env python3
"""
LULESH scaling experiment script for the Crete cluster.

For each node count i (1..num_nodes) and each run j (0..NUM_RUNS-1):
  1. Creates experiments/lulesh/<i>/<j>/counters/
  2. Generates a tailored arts.cfg from the round-robin sample config.
  3. Copies the config to the build directory.
  4. Runs ./run_cxl.sh ./lulesh_arts with size/tile/iter inputs scaled to i.
  5. On success (output contains "Final Origin Energy"), saves inputs.json.
"""

import json
import os
import re
import shutil
import subprocess
from pathlib import Path

# ==============================================================================
# CONFIGURATION
# ==============================================================================

node_names = [
    "ca-fcp0",
    "ca-fcp1",
    "ca-fcp2",
    "ca-fcp3",
]

num_nodes = len(node_names)

NUM_RUNS = 10

# Inputs keyed by node count (i)
LULESH_INPUTS = {
    1: ["-s", "180", "-t", "30", "-i", "5"],
    2: ["-s", "210", "-t", "30", "-i", "5"],
    3: ["-s", "240", "-t", "30", "-i", "5"],
    4: ["-s", "270", "-t", "30", "-i", "5"],
}

# ==============================================================================
# PATHS
# ==============================================================================

# Absolute path to the project root (two levels up from this script's directory)
SCRIPT_DIR = Path(__file__).resolve().parent          # …/experiments/lulesh
PROJECT_ROOT_DIR = SCRIPT_DIR.parent.parent           # …/arts_cxl

SAMPLE_CFG = PROJECT_ROOT_DIR / "sample_configs" / "arts_crete_roundrobin.cfg"
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

    for i in range(1, num_nodes + 1):
        # ----------------------------------------------------------------------
        # i)  Create per-node-count directory  <working_dir>/<i>/
        # ----------------------------------------------------------------------
        node_count_dir = working_dir / str(i)
        node_count_dir.mkdir(parents=True, exist_ok=True)

        # ----------------------------------------------------------------------
        # ii) "Change directory" to i (track as current dir)
        # ----------------------------------------------------------------------
        current_dir = node_count_dir

        for j in range(NUM_RUNS):
            # ------------------------------------------------------------------
            # a) Create <i>/<j>/ and <i>/<j>/counters/; note path to j
            # ------------------------------------------------------------------
            run_dir = current_dir / str(j)
            run_dir.mkdir(parents=True, exist_ok=True)
            counters_dir = run_dir / "counters"
            counters_dir.mkdir(parents=True, exist_ok=True)

            # Path to j (absolute)
            run_dir_abs = run_dir.resolve()

            # ------------------------------------------------------------------
            # i)  Copy sample config to run directory, rename to arts.cfg
            # ------------------------------------------------------------------
            arts_cfg_path = run_dir / "arts.cfg"
            shutil.copy2(SAMPLE_CFG, arts_cfg_path)

            # ------------------------------------------------------------------
            # ii) Edit the copied arts.cfg
            # ------------------------------------------------------------------
            cfg_content = arts_cfg_path.read_text()

            # a) master_node = node_names[0]
            cfg_content = set_cfg_value(cfg_content, "master_node", node_names[0])

            # b) node_count = i
            cfg_content = set_cfg_value(cfg_content, "node_count", str(i))

            # c) nodes = node_names[0],...,node_names[i-1]  (i nodes total)
            nodes_value = ",".join(node_names[:i])
            cfg_content = set_cfg_value(cfg_content, "nodes", nodes_value)

            # d) counter_folder = /path/to/j/counters
            cfg_content = set_cfg_value(
                cfg_content, "counter_folder", str(counters_dir.resolve())
            )

            # ------------------------------------------------------------------
            # iii) Save file; note the working directory
            # ------------------------------------------------------------------
            arts_cfg_path.write_text(cfg_content)
            noted_dir = run_dir_abs  # noted working directory for this run

            print(f"[nodes={i}] run={j}: config written to {noted_dir}/arts.cfg")

            # ------------------------------------------------------------------
            # iv) Copy arts.cfg to PROJECT_ROOT_DIR/build/examples/cpu/
            # ------------------------------------------------------------------
            BUILD_CPU_DIR.mkdir(parents=True, exist_ok=True)
            shutil.copy2(arts_cfg_path, BUILD_CPU_DIR / "arts.cfg")

            # ------------------------------------------------------------------
            # v) Change dir to PROJECT_ROOT_DIR/build/examples/cpu/
            # ------------------------------------------------------------------
            exec_dir = BUILD_CPU_DIR

            # ------------------------------------------------------------------
            # vi) Run ./run_cxl.sh ./lulesh_arts <inputs>
            # ------------------------------------------------------------------
            lulesh_args = LULESH_INPUTS[i]
            cmd = ["./run_cxl.sh", "./lulesh_arts"] + lulesh_args

            print(f"[nodes={i}] run={j}: running {' '.join(cmd)} in {exec_dir}")

            result = subprocess.run(
                cmd,
                cwd=str(exec_dir),
                capture_output=True,
                text=True,
            )

            combined_output = result.stdout + result.stderr
            success = "Final Origin Energy" in combined_output

            if success:
                print(f"[nodes={i}] run={j}: SUCCESS")
                inputs_data = {
                    "node_count": i,
                    "run": j,
                    "args": lulesh_args,
                    "cmd": cmd,
                }
                inputs_json_path = noted_dir / "inputs.json"
                inputs_json_path.write_text(json.dumps(inputs_data, indent=2))
            else:
                print(
                    f"[nodes={i}] run={j}: FAILED (return code {result.returncode})"
                )
                print(combined_output[-2000:] if combined_output else "(no output)")

            # ------------------------------------------------------------------
            # vii) Change dir back to …/experiments/lulesh/<i>
            # ------------------------------------------------------------------
            # (current_dir is already node_count_dir; no mutation needed)

        # ----------------------------------------------------------------------
        # iv) Change dir to ../  (back to working_dir for next i)
        # ----------------------------------------------------------------------
        # (working_dir is unchanged; loop continues from there)

        print(f"[nodes={i}] All {NUM_RUNS} runs complete.\n")


if __name__ == "__main__":
    main()
