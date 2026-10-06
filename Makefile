.DEFAULT_GOAL := help
FIRMWARE_MANAGER_DIR ?= ../cardputer-firmware-manager
WORKSPACE ?= $(abspath ..)
SD ?= /Volumes/CARDPUTER

.PHONY: help build flash stage doctor check
help:
	@printf '%s\n' 'make build - build firmware without SD' 'make flash [SD=/Volumes/CARDPUTER] - build and stage on SD' 'make stage - stage existing build' 'make doctor - validate mounted SD' 'make check - verify firmware'

build flash stage:
	$(MAKE) -C "$(FIRMWARE_MANAGER_DIR)" $@ APP=codex WORKSPACE="$(WORKSPACE)" SD="$(SD)"

doctor:
	$(MAKE) -C "$(FIRMWARE_MANAGER_DIR)" doctor-sd SD="$(SD)"

.PHONY: test
test:
	./tools/test.sh

check: test
	python3 tools/audit_public_tree.py
	$(MAKE) build
