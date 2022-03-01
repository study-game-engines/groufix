/**
 * This file is part of groufix.
 * Copyright (c) Stef Velzel. All rights reserved.
 *
 * groufix : graphics engine produced by Stef Velzel.
 * www     : <www.vuzzel.nl>
 */


#ifndef GFX_CORE_RENDERER_H
#define GFX_CORE_RENDERER_H

#include "groufix/core/deps.h"
#include "groufix/core/device.h"
#include "groufix/core/formats.h"
#include "groufix/core/heap.h"
#include "groufix/core/refs.h"
#include "groufix/core/shader.h"
#include "groufix/core/window.h"
#include "groufix/def.h"


/**
 * Size class of a resource.
 */
typedef enum GFXSizeClass
{
	GFX_SIZE_ABSOLUTE,
	GFX_SIZE_RELATIVE

} GFXSizeClass;


/**
 * Attachment description.
 */
typedef struct GFXAttachment
{
	GFXImageType   type;
	GFXMemoryFlags flags;
	GFXImageUsage  usage;

	GFXFormat format;
	uint32_t  layers;

	// Optionally dynamic size.
	GFXSizeClass size;
	size_t       ref; // Index of the attachment the size is relative to.

	union {
		uint32_t width;
		float xScale;
	};

	union {
		uint32_t height;
		float yScale;
	};

	union {
		uint32_t depth;
		float zScale;
	};

} GFXAttachment;


/**
 * Image view type (interpreted dimensionality).
 */
typedef enum GFXViewType
{
	GFX_VIEW_1D,
	GFX_VIEW_1D_ARRAY,
	GFX_VIEW_2D,
	GFX_VIEW_2D_ARRAY,
	GFX_VIEW_CUBE,
	GFX_VIEW_CUBE_ARRAY,
	GFX_VIEW_3D

} GFXViewType;


/**
 * Sampler parameter flags.
 */
typedef enum GFXSamplerFlags
{
	GFX_SAMPLER_ANISOTROPY   = 0x0001,
	GFX_SAMPLER_COMPARE      = 0x0002,
	GFX_SAMPLER_UNNORMALIZED = 0x0004

} GFXSamplerFlags;


/**
 * Texture lookup filtering.
 */
typedef enum GFXFilter
{
	GFX_FILTER_NEAREST,
	GFX_FILTER_LINEAR

} GFXFilter;


/**
 * Texture lookup filter ('reduction') mode.
 */
typedef enum GFXFilterMode
{
	GFX_FILTER_MODE_AVERAGE,
	GFX_FILTER_MODE_MIN,
	GFX_FILTER_MODE_MAX

} GFXFilterMode;


/**
 * Texture lookup wrapping.
 */
typedef enum GFXWrapping
{
	GFX_WRAP_REPEAT,
	GFX_WRAP_REPEAT_MIRROR,
	GFX_WRAP_CLAMP_TO_EDGE,
	GFX_WRAP_CLAMP_TO_EDGE_MIRROR,
	GFX_WRAP_CLAMP_TO_BORDER

} GFXWrapping;


/**
 * Depth/stencil comparison operation.
 */
typedef enum GFXCompareOp
{
	GFX_CMP_NEVER,
	GFX_CMP_LESS,
	GFX_CMP_LESS_EQUAL,
	GFX_CMP_GREATER,
	GFX_CMP_GREATER_EQUAL,
	GFX_CMP_EQUAL,
	GFX_CMP_NOT_EQUAL,
	GFX_CMP_ALWAYS

} GFXCompareOp;


/**
 * Resource view description.
 */
typedef struct GFXView
{
	size_t binding; // Ignored for pass consumptions.
	size_t index;   // Attachment or binding array index.

	union {
		GFXFormat   format; // For texel buffers.
		GFXViewType type;   // For images.
	};

	GFXRange range;

} GFXView;


/**
 * Sampler description.
 */
typedef struct GFXSampler
{
	size_t binding;
	size_t index; // Binding array index.

	GFXSamplerFlags flags;
	GFXFilterMode   mode;

	GFXFilter minFilter;
	GFXFilter magFilter;
	GFXFilter mipFilter;

	GFXWrapping wrapU;
	GFXWrapping wrapV;
	GFXWrapping wrapW;

	float mipLodBias;
	float minLod;
	float maxLod;
	float maxAnisotropy;

	GFXCompareOp cmp;

} GFXSampler;


/**
 * Renderer definition.
 */
typedef struct GFXRenderer GFXRenderer;


/**
 * Technique (i.e. shader pipeline) definition.
 */
typedef struct GFXTechnique GFXTechnique;


/**
 * Set (i.e. render/descriptor set) definition.
 */
typedef struct GFXSet GFXSet;


/**
 * Pass (i.e. render/compute pass) definition.
 */
