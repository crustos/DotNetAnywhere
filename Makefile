# Everything is built by build.py; this Makefile only calls it.
#
#   make                       python3 build.py        (the native runtime, and corlib.dll if mcs is installed)
#   make ARGS="--m32"          python3 build.py --m32  (any build.py option can be passed in ARGS)
#   make test                  build, then run the test suite
#   make clean                 python3 build.py --clean

PYTHON ?= python3
ARGS ?=

.DEFAULT_GOAL := all
.PHONY: all test clean

all:
	$(PYTHON) build.py $(ARGS)

test: all
	$(PYTHON) tests/run_tests.py

clean:
	$(PYTHON) build.py --clean
