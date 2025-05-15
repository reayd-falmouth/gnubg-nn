#!/bin/bash

# Clean previous builds
rm -rf build/ gnubg.cpython-310-*.so
rm -rf build/ gnubg.cpython-*.so

# Clean and rebuild the Python extension
poetry run python setup.py clean --all
poetry run python setup.py build_ext --inplace
