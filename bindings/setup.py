"""
setup.py — build the pyhft Python extension.

From the repo root:
    pip install pybind11 setuptools
    python3 bindings/setup.py build_ext --inplace

That produces a `pyhft*.so` next to setup.py (or in the build/ dir).
Add `bindings/` to sys.path and `import pyhft` works.
"""

from pybind11.setup_helpers import Pybind11Extension, build_ext
from setuptools import setup
import os


HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)


ext_modules = [
    Pybind11Extension(
        name="pyhft",
        sources=[os.path.join("bindings", "pyhft.cpp")],
        include_dirs=[REPO_ROOT],
        cxx_std=17,
        extra_compile_args=["-O2"],
    ),
]


setup(
    name="pyhft",
    version="0.1",
    description="Python bindings for hft-infra-lab",
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
    zip_safe=False,
)
