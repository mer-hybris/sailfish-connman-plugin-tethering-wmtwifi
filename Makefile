# -*- Mode: makefile-gmake -*-

.PHONY: all clean debug release install

#
# Required packages
#
# connman.pc adds -export-symbols-regex linker option which doesn't  work
# on all platforms. 
#

LDPKGS = libgsupplicant glib-2.0
PKGS = connman $(LDPKGS)

#
# Default target
#

all: debug release

#
# Library name (connman requires that plugin libraries don't have lib prefix)
#
PLUGIN = sailfish-connman-plugin-tethering-wmtwifi.so

#
# Sources
#
SRC = tetheringplugin.c

#
# Directories
#

SRC_DIR = src
BUILD_DIR = build
DEBUG_BUILD_DIR = $(BUILD_DIR)/debug
RELEASE_BUILD_DIR = $(BUILD_DIR)/release
LIBDIR ?= /usr/lib

#
# Tools and flags
#

CC = $(CROSS_COMPILE)gcc
LD = $(CC)
WARNINGS = -Wall
BASE_FLAGS = -fPIC -fvisibility=hidden
FULL_CFLAGS = $(BASE_FLAGS) $(CFLAGS) $(DEFINES) $(WARNINGS) -MMD -MP \
  $(shell pkg-config --cflags $(PKGS))
FULL_LDFLAGS = $(BASE_FLAGS) $(LDFLAGS) -shared \
  $(shell pkg-config --libs $(LDPKGS))
DEBUG_FLAGS = -g
RELEASE_FLAGS =

ifndef KEEP_SYMBOLS
KEEP_SYMBOLS = 0
endif

ifneq ($(KEEP_SYMBOLS),0)
RELEASE_FLAGS += -g
endif

DEBUG_LDFLAGS = $(FULL_LDFLAGS) $(DEBUG_FLAGS)
RELEASE_LDFLAGS = $(FULL_LDFLAGS) $(RELEASE_FLAGS)
DEBUG_CFLAGS = $(FULL_CFLAGS) $(DEBUG_FLAGS) -DDEBUG
RELEASE_CFLAGS = $(FULL_CFLAGS) $(RELEASE_FLAGS) -O2

#
# Files
#

DEBUG_OBJS = $(SRC:%.c=$(DEBUG_BUILD_DIR)/%.o)
RELEASE_OBJS = $(SRC:%.c=$(RELEASE_BUILD_DIR)/%.o)

#
# Dependencies
#

DEPS = $(RELEASE_OBJS:%.o=%.d)
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(DEPS)),)
-include $(DEPS)
endif
endif

$(DEBUG_OBJS) $(DEBUG_PLUGIN): | $(DEBUG_BUILD_DIR)
$(RELEASE_OBJS) $(RELEASE_PLUGIN): | $(RELEASE_BUILD_DIR)

#
# Rules
#

DEBUG_PLUGIN = $(DEBUG_BUILD_DIR)/$(PLUGIN)
RELEASE_PLUGIN = $(RELEASE_BUILD_DIR)/$(PLUGIN)

debug: $(DEBUG_PLUGIN)

release: $(RELEASE_PLUGIN)

clean:
	rm -f *~ rpm/*~ $(SRC_DIR)/*~
	rm -fr $(BUILD_DIR) RPMS installroot

$(DEBUG_BUILD_DIR):
	mkdir -p $@

$(RELEASE_BUILD_DIR):
	mkdir -p $@

$(DEBUG_BUILD_DIR)/%.o : $(SRC_DIR)/%.c
	$(CC) -c $(DEBUG_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(RELEASE_BUILD_DIR)/%.o : $(SRC_DIR)/%.c
	$(CC) -c $(RELEASE_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(DEBUG_PLUGIN): $(DEBUG_OBJS)
	$(LD) $(DEBUG_OBJS) $(DEBUG_LDFLAGS) -o $@

$(RELEASE_PLUGIN): $(RELEASE_OBJS)
	$(LD) $(RELEASE_OBJS) $(RELEASE_LDFLAGS) -o $@
ifeq ($(KEEP_SYMBOLS),0)
	strip $@
endif

#
# Install
#

INSTALL_PERM  = 755
INSTALL = install
INSTALL_DIRS = $(INSTALL) -d
INSTALL_FILES = $(INSTALL) -m $(INSTALL_PERM)
INSTALL_LIB_DIR = $(DESTDIR)/$(LIBDIR)/connman/plugins

install: $(INSTALL_LIB_DIR)
	$(INSTALL_FILES) $(RELEASE_PLUGIN) $(INSTALL_LIB_DIR)

$(INSTALL_LIB_DIR):
	$(INSTALL_DIRS) $@
