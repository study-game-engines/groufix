/**
 * This file is part of groufix.
 * Copyright (c) Stef Velzel. All rights reserved.
 *
 * groufix : graphics engine produced by Stef Velzel.
 * www     : <www.vuzzel.nl>
 */

#include "groufix/core.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>


#define _GFX_GET_DEVICE_PROC_ADDR(pName) \
	context->vk.pName = (PFN_vk##pName)_groufix.vk.GetDeviceProcAddr( \
		context->vk.device, "vk"#pName); \
	if (context->vk.pName == NULL) { \
		gfx_log_error("Could not load vk"#pName"."); \
		goto clean; \
	}

#define _GFX_GET_DEVICE_TYPE(vType) \
	((vType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? \
		GFX_DEVICE_DISCRETE_GPU : \
	(vType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) ? \
		GFX_DEVICE_VIRTUAL_GPU : \
	(vType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? \
		GFX_DEVICE_INTEGRATED_GPU : \
	(vType == VK_PHYSICAL_DEVICE_TYPE_CPU) ? \
		GFX_DEVICE_CPU : \
		GFX_DEVICE_UNKNOWN)


/****************************
 * Array of Vulkan queue priority values in [0,1].
 * TODO: For now just a singular 1, changes when more queues get created...
 */
static const float _gfx_vk_queue_priorities[] = { 1.0f };


/****************************
 * Allocates a new queue set.
 * @param context Append created set to its _GFXQueueSet list.
 * @param count   Number of mutexes to create.
 * @return Non-zero on success.
 */
static int _gfx_alloc_queue_set(_GFXContext* context, uint32_t family,
                                VkQueueFlags flags, int present, size_t count)
{
	// Allocate a new queue set.
	_GFXQueueSet* s = malloc(sizeof(_GFXQueueSet) + sizeof(_GFXMutex) * count);
	if (s == NULL) return 0;

	s->family  = family;
	s->flags   = flags;
	s->present = present;

	// Keep inserting a mutex for each queue and stop as soon as we fail.
	for (s->count = 0; s->count < count; ++s->count)
		if (!_gfx_mutex_init(&s->locks[s->count]))
		{
			while (s->count > 0) _gfx_mutex_clear(&s->locks[--s->count]);
			free(s);

			return 0;
		}

	// Insert into list.
	gfx_list_insert_after(&context->sets, &s->list, NULL);

	return 1;
}

/****************************
 * Creates an array of VkDeviceQueueCreateInfo structures and fills the
 * _GFXQueueSet list of context, on failure, no list elements are freed!
 * @param createInfos Output create info, must call free() on success.
 * @return Number of created queue sets.
 *
 * Output describe the queue families desired by the groufix implementation.
 */
static size_t _gfx_get_queue_sets(_GFXContext* context, VkPhysicalDevice device,
                                  VkDeviceQueueCreateInfo** createInfos)
{
	assert(context != NULL);
	assert(createInfos != NULL);
	assert(*createInfos == NULL);

	// The following properties need to be supported by at least one queue:
	// 1) A general graphics family.
	// 2) A family that supports presentation to surfaces.
	// TODO: 3) A compute-only family for use when others are stalling.
	// Keep track of the family indices for all properties,
	// UINT32_MAX means we haven't found a family yet.
	uint32_t graphics = UINT32_MAX;
	uint32_t present = UINT32_MAX;
	int graphicsHasPresent = 0;

	// So get all queue families, do the searching...
	uint32_t count;
	_groufix.vk.GetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);

	VkQueueFamilyProperties props[count];
	_groufix.vk.GetPhysicalDeviceQueueFamilyProperties(device, &count, props);

	// 1) A general graphics family:
	// We use the family with VK_QUEUE_GRAPHICS_BIT set and
	// as few other bits set as possible.
	// 2) A family that supports presentation to surfaces:
	// Having presentation support has precedence over fewer flags.
	// So if we find a graphics family with presentation support, gogogo!
	// Note we do not check for presentation to a specific surface yet.
	for (uint32_t i = 0; i < count; ++i)
		if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			int pres = glfwGetPhysicalDevicePresentationSupport(
				_groufix.vk.instance, device, i);

			int better = (graphics == UINT32_MAX) ||
				(!graphicsHasPresent && pres) ||
				(props[i].queueFlags < props[graphics].queueFlags &&
				(!graphicsHasPresent || pres));

			if (!better) continue;

			// Pick this family as graphics family.
			graphicsHasPresent = pres;
			graphics = i;

			// Also pick it as presentation family.
			if (present == UINT32_MAX && pres)
				present = i;
		}

	// Check if we found a graphics family.
	if (graphics == UINT32_MAX)
	{
		gfx_log_error("Could not find a queue family with VK_QUEUE_GRAPHICS_BIT set.");
		return 0;
	}

	// 2) A family that supports presentation to surfaces:
	// If no graphics family supports presentation, find another family.
	// Again we prefer fewer bits.
	if (present == UINT32_MAX)
		for (uint32_t i = 0; i < count; ++i)
			if (glfwGetPhysicalDevicePresentationSupport(
				_groufix.vk.instance, device, i))
			{
				int better = (present == UINT32_MAX) ||
					props[i].queueFlags < props[present].queueFlags;

				// Pick this family as novel presentation family.
				if (better) present = i;
			}

	// Check if we found a presentation family.
	if (present == UINT32_MAX)
	{
		gfx_log_error("Could not find a queue family with presentation support.");
		return 0;
	}

	// Ok so we gathered all information at this point.
	// Allocate the queue sets and info structures.
	// Here we decide how many families to create:
	// - graphics queue does not support presentation? Add a family.
	// TODO: - no separate compute queue? Subtract a family.
	size_t num = graphicsHasPresent ? 1 : 2;
	*createInfos = malloc(sizeof(VkDeviceQueueCreateInfo) * num);

	if (*createInfos == NULL)
		goto clean;

	// Just initialize all info structures to some defaults.
	// Default is to create 1 queue of each family.
	for (uint32_t i = 0; i < num; ++i)
		(*createInfos)[i] = (VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,

			.pNext            = NULL,
			.flags            = 0,
			.queueCount       = 1,
			.pQueuePriorities = _gfx_vk_queue_priorities
		};

	// Allocate graphics queue.
	(*createInfos)[0].queueFamilyIndex = graphics;
	int success = _gfx_alloc_queue_set(
		context, graphics, props[graphics].queueFlags, graphicsHasPresent, 1);

	// Allocate novel present queue if necessary.
	if (!graphicsHasPresent)
	{
		(*createInfos)[1].queueFamilyIndex = present;
		success = success && _gfx_alloc_queue_set(
			context, present, props[present].queueFlags, 1, 1);
	}

	if (!success)
		goto clean;

	return num;


	// Cleanup on failure.
clean:
	free(*createInfos);
	*createInfos = NULL;

	return 0;
}

/****************************
 * Destroys a context and all its resources.
 * @param context Cannot be NULL.
 */
static void _gfx_destroy_context(_GFXContext* context)
{
	assert(context != NULL);

	// Erase itself from the context list.
	gfx_list_erase(&_groufix.contexts, &context->list);

	// Loop over all its queue sets and free their resources.
	while (context->sets.head != NULL)
	{
		_GFXQueueSet* set = (_GFXQueueSet*)context->sets.head;
		gfx_list_erase(&context->sets, context->sets.head);

		for (size_t q = 0; q < set->count; ++q)
			_gfx_mutex_clear(&set->locks[q]);

		free(set);
	}

	// We wait for all queues of the device to complete, then we can destroy.
	// We check if the functions were loaded properly,
	// they may not be if something failed during context creation.
	if (context->vk.DeviceWaitIdle != NULL)
		context->vk.DeviceWaitIdle(context->vk.device);
	if (context->vk.DestroyDevice != NULL)
		context->vk.DestroyDevice(context->vk.device, NULL);

	gfx_list_clear(&context->sets);
	free(context);
}

/****************************
 * Creates an appropriate context (Vulkan device + fp's) suited for a device.
 * device->context must be NULL, no prior context can be assigned.
 * @param device Cannot be NULL.
 *
 * Not thread-safe for the same device, it modifies.
 * device->context will remain NULL on failure, on success it will be set to
 * the newly created context (context->index will be set also).
 */
static void _gfx_create_context(_GFXDevice* device)
{
	assert(_groufix.vk.instance != NULL);
	assert(device != NULL);
	assert(device->context == NULL);

	_GFXContext* context = NULL;
	VkDeviceQueueCreateInfo* createInfos = NULL;

	// First of all, check Vulkan version.
	if (device->api < _GFX_VK_VERSION)
	{
		gfx_log_error("Physical device does not support Vulkan version %u.%u.%u: %s.",
			(unsigned int)VK_VERSION_MAJOR(_GFX_VK_VERSION),
			(unsigned int)VK_VERSION_MINOR(_GFX_VK_VERSION),
			(unsigned int)VK_VERSION_PATCH(_GFX_VK_VERSION),
			device->base.name);

		goto error;
	}

	// So first of all we find a device group which this device is part of.
	// We take the first device group we find, this assumes a device is never
	// seen in multiple groups...
	// Then we create a logical Vulkan device for this entire group.
	// Later on, any other device in the group will also use this context.
	uint32_t count;
	_GFX_VK_CHECK(_groufix.vk.EnumeratePhysicalDeviceGroups(
		_groufix.vk.instance, &count, NULL), goto error);

	if (count == 0)
		goto error;

	// We use a scope here so the goto above is allowed.
	{
		// Enumerate all device groups.
		VkPhysicalDeviceGroupProperties groups[count];

		_GFX_VK_CHECK(_groufix.vk.EnumeratePhysicalDeviceGroups(
			_groufix.vk.instance, &count, groups), goto error);

		// Loop over all groups and see if one contains this device.
		// We keep track of the index of the group and the device in it.
		size_t g = 0;
		size_t i = 0;

		for (; g < count; ++g)
		{
			for (i = 0; i < groups[g].physicalDeviceCount; ++i)
				if (groups[g].physicalDevices[i] == device->vk.device)
					break;

			if (i < groups[g].physicalDeviceCount)
				break;
		}

		if (g >= count)
		{
			// Probably want to know when a device is somehow invalid...
			gfx_log_error(
				"Physical device could not be found in any device group: %s.",
				device->base.name);

			goto error;
		}

		// Ok so we found a group, now go create a context.
		// Allocate array of physical devices in the group at the end,
		// this is used to check if a future device can use this context.
		context = malloc(
			sizeof(_GFXContext) +
			sizeof(VkPhysicalDevice) * groups[g].physicalDeviceCount);

		if (context == NULL)
			goto error;

		// Set these to NULL so we don't accidentally call garbage on cleanup.
		context->vk.DestroyDevice = NULL;
		context->vk.DeviceWaitIdle = NULL;

		gfx_list_insert_after(&_groufix.contexts, &context->list, NULL);
		gfx_list_init(&context->sets);
		context->numDevices = groups[g].physicalDeviceCount;

		memcpy(
			context->devices,
			groups[g].physicalDevices,
			sizeof(VkPhysicalDevice) * context->numDevices);

		// Call the thing that gets us the desired queues to create.
		// createInfos is explicitly freed on cleanup or success.
		// When a future device also uses this context,
		// it is assumed it has equivalent queue family properties.
		// If there are any device groups such that this is the case, you
		// probably have equivalent GPUs in an SLI/CrossFire setup anyway...
		size_t sets = _gfx_get_queue_sets(context, device->vk.device, &createInfos);
		if (!sets) goto clean;

		// Pick device features to enable (i.e. disable stuff we dont' want).
		// Again when devices use the same context, we assume they have
		// equivalent features.
		VkPhysicalDeviceFeatures pdf;
		_groufix.vk.GetPhysicalDeviceFeatures(device->vk.device, &pdf);

		// For features we do want, warn if not present.
		if (pdf.geometryShader == VK_FALSE) gfx_log_warn(
			"Physical device does not support geometry shaders: %s.",
			device->base.name);

		if (pdf.tessellationShader == VK_FALSE) gfx_log_warn(
			"Physical device does not support tessellation shaders: %s.",
			device->base.name);

		pdf.robustBufferAccess                      = VK_FALSE;
		pdf.fullDrawIndexUint32                     = VK_FALSE;
		pdf.imageCubeArray                          = VK_FALSE;
		pdf.independentBlend                        = VK_FALSE;
		pdf.sampleRateShading                       = VK_FALSE;
		pdf.dualSrcBlend                            = VK_FALSE;
		pdf.logicOp                                 = VK_FALSE;
		pdf.multiDrawIndirect                       = VK_FALSE;
		pdf.drawIndirectFirstInstance               = VK_FALSE;
		pdf.depthClamp                              = VK_FALSE;
		pdf.depthBiasClamp                          = VK_FALSE;
		pdf.fillModeNonSolid                        = VK_FALSE;
		pdf.depthBounds                             = VK_FALSE;
		pdf.wideLines                               = VK_FALSE;
		pdf.largePoints                             = VK_FALSE;
		pdf.alphaToOne                              = VK_FALSE;
		pdf.multiViewport                           = VK_FALSE;
		pdf.samplerAnisotropy                       = VK_FALSE;
		pdf.textureCompressionETC2                  = VK_FALSE;
		pdf.textureCompressionASTC_LDR              = VK_FALSE;
		pdf.textureCompressionBC                    = VK_FALSE;
		pdf.occlusionQueryPrecise                   = VK_FALSE;
		pdf.pipelineStatisticsQuery                 = VK_FALSE;
		pdf.vertexPipelineStoresAndAtomics          = VK_FALSE;
		pdf.fragmentStoresAndAtomics                = VK_FALSE;
		pdf.shaderTessellationAndGeometryPointSize  = VK_FALSE;
		pdf.shaderImageGatherExtended               = VK_FALSE;
		pdf.shaderStorageImageExtendedFormats       = VK_FALSE;
		pdf.shaderStorageImageMultisample           = VK_FALSE;
		pdf.shaderStorageImageReadWithoutFormat     = VK_FALSE;
		pdf.shaderStorageImageWriteWithoutFormat    = VK_FALSE;
		pdf.shaderUniformBufferArrayDynamicIndexing = VK_FALSE;
		pdf.shaderSampledImageArrayDynamicIndexing  = VK_FALSE;
		pdf.shaderStorageBufferArrayDynamicIndexing = VK_FALSE;
		pdf.shaderStorageImageArrayDynamicIndexing  = VK_FALSE;
		pdf.shaderClipDistance                      = VK_FALSE;
		pdf.shaderCullDistance                      = VK_FALSE;
		pdf.shaderFloat64                           = VK_FALSE;
		pdf.shaderInt64                             = VK_FALSE;
		pdf.shaderInt16                             = VK_FALSE;
		pdf.shaderResourceResidency                 = VK_FALSE;
		pdf.shaderResourceMinLod                    = VK_FALSE;
		pdf.sparseBinding                           = VK_FALSE;
		pdf.sparseResidencyBuffer                   = VK_FALSE;
		pdf.sparseResidencyImage2D                  = VK_FALSE;
		pdf.sparseResidencyImage3D                  = VK_FALSE;
		pdf.sparseResidency2Samples                 = VK_FALSE;
		pdf.sparseResidency4Samples                 = VK_FALSE;
		pdf.sparseResidency8Samples                 = VK_FALSE;
		pdf.sparseResidency16Samples                = VK_FALSE;
		pdf.sparseResidencyAliased                  = VK_FALSE;
		pdf.variableMultisampleRate                 = VK_FALSE;
		pdf.inheritedQueries                        = VK_FALSE;

		// Finally go create the logical Vulkan device.
		// Enable VK_KHR_swapchain so we can interact with surfaces from GLFW.
		// Enable VK_LAYER_KHRONOS_validation if debug,
		// this is deprecated by now, but for older Vulkan versions.
		const char* extensions[] = { "VK_KHR_swapchain" };
#if !defined (NDEBUG)
		const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
#endif

		VkDeviceGroupDeviceCreateInfo dgdci = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,

			.pNext               = NULL,
			.physicalDeviceCount = (uint32_t)context->numDevices,
			.pPhysicalDevices    = context->devices
		};

		VkDeviceCreateInfo dci = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,

			.pNext                   = &dgdci,
			.flags                   = 0,
			.queueCreateInfoCount    = (uint32_t)sets,
			.pQueueCreateInfos       = createInfos,
#if defined (NDEBUG)
			.enabledLayerCount       = 0,
			.ppEnabledLayerNames     = NULL,
#else
			.enabledLayerCount       = sizeof(layers)/sizeof(char*),
			.ppEnabledLayerNames     = layers,
#endif
			.enabledExtensionCount   = sizeof(extensions)/sizeof(char*),
			.ppEnabledExtensionNames = extensions,
			.pEnabledFeatures        = &pdf
		};

		_GFX_VK_CHECK(_groufix.vk.CreateDevice(
			device->vk.device, &dci, NULL, &context->vk.device), goto clean);

#if !defined (NDEBUG)
		// This is like a moment to celebrate, right?
		// We count the number of actual queues here.
		uint32_t queueCount = 0;
		for (GFXListNode* k = context->sets.head; k != NULL; k = k->next)
			queueCount += ((_GFXQueueSet*)k)->count;

		gfx_log_debug(
			"Logical Vulkan device of version %u.%u.%u created:\n"
			"    Contains at least: %s.\n"
			"    #physical devices: %u.\n"
			"    #queues: %u.\n",
			(unsigned int)VK_VERSION_MAJOR(device->api),
			(unsigned int)VK_VERSION_MINOR(device->api),
			(unsigned int)VK_VERSION_PATCH(device->api),
			device->base.name,
			(unsigned int)context->numDevices,
			(unsigned int)queueCount);
#endif

		// Now load all device level Vulkan functions.
		// Load vkDestroyDevice first so we can clean properly.
		_GFX_GET_DEVICE_PROC_ADDR(DestroyDevice);
		_GFX_GET_DEVICE_PROC_ADDR(AcquireNextImageKHR);
		_GFX_GET_DEVICE_PROC_ADDR(AllocateCommandBuffers);
		_GFX_GET_DEVICE_PROC_ADDR(AllocateMemory);
		_GFX_GET_DEVICE_PROC_ADDR(BeginCommandBuffer);
		_GFX_GET_DEVICE_PROC_ADDR(CmdBeginRenderPass);
		_GFX_GET_DEVICE_PROC_ADDR(CmdBindPipeline);
		_GFX_GET_DEVICE_PROC_ADDR(CmdDraw);
		_GFX_GET_DEVICE_PROC_ADDR(CmdEndRenderPass);
		_GFX_GET_DEVICE_PROC_ADDR(CreateCommandPool);
		_GFX_GET_DEVICE_PROC_ADDR(CreateFence);
		_GFX_GET_DEVICE_PROC_ADDR(CreateFramebuffer);
		_GFX_GET_DEVICE_PROC_ADDR(CreateGraphicsPipelines);
		_GFX_GET_DEVICE_PROC_ADDR(CreateImageView);
		_GFX_GET_DEVICE_PROC_ADDR(CreatePipelineLayout);
		_GFX_GET_DEVICE_PROC_ADDR(CreateRenderPass);
		_GFX_GET_DEVICE_PROC_ADDR(CreateSemaphore);
		_GFX_GET_DEVICE_PROC_ADDR(CreateShaderModule);
		_GFX_GET_DEVICE_PROC_ADDR(CreateSwapchainKHR);
		_GFX_GET_DEVICE_PROC_ADDR(DestroyCommandPool);
		_GFX_GET_DEVICE_PROC_ADDR(DestroyFence);
		_GFX_GET_DEVICE_PROC_ADDR(DestroyFramebuffer);
		_GFX_GET_DEVICE_PROC_ADDR(DestroyImageView);
		_GFX_GET_DEVICE_PROC_ADDR(DestroyPipeline);
		_GFX_GET_DEVICE_PROC_ADDR(DestroyPipelineLayout);
		_GFX_GET_DEVICE_PROC_ADDR(DestroyRenderPass);
		_GFX_GET_DEVICE_PROC_ADDR(DestroySemaphore);
		_GFX_GET_DEVICE_PROC_ADDR(DestroyShaderModule);
		_GFX_GET_DEVICE_PROC_ADDR(DestroySwapchainKHR);
		_GFX_GET_DEVICE_PROC_ADDR(DeviceWaitIdle);
		_GFX_GET_DEVICE_PROC_ADDR(EndCommandBuffer);
		_GFX_GET_DEVICE_PROC_ADDR(FreeCommandBuffers);
		_GFX_GET_DEVICE_PROC_ADDR(FreeMemory);
		_GFX_GET_DEVICE_PROC_ADDR(GetDeviceQueue);
		_GFX_GET_DEVICE_PROC_ADDR(GetSwapchainImagesKHR);
		_GFX_GET_DEVICE_PROC_ADDR(QueuePresentKHR);
		_GFX_GET_DEVICE_PROC_ADDR(QueueSubmit);
		_GFX_GET_DEVICE_PROC_ADDR(QueueWaitIdle);
		_GFX_GET_DEVICE_PROC_ADDR(ResetCommandPool);
		_GFX_GET_DEVICE_PROC_ADDR(ResetFences);
		_GFX_GET_DEVICE_PROC_ADDR(WaitForFences);

		// Set device's reference to this context.
		device->index = i;
		device->context = context;

		free(createInfos);

		return;
	}


	// Cleanup on failure.
clean:
	_gfx_destroy_context(context);
	free(createInfos);

error:
	gfx_log_error(
		"Could not create or initialize a logical Vulkan device for physical "
		"device group containing at least: %s.",
		device->base.name);
}

