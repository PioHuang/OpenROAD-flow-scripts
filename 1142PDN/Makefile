PYTHON ?= python3
MANIFEST ?= ../research/mempool.json
DUMP ?= out/latest
PORT ?= 8010

.PHONY: run gui clean

run:
	$(PYTHON) src/main.py --manifest "$(MANIFEST)" --dump-dir "$(DUMP)"

gui:
	$(PYTHON) gui/serve.py --host 0.0.0.0 --port "$(PORT)"

clean:
	rm -rf out/latest
