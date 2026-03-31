AVB_FEATURE_ENDPOINT ?= 1
AVB_FEATURE_GSTREAMER ?= 0
PLATFORM_TOOLCHAIN ?= x86_i210_linux
ifeq ($(PLATFORM_TOOLCHAIN),x86_stock_igb_linux)
IGB_LAUNCHTIME_ENABLED ?= 0
ATL_LAUNCHTIME_ENABLED ?= 0
SOCKET_LAUNCHTIME_ENABLED ?= 1
else
IGB_LAUNCHTIME_ENABLED ?= 1
ATL_LAUNCHTIME_ENABLED ?= 0
SOCKET_LAUNCHTIME_ENABLED ?= 0
endif

.PHONY: all clean

all: build/Makefile
	$(MAKE) -s -C build install

doc: build/Makefile
	$(MAKE) -s -C build doc
	@echo "\n\nTo display documentation use:\n\n" \
	      "\txdg-open $(abspath build/documents/api_docs/index.html)\n"

clean:
	$(RM) -r build

build/Makefile:
	mkdir -p build && \
	cd build && \
	cmake -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_TOOLCHAIN_FILE=../platform/Linux/$(PLATFORM_TOOLCHAIN).cmake \
	      -DAVB_FEATURE_ENDPOINT=$(AVB_FEATURE_ENDPOINT) \
	      -DIGB_LAUNCHTIME_ENABLED=$(IGB_LAUNCHTIME_ENABLED) \
	      -DATL_LAUNCHTIME_ENABLED=$(ATL_LAUNCHTIME_ENABLED) \
	      -DSOCKET_LAUNCHTIME_ENABLED=$(SOCKET_LAUNCHTIME_ENABLED) \
	      -DAVB_FEATURE_GSTREAMER=$(AVB_FEATURE_GSTREAMER) \
	      ..
