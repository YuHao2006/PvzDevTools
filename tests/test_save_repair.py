"""Exercise the standalone repair against a reported DAT64 checkpoint."""
import argparse
import hashlib
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("tool", type=pathlib.Path)
parser.add_argument("checkpoint", type=pathlib.Path)
parser.add_argument("--matches", type=int, required=True)
args = parser.parse_args()
tool, checkpoint = args.tool.resolve(), args.checkpoint.resolve()
original = checkpoint.read_bytes()
def run(*paths, ok=True):
    result = subprocess.run([str(tool), *map(str, paths)], capture_output=True, text=True)
    assert (result.returncode == 0) == ok, result.stdout + result.stderr
    return result.stdout
assert f"matches={args.matches}" in run(checkpoint)
with tempfile.TemporaryDirectory(prefix="pvz-save-repair-") as folder:
    output = pathlib.Path(folder) / "repaired.dat64"
    assert f"matches={args.matches}" in run(checkpoint, output)
    assert "matches=0" in run(output)
    fixed = output.read_bytes()
    assert len(fixed) == len(original)
    # A zero remaining timer becomes exactly one nanosecond, one byte per plant.
    changed = [(a, b) for a, b in zip(original, fixed) if a != b]
    assert changed == [(0, 1)] * args.matches, changed
    run(checkpoint, output, ok=False)
    assert output.read_bytes() == fixed
    run(checkpoint, checkpoint, ok=False)
    malformed = pathlib.Path(folder) / "invalid.dat64"
    malformed.write_bytes(b"not a save")
    rejected = pathlib.Path(folder) / "rejected.dat64"
    run(malformed, rejected, ok=False)
    assert not rejected.exists()
assert checkpoint.read_bytes() == original
print(f"PASS: {args.matches} exact repairs; scan, idempotency, full validation, overwrite refusal; input sha256="
      + hashlib.sha256(original).hexdigest())
