.DEFAULT_GOAL := help

help:
	@echo "Usage: make <demo>   (e.g. make sdf, make post, make fluid)"

%:
	@test -f build/cm/Makefile || cmake -B build/cm -DCMAKE_BUILD_TYPE=Release
	@cmake --build build/cm --target $@ -j$$(nproc)

.PHONY: help
