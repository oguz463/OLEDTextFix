#!/usr/bin/env python3
"""Extracts the HLSL from src/main.cpp (kShader) and syntax-checks each entry point with glslangValidator's HLSL
front end. It does not replace compiling with fxc on Windows, but catches syntax and type errors in CI."""
import re, subprocess, sys, tempfile, os
src = open(os.path.join(os.path.dirname(__file__), '..', 'src', 'main.cpp'), encoding='utf-8').read()
m = re.search(r'kShader\[\] = R"HLSL\((.*?)\)HLSL";', src, re.S)
if not m:
    sys.exit('kShader not found')
hlsl = m.group(1)
ok = True
for entry, stage in (('VSMain', 'vert'), ('PSMain', 'frag'), ('PSCancel', 'frag'), ('PSDiff', 'frag')):
    with tempfile.NamedTemporaryFile('w', suffix='.hlsl', delete=False) as f:
        f.write(hlsl)
    r = subprocess.run(['glslangValidator', '-V', '-D', '-S', stage, '-e', entry, '-o', os.devnull, f.name], capture_output=True, text=True)
    os.unlink(f.name)
    print(entry, 'OK' if r.returncode == 0 else 'FAILED')
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        ok = False
sys.exit(0 if ok else 1)
