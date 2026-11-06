DEBUG = 0
HAVE_VULKAN = 1
LINK_STATIC_LIBCPLUSPLUS = 1
LTO ?=

CORE_DIR := .
TARGET_NAME := parallel_gx1

ifeq ($(ECHO),1)
   E := 1
endif

# Windows is recognised from the environment every Windows shell sets (all
# MSYS2 environments included, whatever uname -s reports there) or from a
# MinGW-w64 compiler, so a cross build needs only CC/CXX.
ifeq ($(platform),)
   platform = unix
   UNAME_S := $(shell uname -s 2>/dev/null)
   ifeq ($(OS),Windows_NT)
      platform = win
   else ifeq ($(UNAME_S),)
      platform = win
   else ifneq ($(findstring Darwin,$(UNAME_S)),)
      platform = osx
   else ifneq (,$(findstring mingw,$(shell $(CC) -dumpmachine 2>/dev/null)))
      platform = win
   endif
endif

# make's built-in default compiler is cc, which MinGW-w64 does not ship.
ifneq (,$(filter win win64,$(platform)))
   ifeq ($(origin CC),default)
      CC := gcc
   endif
   ifeq ($(origin CXX),default)
      CXX := g++
   endif
endif

# webOS SDK toolchains are plain Linux cross compilers; their name is what
# tells them apart.
ifneq (,$(findstring webos,$(CC)))
   ifneq (,$(findstring unix,$(platform)))
      override platform := webos
   endif
endif

# ----------------------------------------------------------------------------
# Target architecture: taken from the compiler (or the Apple -arch/-target
# selection), not the host, so cross builds pick the right JIT and SIMD code.
# ----------------------------------------------------------------------------
ifeq ($(platform), osx)
   ifeq ($(CROSS_COMPILE),1)
      arch := $(firstword $(subst -, ,$(LIBRETRO_APPLE_PLATFORM)))
   endif
   ifeq ($(arch),)
      arch := $(shell uname -m)
   endif
else ifneq (,$(filter ios-arm64 tvos-arm64 android-arm64,$(platform)))
   arch := arm64
else ifeq ($(platform), android-x86_64)
   arch := x86_64
else ifeq ($(arch),)
   arch := $(firstword $(subst -, ,$(shell $(CC) -dumpmachine 2>/dev/null)))
   ifeq ($(arch),)
      arch := $(shell uname -m)
   endif
endif

ifneq (,$(filter x86_64 amd64 x64,$(arch)))
   ARCH_X86_64 := 1
   arch := x86_64
else ifneq (,$(filter aarch64 arm64 arm,$(arch)))
   ARCH_ARM64 := 1
   arch := arm64
else
   $(error Unsupported target architecture '$(arch)': the core builds for x86_64 and arm64)
endif

HAVE_LIBUSB        := 0
HAVE_HIDAPI        := 0
HAVE_CPPIPC        := 0
HAVE_D3D           := 0
HAVE_BUNDLED_ICONV := 0

FLAGS    :=
CFLAGS   :=
CXXFLAGS :=
LDFLAGS  :=
LIBS     :=

# Unix (Linux) and webOS
ifneq (,$(filter unix webos,$(platform)))
   OS_UNIX := 1
   TARGET  := $(TARGET_NAME)_libretro.so
   fpic    := -fPIC
   SHARED  := -shared -Wl,--no-undefined -Wl,--version-script=link.T
   ifeq ($(platform), webos)
      FLAGS += -D__WEBOS__ -DSKIP_WIMOTESCANNER_THREAD -DSKIP_GCMEMCARD_THREAD \
	-DSKIP_SAVESTATE_THREAD -DSKIP_NETWORK_WORKER_THREAD -DSKIP_ASSETLOADER_THREAD \
	-DSKIP_WICONNECT24_WORKER_THREAD -DSKIP_RESOURCE_WORKER_THREAD \
	-DSKIP_KDSCHEDULER_TIMER_THREAD
      HAVE_VULKAN := 0
   else
      HAVE_LIBUSB := 1
      HAVE_HIDAPI := 1
   endif
   HAVE_CPPIPC := 1
   ifeq ($(LINK_STATIC_LIBCPLUSPLUS),1)
      LDFLAGS += -static-libgcc -static-libstdc++
   endif
   LIBS += -lpthread -ldl -lrt

# macOS
else ifeq ($(platform), osx)
   OS_APPLE := 1
   OS_MACOS := 1
   TARGET   := $(TARGET_NAME)_libretro.dylib
   fpic     := -fPIC
   SHARED   := -dynamiclib
   MACOS_MINVER ?= 11.0
   ifeq ($(CROSS_COMPILE),1)
      TARGET_RULE := -target $(arch)-apple-macos$(MACOS_MINVER) -isysroot $(LIBRETRO_APPLE_ISYSROOT)
   else
      TARGET_RULE := -arch $(arch) -mmacosx-version-min=$(MACOS_MINVER)
   endif
   FLAGS   += $(TARGET_RULE)
   LDFLAGS += $(TARGET_RULE) -Wl,-dead_strip
   HAVE_LIBUSB        := 1
   HAVE_HIDAPI        := 1
   HAVE_BUNDLED_ICONV := 1
   LIBS += -framework AppKit -framework CoreFoundation -framework Foundation \
	-framework IOKit -framework CoreServices -framework IOBluetooth \
	-framework Security -lobjc -lresolv

