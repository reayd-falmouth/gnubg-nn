# setup.py - clean Python 3 extension build for GNUBG neural net interface

from setuptools import setup, Extension
from glob import glob

# Clean C++ and C interface files
cpp_sources = [
    "py3mod.cpp",
    "../gnubg/bearoffgammon.cc",
    "../gnubg/racebg.cc",
    "../gnubg/osr.cc",
]
c_sources = glob("../gnubg/*.c") + glob("../gnubg/lib/*.c") + glob("../analyze/*.cc")

# Define the extension module
gnubg_module = Extension(
    "gnubg",
    sources=cpp_sources + c_sources,
    include_dirs=["../", "../gnubg", "../gnubg/lib", "../analyze", "../py"],
    define_macros=[
        ("LOADED_BO", "1"),
        ("OS_BEAROFF_DB", "1"),
    ],
    extra_compile_args=["-std=c++11"],
)

# Setup declaration
setup(
    name="gnubg",
    version="1.01",
    ext_modules=[gnubg_module],
    package_data={
        '': ['data/*.bd', 'data/*.weights', 'data/*.db']
    },
    include_package_data=True,
    description='Python bindings for GNUBG neural evaluation',
    author='David Reay',
    author_email='dr323090@falmouth.ac.uk',
)
