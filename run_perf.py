import json
import subprocess
import os

with open("benchmarks/out/targets/tools-mcp/captures/1773472730265-11755-tools_mcp-ee36fed0ce61b872.json") as f:
    data = json.load(f)

cmd = ["/Users/dcazares/Documents/Code/forged/build/ld64", "-perf"] + data["derived_ld_argv"][1:]
cwd = data["cwd"]

subprocess.run(cmd, cwd=cwd)
