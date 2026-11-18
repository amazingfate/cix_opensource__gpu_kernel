# CIX Mali Vulkan wrapper

This directory contains a proof-of-concept Vulkan ICD wrapper for the
proprietary CIX Mali UMD. The UMD cannot be modified, and it does not expose
`VK_EXT_physical_device_drm`, which Mesa Zink uses to associate a Vulkan
physical device with a DRM render node.

The wrapper is loaded by the standard Khronos Vulkan Loader as an ICD. It
forwards instance/device entry points to
`/opt/cixgpu-pro/lib/aarch64-linux-gnu/libmali.so`, adds the missing instance
extension and injects the render node major/minor into
`VkPhysicalDeviceDrmPropertiesEXT`.

It does **not** replace the UMD, change GPU submission or translate memory
management calls. Those continue to use `/dev/mali0` and the proprietary ABI.

## Build

```sh
make clean && make
```

The build requires `libvulkan-dev` and DRM UAPI headers.

## Test

Create an ICD JSON pointing at the built library:

```json
{
    "ICD": {
        "api_version": "1.4.305",
        "library_path": "/absolute/path/to/libvulkan_cix_mali_wrapper.so"
    },
    "file_format_version": "1.0.1"
}
```

Then run:

```sh
VK_ICD_FILENAMES=/path/to/cix_mali_wrapper_icd.json vulkaninfo --summary
VK_ICD_FILENAMES=/path/to/cix_mali_wrapper_icd.json eglinfo -B
```

Set `CIX_VK_WRAPPER_DEBUG=1` for wrapper diagnostics. The wrapper discovers
the render node by scanning `/dev/dri/renderD*` and issuing
`DRM_IOCTL_VERSION`; it does not assume a fixed render-node minor.

Current result on debian-o6: EGL device platforms that previously failed with
`ZINK: failed to choose pdev` now report
`zink Vulkan 1.4(Mali-G720-Immortalis (ARM_PROPRIETARY))`. Wayland buffer
sharing and end-to-end rendering still need separate validation.