typedef struct GFXPass GFXPass;


/**
 * Virtual frame definition.
 */
typedef struct GFXFrame GFXFrame;


/****************************
 * Renderer handling.
 ****************************/

/**
 * Creates a renderer.
 * @param device NULL is equivalent to gfx_get_primary_device().
 * @param frames Number of virtual frames, must be > 0 (preferably > 1).
 * @return NULL on failure.
 *
 * All descendants of the renderer (techniques, sets, passes and frames) share
 * resources from the renderer and not the renderer, nor its descendants can be
 * operated on concurrently.
 * TODO: Except for the recording of frames!
 */
GFX_API GFXRenderer* gfx_create_renderer(GFXDevice* device, unsigned int frames);

/**
 * Destroys a renderer.
 * This will forcefully submit and block until rendering is done!
 */
GFX_API void gfx_destroy_renderer(GFXRenderer* renderer);

/**
 * Describes the properties of an image attachment of a renderer.
 * If the attachment already exists, it will be overwritten.
 * @param renderer Cannot be NULL.
 * @return Zero on failure.
 *
 * The GFX_MEMORY_HOST_VISIBLE flag is ignored, images cannot be mapped!
 * If anything needs to be detached, this will block until rendering is done!
 */
GFX_API int gfx_renderer_attach(GFXRenderer* renderer,
                                size_t index, GFXAttachment attachment);

/**
 * Attaches a window to an attachment index of a renderer.
 * If the attachment already exists, it will be overwritten.
 * @param renderer Cannot be NULL.
 * @param window   Cannot be NULL.
 * @return Zero on failure.
 *
 * Thread-safe with respect to window.
 * If anything needs to be detached, this will block until rendering is done!
 * Fails if the window was already attached to a renderer or the window and
 * renderer do not share a compatible device.
 */
GFX_API int gfx_renderer_attach_window(GFXRenderer* renderer,
                                       size_t index, GFXWindow* window);

/**
 * Retrieves the properties of an image attachment of a renderer.
 * @param renderer Cannot be NULL.
 * @param index    Must be < largest attachment index of renderer.
 * @return Empty attachment if none attached.
 *
 * An empty attachment has 0'd out values and
 * undefined `type`, `flags`, `usage` and `ref` fields.
 */
GFX_API GFXAttachment gfx_renderer_get_attach(GFXRenderer* renderer, size_t index);

/**
 * Retrieves a window at an attachment index of a renderer.
 * @param renderer Cannot be NULL.
 * @param index    Must be < largest attachment index of renderer.
 * @return NULL if no window is attached.
 */
GFX_API GFXWindow* gfx_renderer_get_window(GFXRenderer* renderer, size_t index);

/**
 * Detaches an attachment at a given index of a renderer.
 * Undescribed if not a window, detached if a window.
 * @param renderer Cannot be NULL.
 * @param index    Must be < largest attachment index of renderer.
 *
 * If anything is detached, this will block until rendering is done!
 */
GFX_API void gfx_renderer_detach(GFXRenderer* renderer, size_t index);


/****************************
 * Technique & set handling.
 ****************************/

/**
 * Adds a new technique to the renderer.
 * @param renderer   Cannot be NULL.
 * @param numShaders Must be > 0.
 * @param shaders    Cannot be NULL, all must store valid SPIR-V bytecode.
 * @return NULL on failure.
 */
GFX_API GFXTechnique* gfx_renderer_add_tech(GFXRenderer* renderer,
                                            size_t numShaders, GFXShader* shaders);

/**
 * Erases a technique from the renderer.
 * @param renderer  Cannot be NULL.
 * @param technique Cannot be NULL, must be of renderer.
 */
GFX_API void gfx_renderer_erase_tech(GFXRenderer* renderer, GFXTechnique* technique);

/**
 * Sets immutable samplers of the technique.
 * @param technique   Cannot be NULL.
 * @param set         Descriptor set number.
 * @param numSamplers Must be > 0.
 * @param samplers    Cannot be NULL.
 *
 * No-op if the technique was already used to render and/or create sets.
 * Samplers that do not match the shader input type are ignored.
 */
GFX_API void gfx_tech_set_samplers(GFXTechnique* technique, size_t set,
                                   size_t numSamplers, const GFXSampler* samplers);

/**
 * Sets buffer bindings of the technique to be dynamic.
 * @param technique Cannot be NULL.
 * @param set       Descriptor set number.
 * @param binding   Descriptor binding number.
 * @return Non-zero if the binding can be made dynamic.
 *
 * No-op if the technique was already used to render and/or create sets.
 * Ignored if the shader input type is not a uniform or storage buffer.
 */
GFX_API void gfx_tech_set_dynamic(GFXTechnique* technique, size_t set,
                                  size_t binding);


/****************************
 * Pass handling.
 ****************************/

