// SPDX-License-Identifier: MIT
/*
 * Minimal Vulkan ICD wrapper for the CIX Mali UMD.
 *
 * The proprietary libmali.so implements Vulkan but does not expose
 * VK_EXT_physical_device_drm. This wrapper forwards the Vulkan entry points
 * to libmali and adds the DRM physical-device properties that Mesa Zink uses
 * to associate a Vulkan device with the mali-kbase render node.
 *
 * The wrapper does not intercept GPU submission or buffer management. Those
 * continue to use the proprietary /dev/mali0 ABI in libmali.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <drm/drm.h>
#include <vulkan/vk_icd.h>

#define MALI_UMD_PATH "/opt/cixgpu-pro/lib/aarch64-linux-gnu/libmali.so"
#define WRAPPER_NAME "cix-mali-drm"

static void *umd_handle;
static PFN_vk_icdGetInstanceProcAddr umd_gipa;
static pthread_once_t node_once = PTHREAD_ONCE_INIT;
static struct {
	int found;
	int64_t major;
	int64_t minor;
} render_node;

#define MAX_INSTANCES 8
#define MAX_PHYSICAL_DEVICES 16
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	VkInstance instance;
	VkPhysicalDevice devices[MAX_PHYSICAL_DEVICES];
	uint32_t count;
} device_map[MAX_INSTANCES];

static void remember_devices(VkInstance instance, uint32_t count,
			     const VkPhysicalDevice *devices)
{
	uint32_t i, j;

	if (!devices)
		return;

	pthread_mutex_lock(&map_lock);
	for (i = 0; i < MAX_INSTANCES; i++) {
		if (device_map[i].instance != instance &&
		    device_map[i].instance != VK_NULL_HANDLE)
			continue;
		device_map[i].instance = instance;
		device_map[i].count = count < MAX_PHYSICAL_DEVICES ?
			count : MAX_PHYSICAL_DEVICES;
		for (j = 0; j < device_map[i].count; j++)
			device_map[i].devices[j] = devices[j];
		break;
	}
	pthread_mutex_unlock(&map_lock);
}

static VkInstance instance_for_device(VkPhysicalDevice device)
{
	VkInstance instance = VK_NULL_HANDLE;
	uint32_t i, j;

	pthread_mutex_lock(&map_lock);
	for (i = 0; i < MAX_INSTANCES; i++) {
		for (j = 0; j < device_map[i].count; j++) {
			if (device_map[i].devices[j] == device) {
				instance = device_map[i].instance;
				goto out;
			}
		}
	}
out:
	pthread_mutex_unlock(&map_lock);
	return instance;
}

static void forget_instance(VkInstance instance)
{
	uint32_t i;

	pthread_mutex_lock(&map_lock);
	for (i = 0; i < MAX_INSTANCES; i++) {
		if (device_map[i].instance == instance) {
			device_map[i].instance = VK_NULL_HANDLE;
			device_map[i].count = 0;
		}
	}
	pthread_mutex_unlock(&map_lock);
}

static void log_msg(const char *msg)
{
	if (getenv("CIX_VK_WRAPPER_DEBUG"))
		fprintf(stderr, "[%s] %s\n", WRAPPER_NAME, msg);
}

static int load_umd(void)
{
	if (umd_handle)
		return 0;

	umd_handle = dlopen(MALI_UMD_PATH, RTLD_NOW | RTLD_LOCAL);
	if (!umd_handle) {
		fprintf(stderr, "[%s] dlopen(%s): %s\n", WRAPPER_NAME,
			MALI_UMD_PATH, dlerror());
		return -1;
	}

	umd_gipa = (PFN_vk_icdGetInstanceProcAddr)dlsym(
		umd_handle, "vk_icdGetInstanceProcAddr");
	if (!umd_gipa) {
		fprintf(stderr, "[%s] libmali has no vk_icdGetInstanceProcAddr\n",
			WRAPPER_NAME);
		return -1;
	}

	log_msg("loaded the Mali UMD");
	return 0;
}

static PFN_vkVoidFunction umd_proc(VkInstance instance, const char *name)
{
	if (load_umd() != 0)
		return NULL;
	return umd_gipa(instance, name);
}

static void find_mali_render_node(void)
{
	DIR *dir;
	struct dirent *ent;

	dir = opendir("/dev/dri");
	if (!dir)
		return;

	while ((ent = readdir(dir)) != NULL) {
		char path[512];
		char name[64] = { 0 };
		struct drm_version version;
		struct stat st;
		int fd;

		if (strncmp(ent->d_name, "renderD", 7) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/dri/%s", ent->d_name);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		memset(&version, 0, sizeof(version));
		version.name = name;
		version.name_len = sizeof(name) - 1;
		if (ioctl(fd, DRM_IOCTL_VERSION, &version) == 0 &&
		    strcmp(name, "mali-kbase") == 0 &&
		    fstat(fd, &st) == 0) {
			render_node.found = 1;
			render_node.major = major(st.st_rdev);
			render_node.minor = minor(st.st_rdev);
			close(fd);
			break;
		}
		close(fd);
	}

	closedir(dir);
	if (render_node.found && getenv("CIX_VK_WRAPPER_DEBUG"))
		fprintf(stderr, "[%s] render node major=%lld minor=%lld\n",
			WRAPPER_NAME, (long long)render_node.major,
			(long long)render_node.minor);
}

static int have_render_node(void)
{
	pthread_once(&node_once, find_mali_render_node);
	return render_node.found;
}

static VkResult VKAPI_CALL
wrapper_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
		       const VkAllocationCallbacks *pAllocator,
		       VkInstance *pInstance)
{
	PFN_vkCreateInstance real_create;
	VkInstanceCreateInfo create_info;
	const char **extensions = NULL;
	uint32_t count = 0;
	VkResult result;

	real_create = (PFN_vkCreateInstance)umd_proc(NULL, "vkCreateInstance");
	if (!real_create)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (!pCreateInfo)
		return real_create(pCreateInfo, pAllocator, pInstance);

	create_info = *pCreateInfo;
	if (create_info.enabledExtensionCount) {
		uint32_t i;

		extensions = calloc(create_info.enabledExtensionCount,
				    sizeof(*extensions));
		if (!extensions)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		for (i = 0; i < create_info.enabledExtensionCount; i++) {
			const char *name = create_info.ppEnabledExtensionNames[i];

			/* This extension is provided by the wrapper, not the UMD. */
			if (strcmp(name,
				   VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0)
				continue;
			extensions[count++] = name;
		}
		create_info.enabledExtensionCount = count;
		create_info.ppEnabledExtensionNames = extensions;
	}

	result = real_create(&create_info, pAllocator, pInstance);
	free(extensions);
	log_msg(result == VK_SUCCESS ? "vkCreateInstance forwarded" :
					      "vkCreateInstance failed in the UMD");
	return result;
}