# iOS / tvOS
else ifneq (,$(filter ios-arm64 tvos-arm64,$(platform)))
   OS_APPLE := 1
   OS_IOS   := 1
   fpic     := -fPIC
   SHARED   := -dynamiclib
   ifeq ($(platform), tvos-arm64)
      TARGET := $(TARGET_NAME)_libretro_tvos.dylib
      MINVER ?= 14.0
      ifeq ($(IOSSDK),)
         IOSSDK := $(shell xcrun -sdk appletvos -show-sdk-path)
      endif
      MINVER_FLAG := -mappletvos-version-min=$(MINVER)
   else
      TARGET := $(TARGET_NAME)_libretro_ios.dylib
      IOS_MINVER ?= 14.0
      ifeq ($(IOSSDK),)
         IOSSDK := $(shell xcrun -sdk iphoneos -show-sdk-path)
      endif
      MINVER_FLAG := -miphoneos-version-min=$(IOS_MINVER)
   endif
   CC      = cc -arch arm64 -isysroot $(IOSSDK)
   CXX     = c++ -arch arm64 -isysroot $(IOSSDK)
   FLAGS   += $(MINVER_FLAG) -DIPHONEOS
   LDFLAGS += $(MINVER_FLAG)
   HAVE_BUNDLED_ICONV := 1
   LIBS += -framework CoreFoundation -framework Foundation -framework IOKit -lobjc -lresolv

