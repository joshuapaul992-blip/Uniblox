# Uniblox - cross-compile from Linux to 32-bit Windows (NT 3.51 and later).
#
#   make            build build/win32/uniblox.exe
#   make check      build, then verify the PE is loadable on NT 3.51
#   make clean
#
# To build with clang + lld instead of GCC:
#   make CC="clang --target=i686-w64-windows-gnu --sysroot=/usr/i686-w64-mingw32 -fuse-ld=lld"

CROSS   ?= i686-w64-mingw32-
CC      ?= $(CROSS)gcc
ifeq ($(origin CC),default)
CC      := $(CROSS)gcc
endif
PYTHON  ?= python3

# CPU baseline. Code must stay correct on a plain Pentium (no MMX/SSE);
# faster paths are chosen at runtime via CPUID.
CPU     ?= pentium

OUT     := build/win32
EXE     := $(OUT)/uniblox.exe

# Engine sources shared by every build (no OS calls).
CORE_SRCS := \
	src/core/ub_string.c \
	src/core/ub_math.c \
	src/core/ub_utf.c \
	src/core/ub_path.c \
	src/core/ub_log.c \
	src/core/ub_compress.c \
	src/audio/ub_mixer.c \
	src/audio/ub_wav.c \
	src/render/ub_palette.c \
	src/world/ub_world.c \
	src/world/ub_rbxl.c \
	src/world/ub_query.c \
	src/render/ub_texture.c \
	src/render/ub_mesh.c \
	src/render/ub_render.c \
	src/render/ub_render_sw.c \
	src/render/ub_render_gl.c

SRCS := \
	src/crt/ub_crt.c \
	$(CORE_SRCS) \
	src/app/app_demo.c \
	src/platform/win32/win32_main.c \
	src/platform/win32/win32_file.c \
	src/platform/win32/win32_audio.c \
	src/platform/win32/win32_display.c \
	src/platform/win32/win32_gl.c

OBJS := $(patsubst src/%.c,$(OUT)/obj/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Windows only guarantees 4-byte stack alignment at entry points and
# callbacks; tell the compiler so. GCC also must not turn the loops in
# ub_crt.c back into calls to memcpy/memset (infinite recursion).
ifneq ($(findstring clang,$(CC)),)
COMPILER_FLAGS := -mstackrealign
else
COMPILER_FLAGS := -mincoming-stack-boundary=2 -fno-tree-loop-distribute-patterns
endif

CFLAGS := -std=c99 -O2 -g0 -Werror -march=$(CPU) -mtune=pentiumpro \
	-ffreestanding -fno-builtin -fno-math-errno $(COMPILER_FLAGS) \
	-fno-stack-protector -fno-stack-check \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Wall -Wextra -Werror=implicit-function-declaration -Wno-unused-parameter -Wshadow -Wstrict-prototypes \
	-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
	-DWINVER=0x0400 -D_WIN32_WINNT=0x0400 \
	-Isrc -MMD -MP

# Mark the image as a Windows NT 3.10 GUI program. Modern defaults (6.0)
# make NT 3.51 and NT 4.0 refuse to run the file. DllCharacteristics bits
# (ASLR, DEP, Terminal Server) did not exist on NT 3.51, so keep them clear.
LDFLAGS := -nostdlib -nostartfiles -s \
	-Wl,-e,_ub_entry \
	-Wl,--subsystem,windows:3.10 \
	-Wl,--major-os-version,3 -Wl,--minor-os-version,10 \
	-Wl,--major-image-version,0 -Wl,--minor-image-version,1 \
	-Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--disable-tsaware \
	-Wl,--no-insert-timestamp

# libgcc supplies 64-bit division helpers (__udivdi3 etc.) and __chkstk.
# Taken from the mingw GCC install so the clang build can use it too.
LIBGCC ?= $(shell $(CROSS)gcc -print-libgcc-file-name)

LIBS := -lkernel32 -luser32 -lgdi32 -lwinmm $(LIBGCC)

# Host unit tests: the OS-independent code compiled natively for Linux with
# AddressSanitizer and UBSan, against a stub platform layer.
HOSTCC     ?= cc
TEST_BIN   := build/host/uniblox_tests
TEST_SRCS  := tests/test_main.c tests/test_render.c tests/test_crt.c tests/test_compress.c tests/test_world.c tests/stub_platform.c $(CORE_SRCS)
TEST_FLAGS := -std=c99 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Isrc \
	-fsanitize=address,undefined -fno-sanitize-recover=all

.PHONY: all check test clean

all: $(EXE)

$(EXE): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(OUT)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Instructions newer than the original Pentium (P6 cmov/fcomi/fcmov, MMX
# and SSE registers). The toolchain's own libgcc is built for Pentium Pro,
# so this also catches anything pulled in from there.
OBJDUMP ?= $(CROSS)objdump
P6_PATTERN := [[:space:]](cmov[a-z]*|fcmov[a-z]*|f(u)?comip?|sysenter|sysexit)[[:space:]]|%(x|y)?mm[0-9]

check: $(EXE)
	$(PYTHON) tools/checkpe.py --allow tools/nt351_imports.txt $(EXE)
	@if $(OBJDUMP) -d $(EXE) | grep -E '$(P6_PATTERN)' | head -5 | grep .; then \
		echo "  FAIL: instructions newer than the Pentium (see above)"; exit 1; \
	else echo "  OK: only Pentium (P5) instructions"; fi

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS) src/crt/ub_crt.c $(wildcard src/*/*.h src/*/*/*.h tests/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(TEST_FLAGS) -o $@ $(TEST_SRCS) -lm

clean:
	rm -rf build

-include $(DEPS)
