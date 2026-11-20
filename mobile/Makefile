# Builds the mobile `mg` API self-test and the pass-cache self-test.
#
#   make            # mg_test: collector + stable radix sort (no Vulkan, no device)
#   make pc_test    # pass_cache self-test (compiles against the tree's Vulkan headers)
#   make clean
#
# Neither needs the Android NDK. The Android demo itself lives in demo/ and is
# built with demo/build_apk.sh (or Gradle against demo/CMakeLists.txt).

CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Werror

all: mg_test pc_test

mg_test: mg.c mg.h
	$(CC) $(CFLAGS) -DMG_TEST -o mg_test mg.c

pc_test: pass_cache.c vk_mobile.h
	$(CC) $(CFLAGS) -DMU_PASS_CACHE_TEST -o pc_test pass_cache.c \
	    -I../desktop -I../desktop/external/vulkan/include

clean:
	rm -f mg_test pc_test

.PHONY: all clean