# Android (NDK clang)
else ifneq (,$(filter android-arm64 android-x86_64,$(platform)))
   OS_ANDROID := 1
   TARGET     := $(TARGET_NAME)_libretro_android.so
   fpic       := -fPIC
   SHARED     := -shared -Wl,--no-undefined -Wl,--version-script=link.T
   ifeq ($(platform), android-arm64)
      ANDROID_API ?= 21
      ANDROID_TRIPLE := aarch64-linux-android$(ANDROID_API)
   else
      ANDROID_API ?= 24
      ANDROID_TRIPLE := x86_64-linux-android$(ANDROID_API)
   endif
   NDK_ROOT ?= $(ANDROID_NDK_ROOT)
   ANDROID_NDK_LLVM ?= $(NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64
   CC  = $(ANDROID_NDK_LLVM)/bin/clang --target=$(ANDROID_TRIPLE)
   CXX = $(ANDROID_NDK_LLVM)/bin/clang++ --target=$(ANDROID_TRIPLE)
   AR  = $(ANDROID_NDK_LLVM)/bin/llvm-ar
   FLAGS   += -DANDROID -DHAVE_EGL=1 -I$(CORE_DIR)/Source/Android -funwind-tables
   LDFLAGS += -static-libstdc++
   HAVE_BUNDLED_ICONV := 1
   LIBS += -lEGL -landroid -llog -latomic -lm -ldl

# Windows (MinGW-w64, native MSYS2 or cross)
else
   OS_WIN := 1
   TARGET := $(TARGET_NAME)_libretro.dll
   SHARED := -shared -Wl,--no-undefined -Wl,--version-script=link.T
   FLAGS  += -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
	-DWINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION=0x50000
   HAVE_LIBUSB      := 1
   HAVE_HIDAPI      := 1
   HAVE_CPPIPC      := 1
   HAVE_D3D         := 1
   LDFLAGS += -static-libgcc -static-libstdc++ -static
   LIBS += -lsetupapi -lcfgmgr32 -lshlwapi -liphlpapi -lwinmm -lws2_32 -lbcrypt \
	-ladvapi32 -lcrypt32 -lsecur32 -lole32 -loleaut32 -luuid -lshell32 \
	-luser32 -lgdi32 -lkernel32 -lwinpthread
endif

GEN_DIR := $(CORE_DIR)/obj/gen
OBJ_DIR := $(CORE_DIR)/obj/$(platform)-$(arch)

include Makefile.common

# ----------------------------------------------------------------------------
# Compiler flags
# ----------------------------------------------------------------------------
FLAGS += -D__LIBRETRO__ -D_ARCH_64=1 -D_DEFAULT_SOURCE -DHAS_OPENGL \
	-DDATA_DIR=\"/usr/local/share/dolphin-emu/\" -I$(SRC_DIR)
ifeq ($(ARCH_X86_64),1)
   FLAGS += -D_M_X86_64=1 -msse2
   ifeq ($(OS_MACOS),1)
      FLAGS += -mssse3 -march=core2
   endif
else
   FLAGS += -D_M_ARM_64=1 -march=armv8-a+crc
endif
ifneq (,$(filter 1,$(OS_UNIX) $(OS_APPLE)))
   FLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
endif
ifeq ($(OS_APPLE),1)
   FLAGS += -DFMT_EXCEPTIONS=0
endif
ifeq ($(HAVE_VULKAN),1)
   FLAGS += -DHAS_VULKAN
endif
ifeq ($(HAVE_LIBUSB),1)
   FLAGS += -D__LIBUSB__
endif

FLAGS += -fno-strict-aliasing -fno-exceptions -fomit-frame-pointer

# Everything the core does not export stays hidden; the libretro entry points
# carry visibility("default") through RETRO_API.
ifneq (,$(fpic))
   FLAGS += $(fpic) -fno-semantic-interposition
endif
FLAGS    += -fvisibility=hidden
CXXFLAGS += -fvisibility-inlines-hidden

ifeq ($(DEBUG), 1)
   FLAGS += -O0 -g -D_DEBUG
else
   FLAGS += -O3 -DNDEBUG
endif

ifneq ($(LTO),)
   FLAGS   += $(LTO)
   LDFLAGS += $(LTO)
endif

ifneq ($(SANITIZER),)
   FLAGS   += -fsanitize=$(SANITIZER)
   LDFLAGS += -fsanitize=$(SANITIZER)
endif

CFLAGS   := $(FLAGS) $(CFLAGS)
CXXFLAGS := $(FLAGS) $(CXXFLAGS)
LDFLAGS  += $(fpic) $(SHARED)

# ----------------------------------------------------------------------------
# Objects
# ----------------------------------------------------------------------------
# Objects live under obj/<platform>-<arch>, one tree per target, so the
# submodules stay clean and a second architecture never links the first
# one's objects.
obj_of = $(patsubst %,$(OBJ_DIR)/%.o,$(1))

define LIB_template
$(1)_OBJECTS := $$(call obj_of,$$($(1)_SOURCES_C) $$($(1)_SOURCES_CXX) $$($(1)_SOURCES_ASM) $$($(1)_SOURCES_OBJCXX))
$$($(1)_OBJECTS): GROUP := $(1)
OBJECTS += $$($(1)_OBJECTS)
endef

OBJECTS :=
$(foreach g,$(LIB_GROUPS),$(eval $(call LIB_template,$(g))))
DEPS := $(OBJECTS:.o=.d)

# The toolchain and configuration a tree was built with; a change wipes it,
# since nothing in an object's name says which compiler produced it.
BUILD_TAG := $(CC)|$(CXX)|$(platform)|$(arch)|$(DEBUG)|$(LTO)|$(SANITIZER)|$(HAVE_VULKAN)
BUILD_TAG_STATUS := $(shell \
	if [ "`cat $(OBJ_DIR)/.build-tag 2>/dev/null`" != "$(BUILD_TAG)" ]; then \
		rm -rf $(OBJ_DIR); mkdir -p $(OBJ_DIR); \
		echo "$(BUILD_TAG)" > $(OBJ_DIR)/.build-tag; \
	fi)

.DEFAULT_GOAL := all

all: $(TARGET)

-include $(DEPS)

$(OBJECTS): | $(GEN_HEADERS)

# The object list outgrows the 32 KiB Windows command line, so there the
# linker reads it from a response file ($(file) needs GNU make 4.0, which
# MSYS2 and the cross toolchains have; the Xcode make is 3.81).
ifeq ($(OS_WIN),1)
LINK_OBJECTS = @$(OBJ_DIR)/link-objects.rsp
else
LINK_OBJECTS = $(OBJECTS)
endif

$(TARGET): $(OBJECTS)
	@$(if $(E),echo LD $@,)
ifeq ($(OS_WIN),1)
	$(file >$(OBJ_DIR)/link-objects.rsp,$(OBJECTS))
endif
	$(CXX) -o $@ $(LINK_OBJECTS) $(LDFLAGS) $(LIBS)

$(OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	@$(if $(E),echo CC $<,)
	$(CC) $(CFLAGS) $($(GROUP)_FLAGS) $($(GROUP)_CFLAGS) $(FILE_FLAGS_$(abspath $<)) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	@$(if $(E),echo CXX $<,)
	$(CXX) $(CXXFLAGS) $($(GROUP)_FLAGS) $($(GROUP)_CXXFLAGS) $(FILE_FLAGS_$(abspath $<)) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.cc.o: %.cc
	@mkdir -p $(@D)
	@$(if $(E),echo CXX $<,)
	$(CXX) $(CXXFLAGS) $($(GROUP)_FLAGS) $($(GROUP)_CXXFLAGS) $(FILE_FLAGS_$(abspath $<)) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.mm.o: %.mm
	@mkdir -p $(@D)
	@$(if $(E),echo OBJCXX $<,)
	$(CXX) -x objective-c++ $(CXXFLAGS) $($(GROUP)_FLAGS) $($(GROUP)_CXXFLAGS) $(FILE_FLAGS_$(abspath $<)) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.S.o: %.S
	@mkdir -p $(@D)
	@$(if $(E),echo AS $<,)
	$(CC) $(CFLAGS) $($(GROUP)_FLAGS) $(FILE_FLAGS_$(abspath $<)) -MMD -MP -c $< -o $@

clean:
	rm -rf $(CORE_DIR)/obj
	rm -f $(TARGET)

.PHONY: all clean
