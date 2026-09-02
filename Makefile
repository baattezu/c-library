# ============================================================================
#  Build system for the C++ learning repo.
#
#  This is a plain GNU Make file on purpose: for a Java dev, Make is the closest
#  thing to "what Maven does, but you can read all of it."  Real projects use
#  CMake (see notes/02-compilation-and-linking.md for why), but you should see
#  the raw compiler invocations at least once, because *that* is what CMake
#  eventually runs.
#
#  USAGE
#    make run  F=01-fundamentals/09-pointers.cpp     # compile (-O0 -g) and run
#    make asan F=02-memory/09-dangling-and-ub.cpp   # + AddressSanitizer/UBSan
#    make opt  F=07-performance/01-cache-locality.cpp # -O2, for benchmarks
#    make asm  F=03-oop/02-virtual-and-vtable.cpp     # dump the assembly
#    make all                                  # build every .cpp in the repo
#    make list                                 # show every runnable file
#    make clean
# ============================================================================

CXX      := clang++
STD      := -std=c++23

# -Wall -Wextra: turn on the warnings that catch real bugs.
# -Wpedantic:    complain about non-standard extensions (portability).
# -Wshadow:      a local variable hiding an outer one is a classic C++ bug.
# -Wconversion:  silent narrowing (int -> char) is a classic C++ bug.
# -g:            emit debug info so lldb and sanitizer stack traces are readable.
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
DEBUG    := -O0 -g -fno-omit-frame-pointer

# -O2 for benchmarks. -march=native lets the compiler use every instruction your
# specific CPU supports (NEON on Apple Silicon, AVX2/AVX-512 on x86).
# Never ship -march=native binaries to machines you don't control.
OPT      := -O2 -g -march=native -fno-omit-frame-pointer

# ASan  = AddressSanitizer  : use-after-free, buffer overflow, leaks, double free.
# UBSan = UndefinedBehavior : signed overflow, bad shifts, null deref, misaligned.
# These make your program ~2x slower and ~3x fatter, and are worth every byte.
SAN      := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
            -fno-sanitize-recover=all

BUILD    := build

# Every .cpp in the repo except the standalone project dirs that own their build.
SRCS     := $(shell find . -name '*.cpp' -not -path './$(BUILD)/*' -not -path './.git/*' | sort)
BINS     := $(patsubst ./%.cpp,$(BUILD)/%,$(SRCS))

# ---------------------------------------------------------------------------
.PHONY: all run asan leaks opt asm clean list help
.DEFAULT_GOAL := help

help:
	@echo "make run  F=<file.cpp>   compile -O0 -g and run"
	@echo "make asan F=<file.cpp>   compile with Address+UB sanitizer and run"
	@echo "make opt  F=<file.cpp>   compile -O2 -march=native and run (benchmarks!)"
	@echo "make asm  F=<file.cpp>   emit annotated assembly to build/<name>.s"
	@echo "make all                 build everything"
	@echo "make list                list all runnable files"
	@echo "make clean"

# $(F) is the source file. We strip the .cpp and drop the binary under build/.
TARGET := $(BUILD)/$(basename $(F))

run:
	@test -n "$(F)" || (echo "usage: make run F=path/to/file.cpp" && exit 1)
	@mkdir -p $(dir $(TARGET))
	$(CXX) $(STD) $(WARN) $(DEBUG) $(F) -o $(TARGET)
	@echo "--- running $(F) ---"
	@$(TARGET)

# NOTE: LeakSanitizer (detect_leaks=1) is NOT supported on macOS/arm64 — setting
# it makes the program abort at startup. ASan still catches use-after-free,
# buffer overflows, double-free, and use-after-return, which is the bulk of it.
# For LEAKS on macOS use the built-in `leaks` tool:
#     MallocStackLogging=1 leaks --atExit -- ./build/path/to/binary
# On Linux, ASan detects leaks by default.
asan:
	@test -n "$(F)" || (echo "usage: make asan F=path/to/file.cpp" && exit 1)
	@mkdir -p $(dir $(TARGET))
	$(CXX) $(STD) $(WARN) $(SAN) $(F) -o $(TARGET)-asan
	@echo "--- running $(F) under ASan+UBSan ---"
	@ASAN_OPTIONS=detect_stack_use_after_return=1 $(TARGET)-asan

# Leak check via macOS's native tool (ASan's LeakSanitizer is unavailable here).
leaks:
	@test -n "$(F)" || (echo "usage: make leaks F=path/to/file.cpp" && exit 1)
	@mkdir -p $(dir $(TARGET))
	$(CXX) $(STD) $(WARN) $(DEBUG) $(F) -o $(TARGET)-leak
	@echo "--- checking $(F) for leaks ---"
	@MallocStackLogging=1 leaks --atExit -- $(TARGET)-leak 2>&1 | tail -30

opt:
	@test -n "$(F)" || (echo "usage: make opt F=path/to/file.cpp" && exit 1)
	@mkdir -p $(dir $(TARGET))
	$(CXX) $(STD) $(WARN) $(OPT) $(F) -o $(TARGET)-opt
	@echo "--- running $(F) at -O2 ---"
	@$(TARGET)-opt

# Assembly output. Read this when you want to know what something *costs*.
# -S      : stop after compiling, don't assemble
# -masm   : (x86 only) intel syntax; on arm64 you get arm64 asm which is very readable
# We also strip the cfi/debug noise so the output is human-scale.
asm:
	@test -n "$(F)" || (echo "usage: make asm F=path/to/file.cpp" && exit 1)
	@mkdir -p $(BUILD)
	$(CXX) $(STD) -O2 -march=native -S -fverbose-asm $(F) -o $(BUILD)/$(notdir $(basename $(F))).s
	@grep -v '^\s*\.cfi' $(BUILD)/$(notdir $(basename $(F))).s | grep -v '^\s*\.loc' > $(BUILD)/$(notdir $(basename $(F))).clean.s
	@echo "wrote $(BUILD)/$(notdir $(basename $(F))).clean.s  ($$(wc -l < $(BUILD)/$(notdir $(basename $(F))).clean.s) lines)"
	@echo "tip: also paste the source into https://godbolt.org for a colour-coded view"

all: $(BINS)
	@echo "built $(words $(BINS)) binaries into $(BUILD)/"

$(BUILD)/%: ./%.cpp
	@mkdir -p $(dir $@)
	@$(CXX) $(STD) $(WARN) $(DEBUG) $< -o $@ && echo "  OK  $<" || echo "  FAIL $<"

list:
	@find . -name '*.cpp' -not -path './$(BUILD)/*' | sort

clean:
	rm -rf $(BUILD)
