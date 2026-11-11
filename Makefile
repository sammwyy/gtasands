BLOCKSDS ?= /opt/wonderful/thirdparty/blocksds/core

ifeq ($(wildcard $(BLOCKSDS)/sys/crts/ds_arm9.specs),)
IMAGE ?= skylyrac/blocksds:slim-v1.24.0
JOBS ?= 16
ASSETS ?= 1
DEBUG ?= 0
QUICKTEST ?= 0
CUTSKIPTEST ?= 0
MODE ?= ds
STORAGE ?= rom
EMU ?= melonds
.PHONY: all build build-ds build-dsi build-sd-ds build-sd-dsi build-quick single stage-sd sdcard sd dldi run run-ds run-sd run-sd-ds assets sync radio-sync radio-assets
.DEFAULT_GOAL := all
all: build
build build-ds build-dsi build-sd-ds build-sd-dsi build-quick single stage-sd sdcard sd dldi:
	@if [ "$(ASSETS)" != "0" ]; then $(MAKE) -f Makefile.blocksds nitrofs/.stamp; fi
	@docker run --rm --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/work:Z" -w /work $(IMAGE) \
		make -f Makefile.blocksds -j$(JOBS) ASSETS=0 DEBUG=$(DEBUG) QUICKTEST=$(QUICKTEST) CUTSKIPTEST=$(CUTSKIPTEST) \
		MODE=$(MODE) STORAGE=$(STORAGE) $(if $(filter undefined,$(origin OPT)),,OPT='$(OPT)') $@
run: build-dsi
	python3 tools/run.py dsi rom "$(EMU)"
run-ds: build-ds
	python3 tools/run.py ds rom "$(EMU)"
run-sd: build-sd-dsi stage-sd
	python3 tools/run.py dsi sd "$(EMU)"
run-sd-ds: build-sd-ds stage-sd
	python3 tools/run.py ds sd "$(EMU)"
assets:
	python3 tools/assets.py build
sync:
	python3 tools/assets.py sync
radio-sync:
	python3 tools/assets.py sync radio_streams
radio-assets:
	python3 tools/assets.py build radio_streams
else
include Makefile.blocksds
endif
