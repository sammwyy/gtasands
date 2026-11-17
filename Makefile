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
.PHONY: all build build-ds build-dsi build-sd-ds build-sd-dsi build-quick single stage-sd sdcard sd dldi run run-ds run-sd run-sd-ds assets sync radio-sync radio-assets package-sd
.DEFAULT_GOAL := all
all: build
build build-ds build-dsi build-sd-ds build-sd-dsi build-quick single stage-sd sdcard sd dldi:
	@if [ "$(ASSETS)" != "0" ]; then $(MAKE) -f Makefile.blocksds nitrofs/.stamp; fi
	@docker run --rm --user "$$(id -u):$$(id -g)" -v "$(CURDIR):/work:Z" -w /work $(IMAGE) \
		make -f Makefile.blocksds -j$(JOBS) ASSETS=0 DEBUG=$(DEBUG) QUICKTEST=$(QUICKTEST) CUTSKIPTEST=$(CUTSKIPTEST) \
		MODE=$(MODE) STORAGE=$(STORAGE) $(if $(filter undefined,$(origin OPT)),,OPT='$(OPT)') $@
run: build-dsi
	node tools/run.mjs dsi rom "$(EMU)"
run-ds: build-ds
	node tools/run.mjs ds rom "$(EMU)"
run-sd: build-sd-dsi stage-sd
	node tools/run.mjs dsi sd "$(EMU)"
run-sd-ds: build-sd-ds stage-sd
	node tools/run.mjs ds sd "$(EMU)"
assets sync radio-sync radio-assets package-sd:
	@$(MAKE) -f Makefile.blocksds $@
else
include Makefile.blocksds
endif
