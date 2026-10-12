CC ?= cc
MINGW_CC ?= x86_64-w64-mingw32-gcc
LEECHCORE_DIR ?= references/LeechCore
LEECHCORE_SO ?= $(LEECHCORE_DIR)/files/leechcore.so
MEMPROCFS_DIR ?= references/MemProcFS
VMM_SO ?= $(MEMPROCFS_DIR)/files/vmm.so
BUILD_DIR ?= build
PLUGIN := leechcore_device_proton.so
EXAMPLE := $(BUILD_DIR)/proton_example
BENCHMARK := $(BUILD_DIR)/proton_benchmark
VMM_CPPFLAGS := -isystem $(MEMPROCFS_DIR)/includes -isystem $(LEECHCORE_DIR)/includes

CPPFLAGS += -Iinclude -I$(LEECHCORE_DIR)/leechcore -D_GNU_SOURCE -D LINUX
CFLAGS += -std=c11 -O2 -fPIC -fvisibility=hidden -fstack-protector-strong \
	-Wall -Wextra -Werror -Wpedantic -Wno-unused-variable
LDFLAGS += -Wl,-z,noexecstack

CORE_SOURCES := src/config.c src/process_maps.c src/synthetic_paging.c src/process_memory.c
PLUGIN_SOURCES := $(CORE_SOURCES) src/device_proton.c
PLUGIN_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(PLUGIN_SOURCES))

.PHONY: all benchmark benchmark-fixture check-leechcore check-vmm clean example \
	fixture install-dev test test-unit test-integration test-sanitized tools

all: $(PLUGIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c include/proton_device.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/device_proton.o: CFLAGS += -Wno-pedantic

check-leechcore:
	@test -f "$(LEECHCORE_SO)" || { \
		echo "Missing $(LEECHCORE_SO). Build LeechCore or set LEECHCORE_SO." >&2; \
		exit 1; \
	}

check-vmm: check-leechcore
	@test -f "$(VMM_SO)" || { \
		echo "Missing $(VMM_SO). Build MemProcFS vmm.so or set VMM_SO." >&2; \
		exit 1; \
	}

$(PLUGIN): $(PLUGIN_OBJECTS) check-leechcore
	$(CC) -shared -o $@ $(PLUGIN_OBJECTS) $(LDFLAGS) \
		-L$(dir $(LEECHCORE_SO)) -Wl,-rpath,'$$ORIGIN' -l:$(notdir $(LEECHCORE_SO))

$(EXAMPLE): examples/example.c check-vmm | $(BUILD_DIR)
	$(CC) $(VMM_CPPFLAGS) \
		-D_GNU_SOURCE -D LINUX -std=c11 -O2 -Wall -Wextra -Werror \
		-Wno-pedantic -Wno-strict-prototypes -o $@ $< \
		-L$(dir $(VMM_SO)) -l:$(notdir $(VMM_SO)) \
		-L$(dir $(LEECHCORE_SO)) -l:$(notdir $(LEECHCORE_SO)) \
		-Wl,-rpath,'$$ORIGIN' -pthread -ldl

$(BENCHMARK): benchmarks/benchmark.c check-vmm | $(BUILD_DIR)
	$(CC) $(VMM_CPPFLAGS) \
		-D_GNU_SOURCE -D LINUX -std=c11 -O2 -Wall -Wextra -Werror \
		-Wno-pedantic -Wno-strict-prototypes -o $@ $< \
		-L$(dir $(VMM_SO)) -l:$(notdir $(VMM_SO)) \
		-L$(dir $(LEECHCORE_SO)) -l:$(notdir $(LEECHCORE_SO)) \
		-Wl,-rpath,'$$ORIGIN' -pthread -ldl

example: $(PLUGIN) $(EXAMPLE)

benchmark: $(PLUGIN) $(BENCHMARK)

benchmark-fixture: benchmark fixture
	@tests/integration/run_proton_fixture.sh --benchmark; result=$$?; \
		if test $$result -eq 77; then echo "Benchmark skipped"; exit 0; fi; \
		exit $$result

tools: example benchmark

$(BUILD_DIR)/test_config: tests/unit/test_config.c src/config.c include/proton_device.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/unit/test_config.c src/config.c

$(BUILD_DIR)/test_process_maps: tests/unit/test_process_maps.c src/process_maps.c include/proton_device.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/unit/test_process_maps.c src/process_maps.c

$(BUILD_DIR)/test_synthetic_paging: tests/unit/test_synthetic_paging.c src/process_maps.c src/synthetic_paging.c include/proton_device.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/unit/test_synthetic_paging.c src/process_maps.c src/synthetic_paging.c

$(BUILD_DIR)/test_plugin_live: tests/unit/test_plugin_live.c $(PLUGIN_SOURCES) include/proton_device.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-pedantic -o $@ tests/unit/test_plugin_live.c $(PLUGIN_SOURCES)

test-unit: $(BUILD_DIR)/test_config $(BUILD_DIR)/test_process_maps $(BUILD_DIR)/test_synthetic_paging $(BUILD_DIR)/test_plugin_live
	$(BUILD_DIR)/test_config
	$(BUILD_DIR)/test_process_maps
	$(BUILD_DIR)/test_synthetic_paging
	$(BUILD_DIR)/test_plugin_live

test: test-unit

fixture:
	$(MAKE) -C tests/fixtures MINGW_CC="$(MINGW_CC)"

test-integration: $(PLUGIN) fixture
	@tests/integration/run_proton_fixture.sh; result=$$?; \
		if test $$result -eq 77; then echo "Integration test skipped"; exit 0; fi; \
		exit $$result

test-sanitized:
	mkdir -p $(BUILD_DIR)/sanitized
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		-o $(BUILD_DIR)/sanitized/test_config tests/unit/test_config.c src/config.c
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		-o $(BUILD_DIR)/sanitized/test_process_maps tests/unit/test_process_maps.c src/process_maps.c
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		-o $(BUILD_DIR)/sanitized/test_synthetic_paging tests/unit/test_synthetic_paging.c src/process_maps.c src/synthetic_paging.c
	ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 $(BUILD_DIR)/sanitized/test_config
	ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 $(BUILD_DIR)/sanitized/test_process_maps
	ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 $(BUILD_DIR)/sanitized/test_synthetic_paging

install-dev: $(PLUGIN)
	@test -n "$(LEECHCORE_INSTALL_DIR)" || { \
		echo "Set LEECHCORE_INSTALL_DIR to the directory containing leechcore.so" >&2; \
		exit 1; \
	}
	install -m 0755 $(PLUGIN) "$(LEECHCORE_INSTALL_DIR)/$(PLUGIN)"

clean:
	rm -rf -- $(BUILD_DIR)
	rm -f -- $(PLUGIN)
	$(MAKE) -C tests/fixtures clean
