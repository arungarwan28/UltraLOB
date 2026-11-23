# UltraLOB — local build.
#
# CMakeLists.txt is the canonical build for CI and Codespaces. This Makefile is
# the no-extra-dependency path (g++ + curl for GoogleTest) used to produce the
# checked-in reports, and it builds the same targets.

CXX       ?= g++
CXXFLAGS  ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Iengine/include
DEPFLAGS  := -MMD -MP
LDLIBS    ?= -lpthread
BUILD     := build
GTEST_DIR ?= $(BUILD)/googletest
GTEST_VER := 1.15.2

ENGINE_SRC := $(wildcard engine/src/*.cpp)
ENGINE_OBJ := $(patsubst engine/src/%.cpp,$(BUILD)/obj/%.o,$(ENGINE_SRC))
TEST_SRC   := $(wildcard tests/*.cpp)
TEST_BIN   := $(BUILD)/ultralob_tests
CLI_BIN    := $(BUILD)/ultralob_cli
BENCH_BIN  := $(BUILD)/ultralob_bench
COMPARE_BIN := $(BUILD)/ultralob_compare
BUILD_FLAGS := $(CXXFLAGS)

.PHONY: all engine test cli bench compare wasm clean reports

all: test cli bench

engine: $(BUILD)/libultralob.a

$(BUILD)/obj/%.o: engine/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/libultralob.a: $(ENGINE_OBJ)
	@mkdir -p $(BUILD)
	ar rcs $@ $^

# --- GoogleTest (fetched once into build/, never vendored) -------------------
$(GTEST_DIR)/googletest/include/gtest/gtest.h:
	@mkdir -p $(BUILD)
	@echo "== fetching googletest $(GTEST_VER) =="
	@curl -sSL "https://github.com/google/googletest/archive/refs/tags/v$(GTEST_VER).tar.gz" -o $(BUILD)/gtest.tar.gz
	@tar xzf $(BUILD)/gtest.tar.gz -C $(BUILD)
	@rm -rf $(GTEST_DIR)
	@mv $(BUILD)/googletest-$(GTEST_VER) $(GTEST_DIR)
	@rm -f $(BUILD)/gtest.tar.gz

$(BUILD)/obj/gtest-all.o: $(GTEST_DIR)/googletest/include/gtest/gtest.h
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -I$(GTEST_DIR)/googletest/include -I$(GTEST_DIR)/googletest \
	  -c $(GTEST_DIR)/googletest/src/gtest-all.cc -o $@

$(BUILD)/obj/gtest_main.o: $(GTEST_DIR)/googletest/include/gtest/gtest.h
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -I$(GTEST_DIR)/googletest/include \
	  -c $(GTEST_DIR)/googletest/src/gtest_main.cc -o $@

TEST_OBJ := $(patsubst tests/%.cpp,$(BUILD)/obj/tests/%.o,$(TEST_SRC))

# GoogleTest must be present before any test TU is compiled (also after clean).
$(TEST_OBJ): $(GTEST_DIR)/googletest/include/gtest/gtest.h

$(BUILD)/obj/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -I$(GTEST_DIR)/googletest/include -c $< -o $@

# The WASM C ABI is plain C++ over the engine, so it is unit-tested natively.
$(BUILD)/obj/wasm/bindings.o: wasm/bindings.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(TEST_BIN): $(TEST_OBJ) $(BUILD)/libultralob.a $(BUILD)/obj/gtest-all.o \
             $(BUILD)/obj/gtest_main.o $(BUILD)/obj/wasm/bindings.o
	$(CXX) $(TEST_OBJ) $(BUILD)/obj/gtest-all.o $(BUILD)/obj/gtest_main.o \
	  $(BUILD)/obj/wasm/bindings.o $(BUILD)/libultralob.a $(LDLIBS) -o $@

test: $(TEST_BIN)
	@mkdir -p reports
	./$(TEST_BIN) --gtest_output=xml:reports/gtest.xml

# --- Tools -----------------------------------------------------------------
cli: $(CLI_BIN)

$(CLI_BIN): tools/ultralob_cli.cpp $(BUILD)/libultralob.a
	$(CXX) $(CXXFLAGS) $< $(BUILD)/libultralob.a $(LDLIBS) -o $@

bench: $(BENCH_BIN)

$(BENCH_BIN): benchmarks/native_benchmark.cpp benchmarks/allocation_tracker.cpp $(BUILD)/libultralob.a
	$(CXX) $(CXXFLAGS) -DULTRALOB_BUILD_FLAGS='"$(BUILD_FLAGS)"' \
	  benchmarks/native_benchmark.cpp benchmarks/allocation_tracker.cpp \
	  $(BUILD)/libultralob.a $(LDLIBS) -o $@

compare: $(COMPARE_BIN)

$(COMPARE_BIN): benchmarks/compare_benchmarks.cpp benchmarks/allocation_tracker.cpp \
                benchmarks/baseline_engine.hpp $(BUILD)/libultralob.a
	$(CXX) $(CXXFLAGS) -DULTRALOB_BUILD_FLAGS='"$(BUILD_FLAGS)"' \
	  benchmarks/compare_benchmarks.cpp benchmarks/allocation_tracker.cpp \
	  $(BUILD)/libultralob.a $(LDLIBS) -o $@

# --- WebAssembly (needs an activated emsdk on PATH) ------------------------
wasm:
	@command -v em++ >/dev/null 2>&1 || { \
	  echo "em++ not found. Install Emscripten (see docs/benchmarking.md#webassembly-build)"; exit 1; }
	@mkdir -p $(BUILD)/wasm web/src/wasm web/public/wasm
	em++ -std=c++20 -O3 -Iengine/include wasm/bindings.cpp $(ENGINE_SRC) \
	  -o $(BUILD)/wasm/ultralob.mjs \
	  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker -sALLOW_MEMORY_GROWTH=0 \
	  -sINITIAL_MEMORY=134217728 -sTOTAL_STACK=2097152 -sSTACK_OVERFLOW_CHECK=2 \
	  -sFILESYSTEM=0 -sASSERTIONS=0 --no-entry \
	  -sEXPORTED_FUNCTIONS=@wasm/exports.json \
	  -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString,HEAPU8,HEAPU32,HEAPU64,HEAPF64
	cp $(BUILD)/wasm/ultralob.mjs web/src/wasm/ultralob.mjs
	cp $(BUILD)/wasm/ultralob.wasm web/public/wasm/ultralob.wasm
	@echo "wasm artifacts written to web/src/wasm/ultralob.mjs and web/public/wasm/ultralob.wasm"

clean:
	rm -rf $(BUILD)

# Header dependency tracking: without this, changing a class layout in a header
# leaves stale objects compiled against the old layout (undefined behaviour at
# link time). -MMD -MP emits the .d files included below.
-include $(wildcard $(BUILD)/obj/*.d) $(wildcard $(BUILD)/obj/tests/*.d) \
         $(wildcard $(BUILD)/obj/wasm/*.d)
