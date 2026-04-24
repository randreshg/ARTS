#!/usr/bin/env python3
"""
Bandwidth profiling experiment script for the Crete cluster.

For each node count i (1 to num_nodes inclusive), this script:
  1. Creates a per-run output directory named after i.
  2. Generates a tailored arts.cfg (round-robin CXL, i nodes).
  3. Copies the config to the build directory.
  4. Runs ./run_cxl.sh ./STREAM -o <output_dir>, directing output to the
     per-run directory.
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

num_nodes = len(node_names)

# ==============================================================================
# PATHS
# ==============================================================================

# Absolute path to the project root (two levels up from this script's directory)
SCRIPT_DIR = Path(__file__).resolve().parent          # …/experiments/bandwidth_profiling
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
        # ------------------------------------------------------------------
        # i)  Create per-run directory named after i inside the working directory
        # ------------------------------------------------------------------
        run_dir = working_dir / str(i)
        run_dir.mkdir(parents=True, exist_ok=True)

        # ------------------------------------------------------------------
        # ii) "Change directory" to i (track as current dir)
        # ------------------------------------------------------------------
        current_dir = run_dir

        # ------------------------------------------------------------------
        # a) Copy sample config to run directory, rename to arts.cfg
        # ------------------------------------------------------------------
        arts_cfg_path = current_dir / "arts.cfg"
        shutil.copy2(SAMPLE_CFG, arts_cfg_path)

        # ------------------------------------------------------------------
        # b) Edit the copied arts.cfg
        # ------------------------------------------------------------------
        cfg_content = arts_cfg_path.read_text()

        # i)   Change master_node to node_names[0]
        cfg_content = set_cfg_value(cfg_content, "master_node", node_names[0])

        # ii)  Change node_count to i
        cfg_content = set_cfg_value(cfg_content, "node_count", str(i))

        # iii) Change nodes to node_names[0]...node_names[i-1]
        #      (i nodes: indices 0 through i-1, comma-separated)
        nodes_value = ",".join(node_names[0:i])
        cfg_content = set_cfg_value(cfg_content, "nodes", nodes_value)

        # ------------------------------------------------------------------
        # c) Save file; note the current directory for -o flag
        # ------------------------------------------------------------------
        arts_cfg_path.write_text(cfg_content)
        output_dir = current_dir  # noted working directory for -o flag

        # ------------------------------------------------------------------
        # d) Copy arts.cfg to PROJECT_ROOT_DIR/build/examples/cpu/
        # ------------------------------------------------------------------
        BUILD_CPU_DIR.mkdir(parents=True, exist_ok=True)
        shutil.copy2(arts_cfg_path, BUILD_CPU_DIR / "arts.cfg")

        # ------------------------------------------------------------------
        # e) Change dir to PROJECT_ROOT_DIR/build/examples/cpu/
        # ------------------------------------------------------------------
        exec_dir = BUILD_CPU_DIR

        # ------------------------------------------------------------------
        # f) Run ./run_cxl.sh ./STREAM -o "<noted working directory>"
        # ------------------------------------------------------------------
        cmd = ["./run_cxl.sh", "./STREAM", "-o", str(output_dir)]
        print(f"[node_count={i}] nodes={nodes_value}: running {' '.join(cmd)} in {exec_dir}")
        subprocess.run(cmd, cwd=str(exec_dir), check=True)

        # ------------------------------------------------------------------
        # g) Return to experiments/bandwidth_profiling
        # ------------------------------------------------------------------
        # (Python tracks directories logically; next iteration uses working_dir)
        print(f"[node_count={i}] Run complete. Output saved to {output_dir}\n")


if __name__ == "__main__":
    main()
