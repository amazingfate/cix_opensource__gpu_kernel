CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra -fPIC
LDFLAGS ?= -shared
LDLIBS ?= -ldl -lpthread

TARGET := libvulkan_cix_mali_wrapper.so

all: $(TARGET)

$(TARGET): cix_mali_vulkan_wrapper.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) $(TARGET)

.PHONY: all clean
