# One repository, two renderers: laptop (desktop) and mobile.
#
# Pick the one you want to build:
#
#   make             # laptop: ./desktop/toon (X11; WAYLAND=1 for Wayland)
#   make TARGET=mobile # mobile: the mg collector self-test (no Vulkan, no Android)
#   make mobile-apk  # mobile: the Android APK via mobile/demo/build_apk.sh
#   make run | shot | check   # laptop demo helpers
#   make clean
#
# TARGET defaults to desktop. Command-line variable assignments (WAYLAND=1, ...)
# are forwarded to the sub-make automatically.

TARGET ?= desktop

.PHONY: all desktop mobile mobile-apk run shot check clean help

all: $(TARGET)

desktop:
	$(MAKE) -C desktop

mobile:
	$(MAKE) -C mobile

mobile-apk:
	$(MAKE) -C mobile/demo apk 2>/dev/null || (cd mobile/demo && sh build_apk.sh)

run shot check:
	$(MAKE) -C desktop $@

clean:
	$(MAKE) -C desktop clean
	$(MAKE) -C mobile clean

help:
	@echo "Targets:"
	@echo "  make                 build the laptop (desktop) demo: ./desktop/toon"
	@echo "  make WAYLAND=1       build the laptop demo for Wayland"
	@echo "  make TARGET=mobile   build the mobile mg self-test (mobile/mg_test)"
	@echo "  make mobile-apk      build the Android APK (needs ANDROID_HOME + NDK)"
	@echo "  make run | shot | check   laptop demo helpers"
	@echo "  make clean"