static VkResult VKAPI_CALL
wrapper_EnumerateInstanceExtensionProperties(const char *pLayerName,
					     uint32_t *pPropertyCount,
					     VkExtensionProperties *pProperties)
{
	PFN_vkEnumerateInstanceExtensionProperties real_enum;
	VkResult result;

	log_msg("wrapper_EnumerateInstanceExtensionProperties called");
	real_enum = (PFN_vkEnumerateInstanceExtensionProperties)umd_proc(
		NULL, "vkEnumerateInstanceExtensionProperties");
	if (!real_enum)
		return VK_ERROR_INITIALIZATION_FAILED;

	{
		uint32_t capacity = pProperties ? *pPropertyCount : 0;
		uint32_t written;

		result = real_enum(pLayerName, pPropertyCount, pProperties);
		if (result < 0 || pLayerName || !have_render_node())
			return result;

		if (!pProperties) {
			(*pPropertyCount)++;
			return result;
		}

		written = *pPropertyCount;
		if (getenv("CIX_VK_WRAPPER_DEBUG"))
			fprintf(stderr, "[%s] extension enum result=%d capacity=%u written=%u\n",
				WRAPPER_NAME, result, capacity, written);
		if (capacity > written) {
			VkExtensionProperties *ext = &pProperties[written];

			memset(ext, 0, sizeof(*ext));
			strncpy(ext->extensionName,
				VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
				sizeof(ext->extensionName) - 1);
			ext->specVersion = VK_EXT_PHYSICAL_DEVICE_DRM_SPEC_VERSION;
			*pPropertyCount = written + 1;
		} else {
			return VK_INCOMPLETE;
		}
	}

	return result;
}