/**
 * Adds a new (target) pass to the renderer given a set of parent.
 * A pass will be after all its parents in submission order.
 * Each element in parents must be associated with the same renderer.
 * @param renderer   Cannot be NULL.
 * @param numParents Number of parents, 0 for none.
 * @param parents    Parent passes, cannot be NULL if numParents > 0.
 * @return NULL on failure.
 */
GFX_API GFXPass* gfx_renderer_add_pass(GFXRenderer* renderer,
                                       size_t numParents, GFXPass** parents);

/**
 * Retrieves the number of target passes of a renderer.
 * A target pass is one that is not a parent off any pass (last in the path).
 * @param renderer Cannot be NULL.
 *
 * This number may change when a new pass is added.
 */
GFX_API size_t gfx_renderer_get_num_targets(GFXRenderer* renderer);

/**
 * Retrieves a target pass of a renderer.
 * @param renderer Cannot be NULL.
 * @param target   Target index, must be < gfx_renderer_get_num(renderer).
 *
 * The index of each target may change when a new pass is added,
 * however their order remains fixed during the lifetime of the renderer.
 */
GFX_API GFXPass* gfx_renderer_get_target(GFXRenderer* renderer, size_t target);

/**
 * TODO: shader location == in add-order?
 * Consume an attachment of a renderer.
 * @param pass  Cannot be NULL.
 * @param mask  Access mask to consume the attachment with.
 * @param stage Shader stages with access to the attachment.
 * @return Zero on failure.
 */
GFX_API int gfx_pass_consume(GFXPass* pass, size_t index,
                             GFXAccessMask mask, GFXShaderStage stage);

/**
 * Consumes a range (area) of an attachment of a renderer.
 * @see gfx_pass_consume.
 */
GFX_API int gfx_pass_consumea(GFXPass* pass, size_t index,
                              GFXAccessMask mask, GFXShaderStage stage,
                              GFXRange range);

/**
 * Consumes an attachment of a renderer with a specific view.
 * @param view Specifies all properties (and attachment index) to consume with.
 * @see gfx_pass_consume.
 */
GFX_API int gfx_pass_consumev(GFXPass* pass,
                              GFXAccessMask mask, GFXShaderStage stage,
                              GFXView view);

/**
 * Release any consumption of an attachment of the renderer.
 * @param pass  Cannot be NULL.
 * @param index Attachment index to release.
 */
GFX_API void gfx_pass_release(GFXPass* pass, size_t index);

/**
 * Retrieves the number of parents of a pass.
 * @param pass Cannot be NULL.
 */
GFX_API size_t gfx_pass_get_num_parents(GFXPass* pass);

/**
 * Retrieves a parent of a pass.
 * @param pass   Cannot be NULL.
 * @param parent Parent index, must be < gfx_pass_get_num_parents(pass).
 */
GFX_API GFXPass* gfx_pass_get_parent(GFXPass* pass, size_t parent);

/**
 * TODO: Totally temporary!
 * Makes the pass render the given things.
 */
GFX_API void gfx_pass_use(GFXPass* pass,
                          GFXPrimitive* primitive, GFXGroup* group);


/****************************
 * Frame handling.
 ****************************/

/**
 * Acquires the next virtual frame of a renderer, blocks until available!
 * Implicitly calls gfx_frame_submit if not yet done after the previous call.
 * @param renderer Cannot be NULL.
 * @return Always returns a valid frame.
 *
 * The renderer (including its attachments, passes and sets) cannot be
 * modified during or after this call until gfx_frame_submit has returned.
 */
GFX_API GFXFrame* gfx_renderer_acquire(GFXRenderer* renderer);

/**
 * Retrieves the index of a virtual frame, used to identify the frame.
 * All frame indices are in the range [0, #frames of the renderer].
 * They will be acquired in order, starting at 0.
 * @param frame Cannot be NULL.
 */
GFX_API unsigned int gfx_frame_get_index(GFXFrame* frame);

/**
 * TODO: Make a call before submit that takes the deps, store those, then
 * call _gfx_deps_catch, then let submit call _gfx_deps_prepare.
 * That way we can put draw() (or whatever) calls inbetween that ALSO modify
 * the dependency object :o !?
 *
 * Submits the acquired virtual frame of a renderer.
 * Must be called exactly once for each call to gfx_renderer_acquire.
 * @param frame Cannot be NULL, invalidated after this call!
 * @param deps  Cannot be NULL if numDeps > 0.
 *
 * All resources used to render a frame cannot be destroyed until the next
 * time this frame is acquired. The frames can be identified by their index.
 *
 * Failure during submission cannot be recovered from,
 * any such failure is appropriately logged.
 */
GFX_API void gfx_frame_submit(GFXFrame* frame,
                              size_t numDeps, const GFXInject* deps);


#endif
