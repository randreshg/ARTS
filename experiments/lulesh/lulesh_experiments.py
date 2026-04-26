#!/usr/bin/env python3
"""
LULESH scaling experiment script for the Crete cluster.

For each node count i (1..num_nodes) and each run j (0..NUM_RUNS-1):
  1. Creates experiments/lulesh/<i>/<j>/counters/
  2. Generates a tailored arts.cfg from the round-robin sample config.
  3. Copies the config to the build directory.
  4. Runs ./run_cxl.sh ./lulesh_arts with size/tile/iter inputs scaled to i.
  5. On success (output contains "Final Origin Energy"), saves inputs.json.
  6. On failure (timeout, missing success string, or any error):
       - Cleans up counters/ and inputs.json
       - SSHes to each node and pkills lulesh_arts
       - Retries up to MAX_RETRIES times (sleeping 10 s after pkill)
"""

import json
import os
import re
import shutil
import subprocess
import time
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

MAX_RETRIES = 5

TIMEOUT_SECONDS = 15 * 60  # 15 minutes

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


def cleanup_run(counters_dir: Path, inputs_json_path: Path) -> None:
    """Remove all files in counters_dir and the inputs.json file if they exist."""
    if counters_dir.exists():
        for item in counters_dir.iterdir():
            if item.is_file():
                item.unlink()
            elif item.is_dir():
                shutil.rmtree(item)
        print(f"  [cleanup] Cleared contents of {counters_dir}")
    if inputs_json_path.exists():
        inputs_json_path.unlink()
        print(f"  [cleanup] Removed {inputs_json_path}")


def pkill_on_all_nodes(nodes: list) -> None:
    """SSH to each node in turn and pkill -9 -x lulesh_arts."""
    for node in nodes:
        print(f"  [pkill] SSHing to {node} to kill lulesh_arts …")
        try:
            subprocess.run(
                ["ssh", node, "pkill", "-9", "-x", "lulesh_arts"],
                timeout=30,
            )
        except subprocess.TimeoutExpired:
            print(f"  [pkill] SSH to {node} timed out; continuing.")
        except Exception as exc:
            print(f"  [pkill] SSH to {node} raised {exc}; continuing.")


def run_lulesh(cmd: list, exec_dir: Path) -> tuple:
    """
    Run the lulesh command with a timeout.

    Returns (success: bool, combined_output: str, timed_out: bool).
    """
    timed_out = False
    combined_output = ""
    success = False

    try:
        result = subprocess.run(
            cmd,
            cwd=str(exec_dir),
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
        )
        combined_output = result.stdout + result.stderr
        success = "Final Origin Energy" in combined_output
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        combined_output = (exc.stdout or "") + (exc.stderr or "")
        print(f"  [timeout] Process exceeded {TIMEOUT_SECONDS}s and was terminated.")

    return success, combined_output, timed_out

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
            inputs_json_path = run_dir_abs / "inputs.json"

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
            # vi) Run ./run_cxl.sh ./lulesh_arts <inputs> with retry logic
            # ------------------------------------------------------------------
            lulesh_args = LULESH_INPUTS[i]
            cmd = ["./run_cxl.sh", "./lulesh_arts"] + lulesh_args

            print(f"[nodes={i}] run={j}: running {' '.join(cmd)} in {exec_dir}")

            success = False
            active_nodes = node_names[:i]

            for attempt in range(MAX_RETRIES + 1):
                if attempt > 0:
                    print(f"[nodes={i}] run={j}: retry attempt {attempt}/{MAX_RETRIES}")

                success, combined_output, timed_out = run_lulesh(cmd, exec_dir)

                if success:
                    print(f"[nodes={i}] run={j}: SUCCESS (attempt {attempt})")
                    inputs_data = {
                        "node_count": i,
                        "run": j,
                        "args": lulesh_args,
                        "cmd": cmd,
                    }
                    inputs_json_path.write_text(json.dumps(inputs_data, indent=2))
                    break

                # ---- FAILED (timeout, missing success string, or other) ----
                reason = "timeout" if timed_out else "missing 'Final Origin Energy'"
                print(
                    f"[nodes={i}] run={j}: FAILED — {reason} (attempt {attempt})"
                )
                print(combined_output[-2000:] if combined_output else "(no output)")

                # a) Clean up counters and inputs.json
                cleanup_run(counters_dir, inputs_json_path)

                if attempt < MAX_RETRIES:
                    # b) SSH to all nodes and pkill lulesh_arts
                    pkill_on_all_nodes(active_nodes)

                    # c) Sleep 10 seconds before retrying
                    print(f"  [retry] Sleeping 10 s before next attempt …")
                    time.sleep(10)
                else:
                    print(
                        f"[nodes={i}] run={j}: Exhausted {MAX_RETRIES} retries. "
                        "Skipping this run."
                    )

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