/****************************/
int _gfx_devices_init(void)
{
	assert(_groufix.vk.instance != NULL);
	assert(_groufix.devices.size == 0);

	// Reserve and create groufix devices.
	// The number or order of devices never changes after initialization,
	// nor is there a user pointer for callbacks, as there are no callbacks.
	// This means we do not have to dynamically allocate the devices.
	uint32_t count;
	_GFX_VK_CHECK(_groufix.vk.EnumeratePhysicalDevices(
		_groufix.vk.instance, &count, NULL), goto terminate);

	if (count == 0)
		goto terminate;

	// Again with the goto-proof scope.
	{
		// Enumerate all devices.
		VkPhysicalDevice devices[count];

		_GFX_VK_CHECK(_groufix.vk.EnumeratePhysicalDevices(
			_groufix.vk.instance, &count, devices), goto terminate);

		// Fill the array of groufix devices.
		// While doing so, keep track of the primary device,
		// this to make sure the primary device is at index 0.
		if (!gfx_vec_reserve(&_groufix.devices, (size_t)count))
			goto terminate;

		GFXDeviceType type = GFX_DEVICE_UNKNOWN;
		uint32_t ver = 0;

		for (uint32_t i = 0; i < count; ++i)
		{
			// Get some Vulkan properties and create new device.
			VkPhysicalDeviceProperties pdp;
			_groufix.vk.GetPhysicalDeviceProperties(devices[i], &pdp);

			_GFXDevice dev = {
				.base    = { .type = _GFX_GET_DEVICE_TYPE(pdp.deviceType) },
				.api     = pdp.apiVersion,
				.index   = 0,
				.context = NULL,
				.vk      = { .device = devices[i] }
			};

			// Init mutex and name string.
			if (!_gfx_mutex_init(&dev.lock))
				goto terminate;

			size_t len = strlen(pdp.deviceName);
			dev.base.name = malloc(sizeof(char*) * (len+1));

			if (dev.base.name == NULL)
			{
				_gfx_mutex_clear(&dev.lock);
				goto terminate;
			}

			strcpy((char*)dev.base.name, pdp.deviceName);
			((char*)dev.base.name)[len] = '\0';

			// Check if the new device is a better pick as primary.
			// If the type of device is superior, pick it as primary.
			// If the type is equal, pick the greater Vulkan version.
			// TODO: Select primary based on physical device features.
			int isPrim = (i == 0) ||
				dev.base.type < type ||
				(dev.base.type == type && pdp.apiVersion > ver);

			if (!isPrim)
				gfx_vec_push(&_groufix.devices, 1, &dev);
			else
			{
				// If new primary, insert it at index 0.
				gfx_vec_insert(&_groufix.devices, 1, &dev, 0);
				type = dev.base.type;
				ver = pdp.apiVersion;
			}
		}

		return 1;
	}


	// Cleanup on failure.
terminate:
	gfx_log_error("Could not find or initialize physical devices.");
	_gfx_devices_terminate();

	return 0;
}

