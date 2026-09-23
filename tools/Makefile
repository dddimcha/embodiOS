# Tools Makefile — host-side validation utilities.
#
# vktest: build and run the Vulkan compute shader validation on the host.
# The host needs libvulkan (runtime) and a software ICD (lavapipe); the test
# pins lavapipe via VK_ICD_FILENAMES unless the caller already chose an ICD.
# No vulkan-dev package is required: host_test_vulkan.c carries the minimal
# Vulkan declarations it needs and links against libvulkan.so.1 directly.
#
# venustest: build and run the Venus protocol-layer validation. Links the
# real kernel Venus device code (kernel/drivers/gpu/venus.c) against a mock
# virtio-gpu backend and a mock Venus renderer that executes the decoded
# command stream on lavapipe (structural decode + semantic replay).

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

VENUS_INC = -I../kernel/include -I../kernel/drivers/gpu
VENUS_SRCS = host_test_venus.c ../kernel/drivers/gpu/venus.c

host_test_vulkan: host_test_vulkan.c ../kernel/ai/vk_shaders.h
	$(CC) $(CFLAGS) $< -o $@ -l:libvulkan.so.1 -lm

host_test_venus: $(VENUS_SRCS) ../kernel/ai/vk_shaders.h \
		../kernel/drivers/gpu/venus_cs.h ../kernel/include/embodios/venus.h
	$(CC) $(CFLAGS) -std=gnu11 $(VENUS_INC) $(VENUS_SRCS) -o $@ \
		-l:libvulkan.so.1 -lm

# Regenerate the embedded shaders first (safe no-op when up to date).
../kernel/ai/vk_shaders.h: spirv_gen.py
	python3 spirv_gen.py

vktest: host_test_vulkan
	./host_test_vulkan

venustest: host_test_venus
	./host_test_venus

clean:
	rm -f host_test_vulkan host_test_venus

.PHONY: vktest venustest clean
