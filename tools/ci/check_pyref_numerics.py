#!/usr/bin/env python3
"""Require the measured numerical reference, never relax comparison assertions."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys

import numpy as np

features = np._core._multiarray_umath.__cpu_features__
dispatch = {k: bool(features.get(k, False)) for k in ('X86_V3', 'X86_V4', 'AVX512_ICL', 'AVX512_SPR')}
kernel = None
assert isinstance(np.__file__, str)
for path in (Path(np.__file__).parent.parent / 'numpy.libs').glob('*openblas*'):
    lib = ctypes.CDLL(str(path))
    for name in ('scipy_openblas_get_corename64_', 'openblas_get_corename64_', 'openblas_get_corename'):
        try:
            f = getattr(lib, name)
        except AttributeError:
            continue
        f.restype = ctypes.c_char_p
        kernel = f().decode()
        break
    if kernel is not None:
        break
result = {'numpy': np.__version__, 'kernel': kernel, 'simd_dispatch': dispatch,
          'OPENBLAS_CORETYPE': os.environ.get('OPENBLAS_CORETYPE'),
          'NPY_DISABLE_CPU_FEATURES': os.environ.get('NPY_DISABLE_CPU_FEATURES')}
print(json.dumps(result), flush=True)
if np.__version__ != '2.4.6':
    raise RuntimeError('unexpected numerical reference version')
if kernel != 'Haswell':
    raise RuntimeError('unexpected BLAS kernel (bitwise baseline requires Haswell)')
if any(dispatch.values()):
    raise RuntimeError('unexpected SIMD dispatch (baseline requires X86_V2)')
probe = subprocess.run([sys.executable, 'tools/migration/geom3d_harness.py', 'blas'],
                       capture_output=True, text=True, timeout=90)
print(probe.stdout, end='')
if probe.returncode != 0:
    raise RuntimeError(probe.stderr)
if json.loads(probe.stdout)['ok'] is not True:
    raise RuntimeError('BLAS arithmetic differs from the C++ reference')
