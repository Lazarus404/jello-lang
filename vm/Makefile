CC ?= cc
AR ?= ar
BUILD_DIR ?= build
# Absolute, so `make -C vm` and build_vm.sh (which passes an absolute path)
# name the same targets; otherwise the .d files written by one never match
# the other's targets, and header edits rebuild nothing.
override BUILD_DIR := $(abspath $(BUILD_DIR))
COMPUTED_GOTO ?= 1
WERROR ?= 1
PREFIX ?= /usr/local

CPPFLAGS += -Isrc/include -I$(BUILD_DIR)/include
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
	-Wsign-conversion -Wstrict-prototypes -Wmissing-prototypes \
	-Wold-style-definition -Wno-unused-parameter

ifeq ($(WERROR),1)
CFLAGS += -Werror
endif
ifeq ($(COMPUTED_GOTO),1)
CPPFLAGS += -DJELLO_USE_COMPUTED_GOTO
endif

# The system and architecture built for, from the compiler rather than the host, so that a
# cross-compiler (CC="clang -arch x86_64", CC="clang --target=...") picks its own. A library
# (JDLL_LINK) reaches the VM through the table it is handed (jello/jdll.h), so nothing is left
# undefined for the program that loads it to supply.
TARGET := $(shell $(CC) -dumpmachine 2>/dev/null)
# The libraries each program and native library links; Windows links winpthreads statically, so
# that only system DLLs are imported.
PROGRAM_LIBS := -lpthread -ldl -lm
STD_LIBS := -lm
TLS_LIBS := -lpthread
COMMAND_SOURCE := std/command.c
ifneq (,$(findstring -apple-,$(TARGET)))
AR_FLAGS := rcs
JDLL_LINK := -bundle
else ifneq (,$(findstring -linux,$(TARGET)))
# glibc hides POSIX and GNU declarations (clock_gettime, pthread_getattr_np) under -std=c11.
CPPFLAGS += -D_GNU_SOURCE
# D: GNU ar's fixed dates and owners, so that the archive changes only when an object does.
AR_FLAGS := rcsD
JDLL_LINK := -shared
else ifneq (,$(findstring -windows-,$(TARGET)))
AR_FLAGS := rcs
JDLL_LINK := -shared
EXE := .exe
PROGRAM_LIBS := -static -lpthread -lws2_32 -lbcrypt
STD_LIBS := -static -lpthread -lbcrypt
TLS_LIBS := -static -lpthread -lws2_32 -lbcrypt -lcrypt32
COMMAND_SOURCE := std/command_windows.c
# Windows 10, as the manifest says, so that its APIs (GetCurrentThreadStackLimits) are declared.
CPPFLAGS += -D_WIN32_WINNT=0x0A00
# The poller's backend (see third_party/wepoll-1.5.8/README.jello), and the manifest each program
# carries; it asks for UTF-8 as the code page, so that narrow paths and arguments are UTF-8.
WEPOLL_OBJECT := $(BUILD_DIR)/third_party/wepoll-1.5.8/wepoll.o
MANIFEST_OBJECT := $(BUILD_DIR)/tools/manifest.o
WINDRES ?= llvm-windres
else
$(error unsupported target '$(TARGET)')
endif
JIT ?= 1

SOURCES := src/load/loader.c src/load/check.c src/load/op_table.c \
	src/interp/frame.c src/interp/dispatch.c src/interp/exec_loop.c \
	src/interp/ops_arith.c src/interp/ops_bytes.c src/interp/ops_dyn.c \
	src/interp/ops_struct.c src/interp/ops_global.c src/interp/ops_array.c src/interp/ops_list.c \
	src/interp/ops_enum.c src/interp/ops_exc.c src/interp/ops_obj.c src/value/value.c src/gc/gc.c \
	src/gc/object.c src/thread/thread.c src/jdll/loader.c src/jdll/marshal.c \
	src/jdll/call_native.c src/jit/ir.c src/proc/stack.c src/proc/process.c \
	src/proc/natives.c src/proc/net.c src/proc/poll.c src/proc/image.c src/gc/copy.c

ifeq ($(JIT),1)
CPPFLAGS += -DJELLO_ENABLE_JIT
SOURCES += src/jit/emit_common.c src/jit/frame.c src/jit/regalloc.c \
	src/jit/survive.c src/jit/module.c src/jit/reload.c
ifneq (,$(filter x86_64% amd64%,$(TARGET)))
SOURCES += src/jit/emit_x64.c src/jit/regalloc_x64.c
else ifneq (,$(filter aarch64% arm64%,$(TARGET)))
SOURCES += src/jit/emit_arm64.c src/jit/regalloc_arm64.c
else
$(error JIT is unsupported for target '$(TARGET)'; use JIT=0)
endif
else
SOURCES += src/jit/none.c
endif

OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/%.o) $(BUILD_DIR)/src/proc/switch.o $(WEPOLL_OBJECT)
DEPS := $(OBJECTS:.o=.d)
HOST_OBJECT := $(BUILD_DIR)/tools/host.o
RUN_OBJECT := $(BUILD_DIR)/tools/run.o
JLO_TOOL_OBJECTS := $(BUILD_DIR)/tools/jlo_tool.o $(BUILD_DIR)/tools/deps.o
JELLO_OBJECTS := $(BUILD_DIR)/tools/jello.o $(BUILD_DIR)/tools/boot.o
JELLOC_OBJECTS := $(BUILD_DIR)/tools/jelloc.o $(BUILD_DIR)/tools/jelloc_image.o
# The compiler module jelloc embeds; build_jello.sh builds jelloc from the
# compiler it built.
JELLOC_IMAGE ?= ../jelloc/build/jelloc1.jlo
override JELLOC_IMAGE := $(abspath $(JELLOC_IMAGE))

# std.jdll, the standard native library, with the vendored PCRE2 10.30 (see
# third_party/pcre2-10.30/README.jello). PCRE2 builds as upstream intends, so
# without this project's warning flags; -I keeps any installed pcre2.h out.
PCRE2_DIR := third_party/pcre2-10.30
PCRE2_SOURCES := $(addprefix $(PCRE2_DIR)/src/pcre2_,auto_possess.c chartables.c compile.c config.c \
	context.c convert.c dfa_match.c error.c find_bracket.c jit_compile.c maketables.c match.c \
	match_data.c newline.c ord2utf.c pattern_info.c serialize.c string_utils.c study.c \
	substitute.c substring.c tables.c ucd.c valid_utf.c xclass.c)
PCRE2_OBJECTS := $(PCRE2_SOURCES:%.c=$(BUILD_DIR)/%.o)
PCRE2_CPPFLAGS := -I$(PCRE2_DIR)/src -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC \
	-DSUPPORT_UNICODE -DHAVE_STDINT_H=1 -DHAVE_INTTYPES_H=1 -DHAVE_MEMMOVE=1 -DHAVE_STRERROR=1
# tls.jdll, TLS with the vendored Mbed TLS 3.5.1 (see third_party/mbedtls-3.5.1/README.jello),
# which only a program importing std.net.Tls loads, so no other pays to load it. Mbed TLS builds
# as upstream intends, with the options tls_config.h adds; -isystem keeps its headers out of
# this project's warnings.
MBEDTLS_DIR := third_party/mbedtls-3.5.1
MBEDTLS_CPPFLAGS := -isystem $(MBEDTLS_DIR)/include -Isrc/include \
	-DMBEDTLS_USER_CONFIG_FILE='<jello/internal/tls_config.h>'