static void fill_drm_properties(VkPhysicalDeviceProperties2 *properties)
{
	VkBaseOutStructure *entry;

	if (!properties || !have_render_node())
		return;

	for (entry = (VkBaseOutStructure *)properties->pNext; entry;
	     entry = entry->pNext) {
		VkPhysicalDeviceDrmPropertiesEXT *drm;

		if (entry->sType !=
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT)
			continue;

		drm = (VkPhysicalDeviceDrmPropertiesEXT *)entry;
		drm->hasPrimary = VK_FALSE;
		drm->hasRender = VK_TRUE;
		drm->primaryMajor = 0;
		drm->primaryMinor = 0;
		drm->renderMajor = render_node.major;
		drm->renderMinor = render_node.minor;
	}
}

static VkResult VKAPI_CALL
wrapper_EnumeratePhysicalDevices(VkInstance instance,
				 uint32_t *pPhysicalDeviceCount,
				 VkPhysicalDevice *pPhysicalDevices)
{
	PFN_vkEnumeratePhysicalDevices real_enum;
	VkResult result;

	real_enum = (PFN_vkEnumeratePhysicalDevices)umd_proc(
		instance, "vkEnumeratePhysicalDevices");
	if (!real_enum)
		return VK_ERROR_INITIALIZATION_FAILED;

	result = real_enum(instance, pPhysicalDeviceCount, pPhysicalDevices);
	if (result >= 0 && pPhysicalDevices)
		remember_devices(instance, *pPhysicalDeviceCount,
				 pPhysicalDevices);
	return result;
}

static void VKAPI_CALL
wrapper_DestroyInstance(VkInstance instance,
			const VkAllocationCallbacks *pAllocator)
{
	PFN_vkDestroyInstance real_destroy;

	forget_instance(instance);
	real_destroy = (PFN_vkDestroyInstance)umd_proc(
		instance, "vkDestroyInstance");
	if (real_destroy)
		real_destroy(instance, pAllocator);
}

static void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
				     VkPhysicalDeviceProperties2 *properties)
{
	PFN_vkGetPhysicalDeviceProperties2 real_props;
	VkInstance instance = instance_for_device(physicalDevice);

	real_props = (PFN_vkGetPhysicalDeviceProperties2)umd_proc(
		instance, "vkGetPhysicalDeviceProperties2");
	if (!real_props)
		real_props = (PFN_vkGetPhysicalDeviceProperties2)umd_proc(
			instance, "vkGetPhysicalDeviceProperties2KHR");
	if (real_props)
		real_props(physicalDevice, properties);

	fill_drm_properties(properties);
}

static void VKAPI_CALL
wrapper_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice,
					VkPhysicalDeviceProperties2 *properties)
{
	wrapper_GetPhysicalDeviceProperties2(physicalDevice, properties);
}

static PFN_vkVoidFunction wrapper_proc(VkInstance instance, const char *name)
{
	if (!name)
		return NULL;

	if (strcmp(name, "vkCreateInstance") == 0)
		return (PFN_vkVoidFunction)wrapper_CreateInstance;
	if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0) {
		log_msg("GIPA vkEnumerateInstanceExtensionProperties");
		return (PFN_vkVoidFunction)
			wrapper_EnumerateInstanceExtensionProperties;
	}
	if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
		return (PFN_vkVoidFunction)wrapper_EnumeratePhysicalDevices;
	if (strcmp(name, "vkDestroyInstance") == 0)
		return (PFN_vkVoidFunction)wrapper_DestroyInstance;
	if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0)
		return (PFN_vkVoidFunction)wrapper_GetPhysicalDeviceProperties2;
	if (strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0)
		return (PFN_vkVoidFunction)wrapper_GetPhysicalDeviceProperties2KHR;

	return umd_proc(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	return wrapper_proc(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
	return wrapper_proc(instance, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion)
{
	PFN_vk_icdNegotiateLoaderICDInterfaceVersion real_negotiate;

	if (load_umd() != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	real_negotiate = (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)dlsym(
		umd_handle, "vk_icdNegotiateLoaderICDInterfaceVersion");
	if (!real_negotiate)
		return VK_ERROR_INITIALIZATION_FAILED;

	return real_negotiate(pVersion);
}
