.DEFAULT_GOAL := build

CMAKE ?= cmake
BUILD_DIR ?= build-cmake
BUILD_TYPE ?= Release
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

INDEX ?= build/index_k8192.ivfi16
NLIST ?= 8192
REFERENCES ?= resources/references.json.gz
QUERIES ?= test/test-data.json
VALIDATE_LIMIT ?= 1000

.PHONY: configure build debug test coverage format format-check lint check index validate clean distclean help

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

debug:
	$(CMAKE) --preset debug
	$(CMAKE) --build --preset debug

test: build
	$(CMAKE) --build $(BUILD_DIR) --target test

coverage:
	$(CMAKE) --preset coverage
	$(CMAKE) --build --preset coverage
	$(CMAKE) --build --preset coverage-report

format: configure
	$(CMAKE) --build $(BUILD_DIR) --target format

format-check: configure
	$(CMAKE) --build $(BUILD_DIR) --target format-check

lint: configure
	$(CMAKE) --build $(BUILD_DIR) --target lint

check: build
	$(CMAKE) --build $(BUILD_DIR) --target check

$(INDEX): $(REFERENCES) build
	mkdir -p $(dir $(INDEX))
	./$(BUILD_DIR)/build-index --references $(REFERENCES) --out $(INDEX) --nlist $(NLIST)

index: $(INDEX)

validate: $(INDEX)
	./$(BUILD_DIR)/validate-index --index $(INDEX) --queries $(QUERIES) --limit $(VALIDATE_LIMIT)

clean:
	rm -rf $(BUILD_DIR) build-cmake-debug build-cmake-coverage

distclean: clean
	rm -f $(INDEX)

help:
	@printf "%s\n" \
	  "Targets:" \
	  "  make build          Configure and build Release" \
	  "  make debug          Configure and build Debug preset" \
	  "  make test           Build and run CTest" \
	  "  make coverage       Build and run coverage-instrumented tests" \
	  "  make format         Apply clang-format" \
	  "  make format-check   Check clang-format" \
	  "  make lint           Run clang-tidy" \
	  "  make check          Build, format-check and lint" \
	  "  make index          Generate build/index_k8192.ivfi16" \
	  "  make validate       Generate index if needed and validate sample"
