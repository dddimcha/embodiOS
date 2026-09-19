# Tools Makefile — host-side validation utilities.
#
# vktest: build and run the Vulkan compute shader validation on the host.
# The host needs libvulkan (runtime) and a software ICD (lavapipe); the test
# pins lavapipe via VK_ICD_FILENAMES unless the caller already chose an ICD.
# No vulkan-dev package is required: host_test_vulkan.c carries the minimal
# Vulkan declarations it needs and links against libvulkan.so.1 directly.

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

host_test_vulkan: host_test_vulkan.c ../kernel/ai/vk_shaders.h
	$(CC) $(CFLAGS) $< -o $@ -l:libvulkan.so.1 -lm

# Regenerate the embedded shaders first (safe no-op when up to date).
../kernel/ai/vk_shaders.h: spirv_gen.py
	python3 spirv_gen.py

vktest: host_test_vulkan
	./host_test_vulkan

clean:
	rm -f host_test_vulkan

.PHONY: vktest clean
