from pathlib import Path

completion_str = "Verified"

log_files = list(Path('build/examples/cpu/').glob('*.log'))

failed_files = []
counter = 0
for f in log_files:
    found = False
    contents = open(f, "r").read().split("\n")
    for c in contents:
        if (completion_str in c):
            counter += 1
            found = True
    if (not found):
        failed_files.append(f)

print(f"Total runs: {len(log_files)}")
print(f"Successful runs: {counter}")

if (len(log_files) == counter):
    print("Runs successful!")
else:
    print("Runs failed!")
    print("Failed files:", failed_files)
(basic) (basic) [alas515@twosisters2 cpu]$ cat ../../../check_logs.py 
from pathlib import Path

#completion_str = "Solution Validates"
#completion_str = "Verified"
#completion_str = "Fib 25: 75025"
completion_str = "Final Origin Energy"
#completion_str = "PASS"

log_files = list(Path('build/examples/cpu/').glob('*.log'))
#log_files = list(Path('build_nonCXL/examples/cpu/').glob('*.log'))

failed_files = []
counter = 0
for f in log_files:
    found = False
    contents = open(f, "r").read().split("\n")
    for c in contents:
        if (completion_str in c):
            counter += 1
            found = True
    if (not found):
        failed_files.append(f)

print(f"Total runs: {len(log_files)}")
print(f"Successful runs: {counter}")

if (len(log_files) == counter):
    print("Runs successful!")
else:
    print("Runs failed!")
    print("Failed files:", failed_files)