MBEDTLS_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(wildcard $(MBEDTLS_DIR)/library/*.c))
TLS_OBJECTS := $(BUILD_DIR)/tls/tls.o $(BUILD_DIR)/tls/crypto.o $(MBEDTLS_OBJECTS)
STD_SOURCES := std/std.c std/regexp.c std/unicode.c std/io.c $(COMMAND_SOURCE) std/text.c std/float.c std/math.c std/object.c std/date.c std/random.c std/encode.c
STD_OBJECTS := $(STD_SOURCES:%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean install jelloc
all: $(BUILD_DIR)/libjello_vm.a $(BUILD_DIR)/jello$(EXE) $(BUILD_DIR)/jlo_tool$(EXE) \
	$(BUILD_DIR)/std.jdll $(BUILD_DIR)/tls.jdll

$(BUILD_DIR)/std.jdll: $(STD_OBJECTS) $(PCRE2_OBJECTS)
	$(CC) $(CFLAGS) $(JDLL_LINK) $^ $(LDFLAGS) $(STD_LIBS) -o $@

$(BUILD_DIR)/$(PCRE2_DIR)/%.o: $(PCRE2_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(PCRE2_CPPFLAGS) -O2 -fPIC -fvisibility=hidden -MMD -MP -c $< -o $@

$(BUILD_DIR)/tls.jdll: $(TLS_OBJECTS)
	$(CC) $(CFLAGS) $(JDLL_LINK) $^ $(LDFLAGS) $(TLS_LIBS) -o $@

# Mbed TLS reaches its user configuration from a system header, which -MMD leaves out, and the
# configuration changes the layout of the structures every TLS object shares.
$(TLS_OBJECTS): src/include/jello/internal/tls_config.h

$(BUILD_DIR)/$(MBEDTLS_DIR)/%.o: $(MBEDTLS_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(MBEDTLS_CPPFLAGS) -O2 -fPIC -fvisibility=hidden -MMD -MP -c $< -o $@

$(BUILD_DIR)/tls/%.o: tls/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(MBEDTLS_CPPFLAGS) $(CFLAGS) -fPIC -fvisibility=hidden -MMD -MP -c $< -o $@

$(BUILD_DIR)/std/%.o: std/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -I$(PCRE2_DIR)/src $(CFLAGS) -fPIC -fvisibility=hidden -MMD -MP -c $< -o $@

# Made afresh, since `ar r` replaces members by name and two objects share
# each of frame.o and loader.o; ZERO_AR_DATE gives Apple's ar fixed dates, so
# the archive changes only when an object does.
$(BUILD_DIR)/libjello_vm.a: $(OBJECTS)
	rm -f $@
	ZERO_AR_DATE=1 $(AR) $(AR_FLAGS) $@ $^

$(BUILD_DIR)/jello$(EXE): $(JELLO_OBJECTS) $(RUN_OBJECT) $(HOST_OBJECT) $(MANIFEST_OBJECT) $(BUILD_DIR)/libjello_vm.a
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) $(PROGRAM_LIBS) -o $@

$(BUILD_DIR)/jlo_tool$(EXE): $(JLO_TOOL_OBJECTS) $(HOST_OBJECT) $(MANIFEST_OBJECT) $(BUILD_DIR)/libjello_vm.a
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) $(PROGRAM_LIBS) -o $@

# jelloc, the compiler as a native program, is not part of `all`: its image
# comes from the compiler's build.
jelloc: $(BUILD_DIR)/jelloc$(EXE)
$(BUILD_DIR)/jelloc$(EXE): $(JELLOC_OBJECTS) $(RUN_OBJECT) $(HOST_OBJECT) $(MANIFEST_OBJECT) $(BUILD_DIR)/libjello_vm.a
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) $(PROGRAM_LIBS) -o $@

$(BUILD_DIR)/tools/jelloc_image.o: tools/jelloc_image.S $(JELLOC_IMAGE)
	@mkdir -p $(@D)
	$(CC) -DJELLOC_IMAGE='"$(JELLOC_IMAGE)"' -c $< -o $@

$(BUILD_DIR)/src/proc/switch.o: src/proc/switch.S
	@mkdir -p $(@D)
	$(CC) -c $< -o $@

ifneq (,$(WEPOLL_OBJECT))
# wepoll builds as upstream intends, without this project's warning flags.
$(WEPOLL_OBJECT): third_party/wepoll-1.5.8/wepoll.c
	@mkdir -p $(@D)
	$(CC) -O2 -MMD -MP -c $< -o $@

$(MANIFEST_OBJECT): tools/manifest.rc tools/jello.manifest
	@mkdir -p $(@D)
	$(WINDRES) --target=$(TARGET) --no-preprocess -I tools -i $< -o $@
endif

# The release's version, the one place it is written being the `version` of jelloc/jello.json,
# which jello --version and std.sys.Platform's version() report.
VERSION_HEADER := $(BUILD_DIR)/include/jello_version.h
$(VERSION_HEADER): ../jelloc/jello.json
	@mkdir -p $(@D)
	@version="$$(sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' $<)"; \
	  [ -n "$$version" ] || { echo "$<: no version" >&2; exit 1; }; \
	  printf '#define JELLO_VERSION "%s"\n' "$$version" > $@
$(BUILD_DIR)/tools/jello.o $(BUILD_DIR)/std/io.o: $(VERSION_HEADER)

# The VM and its tools define jello/jdll.h's functions; the libraries above call them through
# the table they are handed.
$(OBJECTS) $(HOST_OBJECT) $(RUN_OBJECT) $(JLO_TOOL_OBJECTS) $(JELLO_OBJECTS) $(JELLOC_OBJECTS): \
	CPPFLAGS += -DJELLO_JDLL_HOST

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# The libraries go in lib/, where an installed jello looks (../lib from bin/).
install: $(BUILD_DIR)/libjello_vm.a $(BUILD_DIR)/jello$(EXE) $(BUILD_DIR)/std.jdll $(BUILD_DIR)/tls.jdll
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/jello
	install -m 755 $(BUILD_DIR)/jello$(EXE) $(DESTDIR)$(PREFIX)/bin/
	install -m 644 $(BUILD_DIR)/libjello_vm.a $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(BUILD_DIR)/std.jdll $(BUILD_DIR)/tls.jdll $(DESTDIR)$(PREFIX)/lib/
	install -m 644 src/include/jello.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 src/include/jello/jdll.h $(DESTDIR)$(PREFIX)/include/jello/

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS) $(PCRE2_OBJECTS:.o=.d) $(TLS_OBJECTS:.o=.d) $(STD_OBJECTS:.o=.d) $(HOST_OBJECT:.o=.d) $(RUN_OBJECT:.o=.d) $(JLO_TOOL_OBJECTS:.o=.d) $(JELLO_OBJECTS:.o=.d) \
	$(JELLOC_OBJECTS:.o=.d)
