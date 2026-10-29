# AlgaOps Assurance (Linux / Raspberry Pi OS).  Windows: scripts\*.ps1.
.PHONY: all build core firmware-host dashboard firmware test test-quick test-browser demo verify clean

PY      ?= .venv/bin/python
CMAKE   ?= cmake
CORE    := algaops-assurance
HOSTT   := firmware/esp32-sensor-node/host_test

all build:
	bash scripts/build_all.sh

core:
	$(CMAKE) -S $(CORE) -B $(CORE)/build -DCMAKE_BUILD_TYPE=Release -DALGAOPS_WERROR=ON
	$(CMAKE) --build $(CORE)/build -j

firmware-host:
	$(CMAKE) -S $(HOSTT) -B $(HOSTT)/build -DNODE_HOST_WERROR=ON
	$(CMAKE) --build $(HOSTT)/build -j

dashboard:
	cd dashboard && npm ci --no-audit --no-fund && npm run build

firmware:
	bash scripts/build_firmware.sh

test:
	bash scripts/test_all.sh

test-quick:
	bash scripts/test_all.sh --quick

test-browser:
	bash scripts/test_all.sh --browser

demo:
	bash scripts/run_demo.sh

# Independent check of an evidence bundle: make verify BUNDLE=path/to/bundle.zip
verify:
	$(PY) -I tools/verify_evidence.py $(BUNDLE)

clean:
	rm -rf $(CORE)/build $(HOSTT)/build dashboard/dist