/****************************/
void _gfx_devices_terminate(void)
{
	// Destroy all Vulkan contexts.
	while (_groufix.contexts.head != NULL)
		_gfx_destroy_context((_GFXContext*)_groufix.contexts.head);

	// And free all groufix devices, this only entails freeing the name string.
	// Devices are allocated in-place so no need to free anything else.
	for (size_t i = 0; i < _groufix.devices.size; ++i)
	{
		_GFXDevice* device = gfx_vec_at(&_groufix.devices, i);
		free((char*)device->base.name);
		_gfx_mutex_clear(&device->lock);
	}

	// Regular cleanup.
	gfx_vec_clear(&_groufix.devices);
	gfx_list_clear(&_groufix.contexts);
}

/****************************/
_GFXContext* _gfx_device_init_context(_GFXDevice* device)
{
	assert(device != NULL);

	// Lock the device's lock to sync access to the device's context.
	// Once this call returns successfully the context will not be modified anymore,
	// which means after this call, we can just read device->context directly.
	_gfx_mutex_lock(&device->lock);

	if (device->context == NULL)
	{
		// We only use the context lock here to sync the context array.
		// Other uses happen during initialization or termination,
		// any other operation must happen inbetween those two
		// function calls anyway so no need to lock in them.
		_gfx_mutex_lock(&_groufix.contextLock);

		// No context, go search for a compatible one.
		for (
			_GFXContext* context = (_GFXContext*)_groufix.contexts.head;
			context != NULL;
			context = (_GFXContext*)context->list.next)
		{
			for (size_t j = 0; j < context->numDevices; ++j)
				if (context->devices[j] == device->vk.device)
				{
					device->index = j;
					device->context = context;

					goto unlock;
				}
		}

		// If none found, create a new one.
		// It returns if it was successful, but just ignore it...
		_gfx_create_context(device);

	unlock:
		_gfx_mutex_unlock(&_groufix.contextLock);
	}

	// Read the result before unlock just in case it failed,
	// only when succeeded are we sure we don't write to it anymore.
	_GFXContext* ret = device->context;

	_gfx_mutex_unlock(&device->lock);

	return ret;
}

/****************************/
GFX_API size_t gfx_get_num_devices(void)
{
	assert(_groufix.initialized);

	return _groufix.devices.size;
}

/****************************/
GFX_API GFXDevice* gfx_get_device(size_t index)
{
	assert(_groufix.initialized);
	assert(_groufix.devices.size > 0);
	assert(index < _groufix.devices.size);

	return gfx_vec_at(&_groufix.devices, index);
}

/****************************/
GFX_API GFXDevice* gfx_get_primary_device(void)
{
	assert(_groufix.initialized);
	assert(_groufix.devices.size > 0);

	return gfx_vec_at(&_groufix.devices, 0);
}
