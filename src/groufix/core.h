/**
 * This file is part of groufix.
 * Copyright (c) Stef Velzel. All rights reserved.
 *
 * groufix : graphics engine produced by Stef Velzel.
 * www     : <www.vuzzel.nl>
 */


#ifndef _GFX_CORE_H
#define _GFX_CORE_H

#include "groufix/containers/vec.h"
#include "groufix/core/log.h"
#include "groufix/core/window.h"
#include "groufix/core/threads.h"
#include <stdio.h>

#if !defined (__STDC_NO_ATOMICS__)
	#include <stdatomic.h>
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// Vulkan function pointer.
#define _GFX_PFN_VK(pName) PFN_vk##pName pName


/**
 * groufix global data, i.e. groufix state.
 */
typedef struct _GFXState
{
	int initialized;

	GFXVec devices;  // Stores GFXDevice (no user pointer, so not dynamic)
	GFXVec contexts; // Stores _GFXContext*
	GFXVec monitors; // Stores _GFXMonitor*

	// Monitor configuration change.
	void (*monitorEvent)(GFXMonitor*, int);


	// Thread local data access.
	struct
	{
#if defined (__STDC_NO_ATOMICS__)
		unsigned int  id;
		_GFXMutex     idLock;
#else
		atomic_uint   id;
#endif
		_GFXThreadKey key; // Stores _GFXThreadState*
		_GFXMutex     ioLock;

	} thread;


	// Vulkan fields.
	struct
	{
		VkInstance instance;

		_GFX_PFN_VK(CreateInstance);
		_GFX_PFN_VK(EnumerateInstanceVersion);

		_GFX_PFN_VK(CreateDevice);
		_GFX_PFN_VK(DestroyInstance);
		_GFX_PFN_VK(EnumeratePhysicalDeviceGroups);
		_GFX_PFN_VK(EnumeratePhysicalDevices);
		_GFX_PFN_VK(GetDeviceProcAddr);
		_GFX_PFN_VK(GetPhysicalDeviceProperties);

	} vk;

} _GFXState;


/**
 * Thread local data.
 */
typedef struct _GFXThreadState
{
	unsigned int id;

	// Logging data.
	struct
	{
		GFXLogLevel level;
		int         std;
		FILE*       file;

	} log;

} _GFXThreadState;


/**
 * Logical Vulkan context (superset of a device).
 */
typedef struct _GFXContext
{
	// Vulkan fields.
	struct
	{
		VkDevice device;

		_GFX_PFN_VK(DestroyDevice);

	} vk;

	// Associated physical device group.
	size_t           numDevices;
	VkPhysicalDevice devices[];

} _GFXContext;


/****************************
 * User visible objects.
 ****************************/

/**
 * Physical device definition (opaque public definition).
 */
typedef struct GFXDevice
{
	GFXDeviceType type;
	size_t        index; // Index into the device group.
	_GFXContext*  context;

	// Vulkan fields.
	struct
	{
		VkPhysicalDevice device;

	} vk;

} GFXDevice;


/**
 * Internal logical monitor definition.
 */
typedef struct _GFXMonitor
{
	GFXMonitor   base;
	GLFWmonitor* handle;

} _GFXMonitor;


/**
 * Internal logical window definition.
 */
typedef struct _GFXWindow
{
	GFXWindow   base;
	GLFWwindow* handle;

	// Vulkan fields.
	struct
	{
		VkSurfaceKHR surface;

	} vk;

} _GFXWindow;


/****************************
 * Global and local state.
 ****************************/

/**
 * The only instance of global groufix data.
 */
extern _GFXState _groufix;


/**
 * Initializes global groufix state.
 * _groufix.initialized must be 0, on success it will be set to 1.
 * @return Non-zero on success.
 */
int _gfx_state_init(void);

/**
 * Terminates global groufix state.
 * _groufix.initialized must be 1, after this call it will be set to 0.
 * Must be called by the same thread that called _gfx_state_init.
 */
void _gfx_state_terminate(void);

/**
 * Allocates thread local state for the calling thread.
 * _groufix.initialized must be 1.
 * May not be called when data is already allocated on the calling thread.
 * @return Non-zero on success.
 */
int _gfx_state_create_local(void);

/**
 * Frees thread local state of the calling thread.
 * _groufix.initialized must be 1.
 * May not be called when no data is allocated on the calling thread.
 * All threads with local data need to call this before _gfx_state_terminate.
 */
void _gfx_state_destroy_local(void);

/**
 * Retrieves thread local state of the calling thread.
 * _groufix.initialized must be 1.
 * @return NULL if no state was allocated.
 */
_GFXThreadState* _gfx_state_get_local(void);


/****************************
 * Vulkan and its device state.
 ****************************/

/**
 * Logs a Vulkan result as a readable string.
 */
void _gfx_vulkan_log(VkResult result);

/**
 * Initializes Vulkan state, including all physical devices.
 * _groufix.vk.instance must be NULL.
 * Must be called by the same thread that called _gfx_state_init.
 * @return Non-zero on success.
 */
int _gfx_vulkan_init(void);

/**
 * Terminates Vulkan state.
 * This will make sure all physical devices will be destroyed.
 * Must be called by the same thread that called _gfx_state_init.
 */
void _gfx_vulkan_terminate(void);

/**
 * Retrieves the Vulkan context.
 * It will automatically be created if it did not exist yet.
 * The device will share its context with all devices in its device group.
 * @param device Cannot be NULL.
 * @return NULL if the context could not be found or created.
 */
_GFXContext* _gfx_vulkan_get_context(GFXDevice* device);


/****************************
 * Monitor configuration.
 ****************************/

/**
 * Initializes internal monitor configuration.
 * _groufix.monitors.size must be 0.
 * Must be called by the same thread that called _gfx_state_init.
 * @return Non-zero on success.
 */
int _gfx_monitors_init(void);

/**
 * Terminates internal monitor configuration.
 * This will make sure any monitors will be destroyed.
 * Must be called by the same thread that called _gfx_state_init.
 */
void _gfx_monitors_terminate(void);


#endif