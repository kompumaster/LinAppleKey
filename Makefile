# LinAppleKey — user-space port of WinAppleKey
#
# Safe by default: this only compiles. Nothing is loaded, grabbed or
# installed.

CC      ?= cc
CFLAGS  ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=

WARN    := -Wall -Wextra -Wno-unused-parameter
STD     := -std=gnu11

SRCDIR  := LinAppleKey
BUILDDIR := build
TARGET  := $(BUILDDIR)/linapplekeyd
TEST_TARGET := $(BUILDDIR)/multi_keyboard_test
VERSION := $(shell sed -n 's/.*LINAPPLEKEY_VERSION "\([^"]*\)".*/\1/p' $(SRCDIR)/Driver.h)
DISTDIR := dist
RELEASE := $(DISTDIR)/linapplekey-$(VERSION)
RELEASE_TARBALL := $(RELEASE).tar.gz
RELEASE_ITEMS := Makefile install.sh uninstall.sh README.md README_RU.md \
	A1314.jpg linapplekey.conf.example linapplekeyd.service \
	$(SRCDIR) tests

SRCS := \
	$(SRCDIR)/Driver.c \
	$(SRCDIR)/A1314.c \
	$(SRCDIR)/A1644.c \
	$(SRCDIR)/Vhf.c \
	$(SRCDIR)/Power.c \
	$(SRCDIR)/Replay.c

OBJS := $(SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
TEST_OBJS := $(filter-out $(BUILDDIR)/Driver.o,$(OBJS)) $(BUILDDIR)/Driver_test.o

.PHONY: all clean check test release

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(BUILDDIR)/Driver_test.o: $(SRCDIR)/Driver.c $(SRCDIR)/Driver.h | $(BUILDDIR)
	$(CC) $(STD) $(WARN) $(CPPFLAGS) $(CFLAGS) -Dmain=linapplekey_main -c -o $@ $<

$(TEST_TARGET): tests/multi_keyboard_test.c $(TEST_OBJS) $(SRCDIR)/Driver.h | $(BUILDDIR)
	$(CC) $(STD) $(WARN) $(CPPFLAGS) $(CFLAGS) -I$(SRCDIR) $(LDFLAGS) -o $@ tests/multi_keyboard_test.c $(TEST_OBJS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/Driver.h | $(BUILDDIR)
	$(CC) $(STD) $(WARN) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $@

# Compile-only sanity check; does not run or install anything.
check: $(TARGET)
	@echo "OK: $(TARGET) built"

# Replay tests: no devices, no root, no grab.
test: $(TARGET) $(TEST_TARGET)
	BIN=$(TARGET) ./tests/run.sh
	$(TEST_TARGET)

release:
	@test -n "$(VERSION)" || { echo "cannot read version from $(SRCDIR)/Driver.h" >&2; exit 1; }
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) test
	rm -rf "$(RELEASE)"
	mkdir -p "$(RELEASE)"
	cp -a $(RELEASE_ITEMS) "$(RELEASE)/"
	tar -C "$(DISTDIR)" -czf "$(RELEASE_TARBALL)" "linapplekey-$(VERSION)"
	cd "$(DISTDIR)" && sha256sum "linapplekey-$(VERSION).tar.gz" > "linapplekey-$(VERSION).tar.gz.sha256"
	rm -rf "$(RELEASE)"
	@echo "release: $(RELEASE_TARBALL)"

clean:
	rm -rf $(BUILDDIR)
