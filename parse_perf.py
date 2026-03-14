import sys

lines = sys.stdin.read().splitlines()
start = False
for line in lines:
    if "User   System     Real  Name" in line:
        start = True
        continue
    if not start:
        continue
    if not line.startswith(" "):
        continue
    
    # only print top level (4 spaces)
    if line.startswith("    ") and not line.startswith("      "):
        print(line)

