from pathlib import Path                                                                                                                 │(basic) (basic) [alas515@twosisters2 ARTS]$ bc -l
                                                                                                                                         │bc 1.07.1
completion_str = "Verified"                                                                                                             │This is free software with ABSOLUTELY NO WARRANTY.
                                                                                                                                         │12800/(192*4)
log_files = list(Path('build/examples/cpu/').glob('*.log'))                                                                              │16.66666666666666666666
                                                                                                                                         │760
failed_files = []                                                                                                                        │760 * 760
counter = 0                                                                                                                              │577600
for f in log_files:                                                                                                                      │760 * 10
    found = False                                                                                                                        │7600
    contents = open(f, "r").read().split("\n")                                                                                           │760 * 256
    for c in contents:                                                                                                                   │194560
        if (completion_str in c):                                                                                                        │760 * 128
            counter += 1                                                                                                                 │97280
            found = True                                                                                                                 │2621440/760
    if (not found):                                                                                                                      │3449.26315789473684210526
        failed_files.append(f)                                                                                                           │760*1280
                                                                                                                                         │972800
print(f"Total runs: {len(log_files)}")                                                                                                   │
print(f"Successful runs: {counter}")                                                                                                     │
                                                                                                                                         │
if (len(log_files) == counter):                                                                                                          │
    print("Runs successful!")                                                                                                            │
else:                                                                                                                                    │
    print("Runs failed!")                                                                                                                │
    print("Failed files:", failed_files)
