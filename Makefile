GOOSE ?= .toolchain/goose/build/goose
CC ?= gcc
.PHONY: setup format check build debug test sanitize
setup:
	./scripts/setup-goose.sh
format:
	python3 scripts/native-manifest.py
	"$(GOOSE)" format search.goose src
check:
	python3 scripts/check-vendor.py
	python3 tests/vendor.py
	python3 scripts/native-manifest.py --check
	python3 vendor/goose-regex/scripts/native-manifest.py --check
	python3 vendor/goose-fs/scripts/native-manifest.py --check
	"$(GOOSE)" format --check search.goose src
	"$(GOOSE)" check search.goose
	python3 -m py_compile scripts/*.py tests/*.py
	git diff --check
build:
	mkdir -p .build
	"$(GOOSE)" build search.goose --standalone --cc "$(CC)" -O2 -o .build/goose-search
debug:
	mkdir -p .build
	"$(GOOSE)" build search.goose --standalone --debug --no-bce --cc "$(CC)" -o .build/goose-search-debug
test: build debug
	python3 tests/cli.py --tool .build/goose-search
	python3 tests/cli.py --tool .build/goose-search-debug
sanitize:
	mkdir -p .build
	"$(GOOSE)" build search.goose --standalone --debug --no-bce --cc "$$(python3 scripts/sanitizer-compiler.py --cc "$(CC)")" -o .build/goose-search-sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/cli.py --tool .build/goose-search-sanitize

.PHONY: native test-races sanitize-races
native:
	mkdir -p .build
	$(CC) -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer tests/native_output.c native/output.c -Wl,--wrap=write -o .build/native-output
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/native-output
test-races:
	mkdir -p .build
	"$(GOOSE)" build search.goose --standalone --cc "$$(python3 scripts/race-compiler.py --cc "$(CC)")" -O2 -o .build/goose-search-races
	python3 tests/races.py --tool .build/goose-search-races
sanitize-races:
	mkdir -p .build
	"$(GOOSE)" build search.goose --standalone --debug --no-bce --cc "$$(python3 scripts/race-compiler.py --cc "$(CC)" --sanitize)" -o .build/goose-search-races-sanitize
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/races.py --tool .build/goose-search-races-sanitize
