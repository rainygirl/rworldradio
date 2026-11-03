# R WorldRadio for Mac OS 9 - PowerPC CFM app built with Retro68 (docker).
RETRO68 := ghcr.io/autc04/retro68:latest
TOOLCHAIN := /Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake

all:
	docker run --rm --platform linux/amd64 -v "$(CURDIR)":/src -w /src $(RETRO68) \
	  sh -c 'cmake -S mac -B build/mac -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) >/dev/null && cmake --build build/mac'
	@echo "-> build/mac/RWorldRadio.bin (MacBinary)"

clean:
	rm -rf build
.PHONY: all clean
