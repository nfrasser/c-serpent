# Everything except the extension module is declared in pyproject.toml.
# This file exists only because declaring ext-modules there is still
# experimental in setuptools.

from setuptools import setup, Extension

setup(ext_modules=[Extension("_cserpent", sources=["cserpent_py.c"])])
