/**
 * This file is part of groufix.
 * Copyright (c) Stef Velzel. All rights reserved.
 *
 * groufix : graphics engine produced by Stef Velzel.
 * www     : <www.vuzzel.nl>
 */

#include "groufix/core/objects.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>


// Detect whether a render pass is warmed.
#define _GFX_PASS_IS_WARMED(rPass) (rPass->vk.pass != VK_NULL_HANDLE)

// Detect whether a render pass is built.
#define _GFX_PASS_IS_BUILT(rPass) (rPass->vk.frames.size > 0)

// Auto log on any zero or mismatching framebuffer dimensions.
#define _GFX_VALIDATE_DIMS(rPass, width, height, layers, action) \
	do { \
		if ((width) == 0 || (height) == 0 || (layers) == 0) \
		{ \
			gfx_log_debug( /* Not an error if e.g. minimized. */ \
				"Encountered framebuffer dimensions " \
				"(%"PRIu32"x%"PRIu32"x%"PRIu32") " \
				"of zero during pass building, pass skipped.", \
				width, height, layers); \
			action; \
		} \
		else if ( \
			(rPass->build.fWidth && (width) != rPass->build.fWidth) || \
			(rPass->build.fHeight && (height) != rPass->build.fHeight) || \
			(rPass->build.fLayers && (layers) != rPass->build.fLayers)) \
		{ \
			gfx_log_warn( \
				"Encountered mismatching framebuffer dimensions " \
				"(%"PRIu32"x%"PRIu32"x%"PRIu32") " \
				"(%"PRIu32"x%"PRIu32"x%"PRIu32") " \
				"during pass building, pass skipped.", \
				rPass->build.fWidth, rPass->build.fHeight, rPass->build.fLayers, \
				width, height, layers); \
			action; \
		} \
		else { \
			rPass->build.fWidth = width; \
			rPass->build.fHeight = height; \
			rPass->build.fLayers = layers; \
		} \
	} while (0);


/****************************
 * Image view (for all framebuffers) element definition.
 */
typedef struct _GFXViewElem
{
	const _GFXConsume* consume;
	VkImageView        view; // Remains VK_NULL_HANDLE if a swapchain.

} _GFXViewElem;


/****************************
 * Frame (framebuffer + swapchain view) element definition.
 */
typedef struct _GFXFrameElem
{
	VkImageView   view; // Swapchain view, may be VK_NULL_HANDLE.
	VkFramebuffer buffer;

} _GFXFrameElem;


/****************************
 * Compares two user defined rasterization state descriptions.
 * @return Non-zero if equal.
 */
static inline bool _gfx_cmp_raster(const GFXRasterState* l,
                                   const GFXRasterState* r)
{
	return
		l->mode == r->mode &&
		l->front == r->front &&
		l->cull == r->cull &&
		l->topo == r->topo &&
		l->samples == r->samples;
}

/****************************
 * Compares two user defined blend state descriptions.
 * @return Non-zero if equal.
 */
static inline bool _gfx_cmp_blend(const GFXBlendState* l,
                                  const GFXBlendState* r)
{
	return
		l->logic == r->logic &&
		l->color.srcFactor == r->color.srcFactor &&
		l->color.dstFactor == r->color.dstFactor &&
		l->color.op == r->color.op &&
		l->alpha.srcFactor == r->alpha.srcFactor &&
		l->alpha.dstFactor == r->alpha.dstFactor &&
		l->alpha.op == r->alpha.op &&
		l->constants[0] == r->constants[0] &&
		l->constants[1] == r->constants[1] &&
		l->constants[2] == r->constants[2] &&
		l->constants[3] == r->constants[3];
}

/****************************
 * Compares two user defined depth state descriptions.
 * @return Non-zero if equal.
 */
static inline bool _gfx_cmp_depth(const GFXDepthState* l,
                                  const GFXDepthState* r)
{
	return
		l->flags == r->flags &&
		l->cmp == r->cmp &&
		(!(l->flags & GFX_DEPTH_BOUNDED) ||
			(l->minDepth == r->minDepth &&
			l->maxDepth == r->maxDepth));
}

/****************************
 * Compares two user defined stencil operation states.
 * @return Non-zero if equal.
 */
static inline bool _gfx_cmp_stencil(const GFXStencilOpState* l,
                                    const GFXStencilOpState* r)
{
	return
		l->fail == r->fail &&
		l->pass == r->pass &&
		l->depthFail == r->depthFail &&
		l->cmp == r->cmp &&
		l->cmpMask == r->cmpMask &&
		l->writeMask == r->writeMask &&
		l->reference == r->reference;
}

/****************************
 * Increases the pass 'generation'; invalidating any renderable/computable
 * pipeline that references this pass.
 */
static inline void _gfx_pass_gen(_GFXRenderPass* rPass)
{
	if (++rPass->gen == 0) gfx_log_warn(
		"Pass build generation reached maximum (%"PRIu32") and overflowed; "
		"may cause old renderables/computables to not be invalidated.",
		UINT32_MAX);
}

/****************************
 * Stand-in function for all the gfx_pass_consume* variants.
 * The `flags`, `mask`, `stage` and `view` fields of consume must be set.
 * @see gfx_pass_consume*.
 * @param consume Cannot be NULL.
 */
static bool _gfx_pass_consume(GFXPass* pass, _GFXConsume* consume)
{
	assert(pass != NULL);
	assert(!pass->renderer->recording);
	assert(consume != NULL);

	// Firstly, remove any host access mask, images cannot be mapped!
	consume->mask &= ~(GFXAccessMask)GFX_ACCESS_HOST_READ_WRITE;

	// Try to find it first.
	_GFXConsume* con;

	for (size_t i = pass->consumes.size; i > 0; --i)
	{
		con = gfx_vec_at(&pass->consumes, i-1);
		if (con->view.index == consume->view.index)
		{
			// Keep old clear, blend & resolve values.
			_GFXConsume t = *con;
			*con = *consume;

			if (t.flags & _GFX_CONSUME_BLEND)
				con->flags |= _GFX_CONSUME_BLEND;

			con->cleared = t.cleared;
			con->clear = t.clear;
			con->color = t.color;
			con->alpha = t.alpha;
			con->resolve = t.resolve;

			goto invalidate;
		}
	}

	// Insert anew.
	if (!gfx_vec_push(&pass->consumes, 1, consume))
		return 0;

	con = gfx_vec_at(&pass->consumes, pass->consumes.size - 1);

	// With some default values.
	GFXBlendOpState blendOpState = {
		.srcFactor = GFX_FACTOR_ONE,
		.dstFactor = GFX_FACTOR_ZERO,
		.op = GFX_BLEND_NO_OP
	};

	con->cleared = 0;
	con->clear.gfx = (GFXClear){ .depth = 0.0f, .stencil = 0 };
	con->color = blendOpState;
	con->alpha = blendOpState;
	con->resolve = SIZE_MAX;

invalidate:
	// Always reset graph output.
	con->out.initial = VK_IMAGE_LAYOUT_UNDEFINED;
	con->out.final = VK_IMAGE_LAYOUT_UNDEFINED;
	con->out.prev = NULL;

	// Changed a pass, the graph is invalidated.
	// This makes it so the graph will destruct this pass before anything else.
	_gfx_render_graph_invalidate(pass->renderer);

	return 1;
}

/****************************
 * Destructs a subset of all Vulkan objects, non-recursively.
 * @param rPass Cannot be NULL.
 * @param flags What resources should be destroyed (0 to do nothing).
 *
 * Not thread-safe with respect to pushing stale resources!
 */
static void _gfx_pass_destruct_partial(_GFXRenderPass* rPass,
                                       _GFXRecreateFlags flags)
{
	assert(rPass != NULL);
	assert(rPass->base.type == GFX_PASS_RENDER);

	// The recreate flag is always set if anything is set and signals that
	// the actual images have been recreated.
	if (flags & _GFX_RECREATE)
	{
		// Make all framebuffers and views stale.
		// Note that they might still be in use by pending virtual frames.
		// NOT locked using the renderer's lock;
		// the reason that _gfx_pass_(build|destruct) are not thread-safe.
		for (size_t i = 0; i < rPass->vk.frames.size; ++i)
		{
			_GFXFrameElem* elem = gfx_vec_at(&rPass->vk.frames, i);
			_gfx_push_stale(rPass->base.renderer,
				elem->buffer, elem->view,
				VK_NULL_HANDLE, VK_NULL_HANDLE);
		}

		for (size_t i = 0; i < rPass->vk.views.size; ++i)
		{
			_GFXViewElem* elem = gfx_vec_at(&rPass->vk.views, i);
			if (elem->view != VK_NULL_HANDLE)
				_gfx_push_stale(rPass->base.renderer,
					VK_NULL_HANDLE, elem->view,
					VK_NULL_HANDLE, VK_NULL_HANDLE);

			// We DO NOT release rPass->vk.views.
			// This because on-swapchain recreate, the consumptions of
			// attachments have not changed, we just have new images with
			// potentially new dimensions.
			// Meaning we do not need to filter all consumptions into
			// framebuffer views, we only need to recreate the views.
			elem->view = VK_NULL_HANDLE;
		}

		// We do not re-filter, so we must keep `build.backing`!
		rPass->build.fWidth = 0;
		rPass->build.fHeight = 0;
		rPass->build.fLayers = 0;
		gfx_vec_release(&rPass->vk.frames); // Force a rebuild.
	}

	// Second, we check if the Vulkan render pass needs to be reconstructed.
	// This object is cached, so no need to destroy anything.
	if (flags & _GFX_REFORMAT)
	{
		rPass->build.pass = NULL;
		rPass->vk.pass = VK_NULL_HANDLE;

		// Increase generation; the render pass is used in pipelines,
		// ergo we need to invalidate current pipelines using it.
		_gfx_pass_gen(rPass);
	}
}

/****************************/
GFXPass* _gfx_create_pass(GFXRenderer* renderer, GFXPassType type,
                          size_t numParents, GFXPass** parents)
{
	assert(renderer != NULL);
	assert(numParents == 0 || parents != NULL);

	// Check if all parents are compatible.
	for (size_t p = 0; p < numParents; ++p)
	{
		if (parents[p]->renderer != renderer)
		{
			gfx_log_error(
				"Render/compute passes cannot be the parent of a pass "
				"associated with a different renderer.");

			return NULL;
		}

		if (
			(type == GFX_PASS_COMPUTE_ASYNC &&
				parents[p]->type != GFX_PASS_COMPUTE_ASYNC) ||
			(type != GFX_PASS_COMPUTE_ASYNC &&
				parents[p]->type == GFX_PASS_COMPUTE_ASYNC))
		{
			gfx_log_error(
				"Asynchronous compute passes cannot be the parent of any "
				"render or inline compute pass and vice versa.");

			return NULL;
		}
	}

	// Allocate a new pass.
	const size_t structSize =
		(type == GFX_PASS_RENDER) ?
		sizeof(_GFXRenderPass) : sizeof(_GFXComputePass);

	GFXPass* pass = malloc(
		structSize +
		sizeof(GFXPass*) * numParents);

	if (pass == NULL)
		return NULL;

	// Initialize things.
	pass->type = type;
	pass->renderer = renderer;
	pass->level = 0;
	pass->order = 0;
	pass->childs = 0;

	gfx_vec_init(&pass->consumes, sizeof(_GFXConsume));
	gfx_vec_init(&pass->deps, sizeof(GFXInject));

	// The level is the highest level of all parents + 1.
	for (size_t p = 0; p < numParents; ++p)
	{
		if (parents[p]->level >= pass->level)
			pass->level = parents[p]->level + 1;

		++parents[p]->childs; // (!)
	}

	// Initialize as render pass.
	if (type == GFX_PASS_RENDER)
	{
		_GFXRenderPass* rPass = (_GFXRenderPass*)pass;
		rPass->gen = 0;

		// Set parents.
		rPass->numParents = numParents;
		memcpy(rPass->parents, parents, sizeof(GFXPass*) * numParents);

		// Initialize building stuff.
		rPass->out.master = NULL;
		rPass->out.next = NULL;
		rPass->out.subpass = 0;

		rPass->build.backing = SIZE_MAX;
		rPass->build.fWidth = 0;
		rPass->build.fHeight = 0;
		rPass->build.fLayers = 0;
		rPass->build.pass = NULL;
		rPass->vk.pass = VK_NULL_HANDLE;

		gfx_vec_init(&rPass->vk.clears, sizeof(VkClearValue));
		gfx_vec_init(&rPass->vk.blends, sizeof(VkPipelineColorBlendAttachmentState));
		gfx_vec_init(&rPass->vk.views, sizeof(_GFXViewElem));
		gfx_vec_init(&rPass->vk.frames, sizeof(_GFXFrameElem));

		// And finally some default state.
		rPass->state.samples = 1;
		rPass->state.enabled = 0;

		rPass->state.raster = (GFXRasterState){
			.mode = GFX_RASTER_FILL,
			.front = GFX_FRONT_FACE_CW,
			.cull = GFX_CULL_BACK,
			.topo = GFX_TOPO_TRIANGLE_LIST,
			.samples = 1
		};

		GFXBlendOpState blendOpState = {
			.srcFactor = GFX_FACTOR_ONE,
			.dstFactor = GFX_FACTOR_ZERO,
			.op = GFX_BLEND_NO_OP
		};

		rPass->state.blend = (GFXBlendState){
			.logic = GFX_LOGIC_NO_OP,
			.color = blendOpState,
			.alpha = blendOpState,
			.constants = { 0.0f, 0.0f, 0.0f, 0.0f }
		};

		rPass->state.depth = (GFXDepthState){
			.flags = GFX_DEPTH_WRITE,
			.cmp = GFX_CMP_LESS,
		};

		GFXStencilOpState stencilOpState = {
			.fail = GFX_STENCIL_KEEP,
			.pass = GFX_STENCIL_KEEP,
			.depthFail = GFX_STENCIL_KEEP,
			.cmp = GFX_CMP_NEVER,

			.cmpMask = 0,
			.writeMask = 0,
			.reference = 0
		};

		rPass->state.stencil = (GFXStencilState){
			.front = stencilOpState,
			.back = stencilOpState
		};
	}

	// Initialize as compute pass.
	else
	{
		_GFXComputePass* cPass = (_GFXComputePass*)pass;

		// Set parents.
		cPass->numParents = numParents;
		memcpy(cPass->parents, parents, sizeof(GFXPass*) * numParents);
	}

	return pass;
}

/****************************/
void _gfx_destroy_pass(GFXPass* pass)
{
	assert(pass != NULL);

	// Destruct as render pass.
	if (pass->type == GFX_PASS_RENDER)
	{
		_GFXRenderPass* rPass = (_GFXRenderPass*)pass;

		// Destruct all partial things.
		_gfx_pass_destruct_partial(rPass, _GFX_RECREATE_ALL);

		// Free all remaining things.
		gfx_vec_clear(&rPass->vk.clears);
		gfx_vec_clear(&rPass->vk.blends);
		gfx_vec_clear(&rPass->vk.views);
		gfx_vec_clear(&rPass->vk.frames);

		// Decrease child counter of all parents.
		for (size_t p = 0; p < rPass->numParents; ++p)
			--rPass->parents[p]->childs;
	}

	// Destruct as compute pass.
	else
	{
		_GFXComputePass* cPass = (_GFXComputePass*)pass;

		// Decrease child counter of all parents.
		for (size_t p = 0; p < cPass->numParents; ++p)
			--cPass->parents[p]->childs;
	}

	// More destruction.
	gfx_vec_clear(&pass->consumes);
	gfx_vec_clear(&pass->deps);

	free(pass);
}

/****************************/
VkFramebuffer _gfx_pass_framebuffer(_GFXRenderPass* rPass, GFXFrame* frame)
{
	assert(rPass != NULL);
	assert(rPass->base.type == GFX_PASS_RENDER);
	assert(frame != NULL);

	// TODO:GRA: Get framebuffer from master pass.

	// Just a single framebuffer.
	if (rPass->vk.frames.size == 1)
		return ((_GFXFrameElem*)gfx_vec_at(&rPass->vk.frames, 0))->buffer;

	// Query the swapchain image index.
	const uint32_t image =
		_gfx_frame_get_swapchain_index(frame, rPass->build.backing);

	// Validate & return.
	return rPass->vk.frames.size <= image ?
		VK_NULL_HANDLE :
		((_GFXFrameElem*)gfx_vec_at(&rPass->vk.frames, image))->buffer;
}

/****************************
 * Filters all consumed attachments into framebuffer views &
 * a potential window to use as back-buffer, silently logging issues.
 * @param rPass Cannot be NULL.
 * @return Zero on failure.
 */
static bool _gfx_pass_filter_attachments(_GFXRenderPass* rPass)
{
	assert(rPass != NULL);
	assert(rPass->base.type == GFX_PASS_RENDER);

	GFXRenderer* rend = rPass->base.renderer;

	// Already filtered.
	if (rPass->vk.views.size > 0)
		return 1;

	// TODO:GRA: Should get from all next subpasses too and skip if not master.
	// Literally point to the consume elem of a next pass.
	// Note that we can still only have one window attachment for
	// framebuffer creation reasons + we CAN have multiple depth/stencil
	// attachments now, one per subpass!

	// Reserve as many views as there are attachments, can never be more.
	if (!gfx_vec_reserve(&rPass->vk.views, rPass->base.consumes.size))
		return 0;

	// And start looping over all consumptions :)
	size_t depSten = SIZE_MAX; // Only to warn for duplicates.

	for (size_t i = 0; i < rPass->base.consumes.size; ++i)
	{
		const _GFXConsume* con = gfx_vec_at(&rPass->base.consumes, i);
		const _GFXAttach* at = gfx_vec_at(&rend->backing.attachs, con->view.index);

		// Validate existence of the attachment.
		if (
			con->view.index >= rend->backing.attachs.size ||
			at->type == _GFX_ATTACH_EMPTY)
		{
			gfx_log_warn(
				"Consumption of attachment at index %"GFX_PRIs" ignored, "
				"attachment not described.",
				con->view.index);

			continue;
		}

		// Validate that we want to access it as attachment.
		if (!(con->mask &
			(GFX_ACCESS_ATTACHMENT_INPUT |
			GFX_ACCESS_ATTACHMENT_READ |
			GFX_ACCESS_ATTACHMENT_WRITE |
			GFX_ACCESS_ATTACHMENT_RESOLVE)))
		{
			continue;
		}

		// If a window we read/write color to, pick it.
		if (at->type == _GFX_ATTACH_WINDOW &&
			(con->view.range.aspect & GFX_IMAGE_COLOR) &&
			(con->mask &
				(GFX_ACCESS_ATTACHMENT_READ |
				GFX_ACCESS_ATTACHMENT_WRITE |
				GFX_ACCESS_ATTACHMENT_RESOLVE)))
		{
			// Check if we already had a backing window.
			if (rPass->build.backing == SIZE_MAX)
				rPass->build.backing = con->view.index;
			else
			{
				// Skip any other candidate, cannot create a view for it.
				gfx_log_warn(
					"Consumption of attachment at index %"GFX_PRIs" "
					"ignored, a single pass can only read/write to a "
					"single window attachment at a time.",
					con->view.index);

				continue;
			}
		}

		// Skip any other windows too, no view will be created.
		else if (at->type == _GFX_ATTACH_WINDOW)
		{
			gfx_log_warn(
				"Consumption of attachment at index %"GFX_PRIs" ignored, "
				"a pass can only read/write to a window attachment.",
				con->view.index);

			continue;
		}

		// If a depth/stencil we read/write to, warn for duplicates.
		else if (
			GFX_FORMAT_HAS_DEPTH_OR_STENCIL(at->image.base.format) &&
			(con->view.range.aspect &
				(GFX_IMAGE_DEPTH | GFX_IMAGE_STENCIL)) &&
			(con->mask &
				(GFX_ACCESS_ATTACHMENT_READ | GFX_ACCESS_ATTACHMENT_WRITE)))
		{
			if (depSten == SIZE_MAX)
				depSten = con->view.index;
			else
				gfx_log_warn(
					"A single pass can only read/write to a single "
					"depth/stencil attachment at a time.");
		}

		// Add a view element referencing this consumption.
		_GFXViewElem elem = { .consume = con, .view = VK_NULL_HANDLE };
		gfx_vec_push(&rPass->vk.views, 1, &elem);
	}

	return 1;
}

/****************************
 * Finds a filtered attachment based on attachment index.
 * If not found, will return VK_ATTACHMENT_UNUSED.
 * @param rPass Cannot be NULL.
 * @param index Attachment index to find.
 * @return Index into VkRenderPassCreateInfo::pAttachments.
 */
static uint32_t _gfx_pass_find_attachment(_GFXRenderPass* rPass, size_t index)
{
	assert(rPass != NULL);
	assert(rPass->base.type == GFX_PASS_RENDER);

	// Early exit.
	if (index == SIZE_MAX)
		return VK_ATTACHMENT_UNUSED;

	// Find the view made for the consumption of the attachment at index.
	for (size_t i = 0; i < rPass->vk.views.size; ++i)
	{
		const _GFXViewElem* view = gfx_vec_at(&rPass->vk.views, i);
		if (view->consume->view.index == index) return (uint32_t)i;
	}

	return VK_ATTACHMENT_UNUSED;
}

/****************************/
bool _gfx_pass_warmup(_GFXRenderPass* rPass)
{
	assert(rPass != NULL);
	assert(rPass->base.type == GFX_PASS_RENDER);

	GFXRenderer* rend = rPass->base.renderer;

	// TODO:GRA: Somehow do this for all subpasses if this is master.
	// And skip all this if this is not master.
	// Need to set the correct state.enabled value for all subpasses.
	// And somehow propagate the VK pass and subpass index to all subpasses.
	// Used for creating pipelines, which are still for specific passes.

	// Already warmed.
	if (_GFX_PASS_IS_WARMED(rPass))
		return 1;

	// Ok so we need to know about all pass attachments.
	// Filter consumptions into attachment views.
	if (!_gfx_pass_filter_attachments(rPass))
		return 0;

	// We are always gonna update the clear & blend values.
	// Do it here and not build so we don't unnecessarily reconstruct this.
	// Same for state variables & enables.
	gfx_vec_release(&rPass->vk.clears);
	gfx_vec_release(&rPass->vk.blends);
	rPass->state.samples = 1;
	rPass->state.enabled = 0;

	// Both just need one element per view.
	if (!gfx_vec_reserve(&rPass->vk.clears, rPass->vk.views.size))
		return 0;

	if (!gfx_vec_reserve(&rPass->vk.blends, rPass->vk.views.size))
		return 0;

	// Describe all attachments.
	// We loop over all framebuffer views, which guarantees non-empty
	// attachments with attachment input/read/write/resolve access.
	// Keep track of all the input/color and depth/stencil attachment counts.
	size_t numInputs = 0;
	size_t numColors = 0;

	const VkAttachmentReference unused = (VkAttachmentReference){
		.attachment = VK_ATTACHMENT_UNUSED,
		.layout     = VK_IMAGE_LAYOUT_UNDEFINED
	};

	const size_t vlaViews = rPass->vk.views.size > 0 ? rPass->vk.views.size : 1;
	VkAttachmentDescription ad[vlaViews];
	VkAttachmentReference input[vlaViews];
	VkAttachmentReference color[vlaViews];
	VkAttachmentReference resolve[vlaViews];
	VkAttachmentReference depSten = unused;

	for (size_t i = 0; i < rPass->vk.views.size; ++i)
	{
		const _GFXViewElem* view = gfx_vec_at(&rPass->vk.views, i);
		const _GFXConsume* con = view->consume;
		const _GFXAttach* at = gfx_vec_at(&rend->backing.attachs, con->view.index);

		bool isColor = 0;

		// Swapchain.
		if (at->type == _GFX_ATTACH_WINDOW)
		{
			// Reference the attachment if appropriate.
			if (con->mask &
				(GFX_ACCESS_ATTACHMENT_READ | GFX_ACCESS_ATTACHMENT_WRITE))
			{
				resolve[numColors] = unused;
				color[numColors] = (VkAttachmentReference){
					.attachment = (uint32_t)i,
					.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
				};

				numColors++;
				isColor = 1;
			}

			// Describe the attachment.
			const bool clear = con->cleared & GFX_IMAGE_COLOR;
			const bool load = con->out.initial != VK_IMAGE_LAYOUT_UNDEFINED;

			ad[i] = (VkAttachmentDescription){
				.flags   = 0,
				.format  = at->window.window->frame.format,
				.samples = VK_SAMPLE_COUNT_1_BIT,

				.loadOp =
					(clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
					(load) ? VK_ATTACHMENT_LOAD_OP_LOAD :
					VK_ATTACHMENT_LOAD_OP_DONT_CARE,

				.storeOp = (con->mask & GFX_ACCESS_DISCARD) ?
					VK_ATTACHMENT_STORE_OP_DONT_CARE :
					VK_ATTACHMENT_STORE_OP_STORE,

				.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout  = con->out.initial,
				.finalLayout    = con->out.final
			};
		}

		// Non-swapchain.
		else
		{
			const GFXFormat fmt = at->image.base.format;

			const bool aspectMatch =
				con->view.range.aspect &
				(GFX_FORMAT_HAS_DEPTH_OR_STENCIL(fmt) ?
					GFX_IMAGE_DEPTH | GFX_IMAGE_STENCIL : GFX_IMAGE_COLOR);

			const bool firstClear =
				!GFX_FORMAT_HAS_DEPTH_OR_STENCIL(fmt) ?
					con->cleared & GFX_IMAGE_COLOR :
					GFX_FORMAT_HAS_DEPTH(fmt) &&
						con->cleared & GFX_IMAGE_DEPTH;

			const bool firstLoad =
				(GFX_FORMAT_HAS_DEPTH(fmt) || !GFX_FORMAT_HAS_STENCIL(fmt)) &&
					con->out.initial != VK_IMAGE_LAYOUT_UNDEFINED;

			const bool secondClear =
				GFX_FORMAT_HAS_STENCIL(fmt) &&
					con->cleared & GFX_IMAGE_STENCIL;

			const bool secondLoad =
				GFX_FORMAT_HAS_STENCIL(fmt) &&
					con->out.initial != VK_IMAGE_LAYOUT_UNDEFINED;

			// Build references.
			uint32_t resolveInd =
				_gfx_pass_find_attachment(rPass, con->resolve);

			const VkAttachmentReference ref = (VkAttachmentReference){
				.attachment = (uint32_t)i,
				.layout = _GFX_GET_VK_IMAGE_LAYOUT(con->mask, fmt)
			};

			const VkAttachmentReference refResolve =
				(resolveInd == VK_ATTACHMENT_UNUSED) ?
					unused :
					(VkAttachmentReference){
						.attachment = resolveInd,
						.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
					};

			// Reference the attachment if appropriate.
			if (con->mask & GFX_ACCESS_ATTACHMENT_INPUT)
				input[numInputs++] = aspectMatch ? ref : unused;

			if (con->mask &
				(GFX_ACCESS_ATTACHMENT_READ | GFX_ACCESS_ATTACHMENT_WRITE))
			{
				if (!GFX_FORMAT_HAS_DEPTH_OR_STENCIL(fmt))
					resolve[numColors] = aspectMatch ? refResolve : unused,
					color[numColors] = aspectMatch ? ref : unused,
					numColors++,
					isColor = 1;

				// Only set depSten on aspect match.
				else if (aspectMatch)
				{
					depSten = ref;

					// Adjust state enables.
					rPass->state.enabled &= ~(unsigned int)(
						_GFX_PASS_DEPTH | _GFX_PASS_STENCIL);
					rPass->state.enabled |= (unsigned int)(
						(GFX_FORMAT_HAS_DEPTH(fmt) ? _GFX_PASS_DEPTH : 0) |
						(GFX_FORMAT_HAS_STENCIL(fmt) ? _GFX_PASS_STENCIL : 0));
				}
			}

			// Describe the attachment.
			ad[i] = (VkAttachmentDescription){
				.flags   = 0,
				.format  = at->image.vk.format,
				.samples = at->image.base.samples,

				.loadOp =
					(firstClear) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
					(firstLoad) ? VK_ATTACHMENT_LOAD_OP_LOAD :
					VK_ATTACHMENT_LOAD_OP_DONT_CARE,

				.storeOp = (con->mask & GFX_ACCESS_DISCARD) ?
					VK_ATTACHMENT_STORE_OP_DONT_CARE :
					VK_ATTACHMENT_STORE_OP_STORE,

				.stencilLoadOp =
					(secondClear) ? VK_ATTACHMENT_LOAD_OP_CLEAR :
					(secondLoad) ? VK_ATTACHMENT_LOAD_OP_LOAD :
					VK_ATTACHMENT_LOAD_OP_DONT_CARE,

				.stencilStoreOp = (con->mask & GFX_ACCESS_DISCARD) ?
					VK_ATTACHMENT_STORE_OP_DONT_CARE :
					VK_ATTACHMENT_STORE_OP_STORE,

				.initialLayout = con->out.initial,
				.finalLayout = con->out.final
			};

			// Remember the greatest sample count for pipelines.
			if (ad[i].samples > rPass->state.samples)
				rPass->state.samples = ad[i].samples;
		}

		// Lastly, store the clear value for when we begin the pass,
		// memory is already reserved :)
		gfx_vec_push(&rPass->vk.clears, 1, &con->clear.vk);

		// Same for the blend values for building pipelines.
		if (isColor)
		{
			VkPipelineColorBlendAttachmentState pcbas = {
				.blendEnable         = VK_FALSE,
				.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
				.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
				.colorBlendOp        = VK_BLEND_OP_ADD,
				.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
				.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
				.alphaBlendOp        = VK_BLEND_OP_ADD,
				.colorWriteMask      =
					VK_COLOR_COMPONENT_R_BIT |
					VK_COLOR_COMPONENT_G_BIT |
					VK_COLOR_COMPONENT_B_BIT |
					VK_COLOR_COMPONENT_A_BIT
			};

			// Use independent blend state if given.
			const GFXBlendOpState* blendColor;
			const GFXBlendOpState* blendAlpha;

			if (con->flags & _GFX_CONSUME_BLEND)
				blendColor = &con->color,
				blendAlpha = &con->alpha;
			else
				blendColor = &rPass->state.blend.color,
				blendAlpha = &rPass->state.blend.alpha;

			if (blendColor->op != GFX_BLEND_NO_OP)
			{
				pcbas.blendEnable = VK_TRUE;
				pcbas.srcColorBlendFactor =
					_GFX_GET_VK_BLEND_FACTOR(blendColor->srcFactor);
				pcbas.dstColorBlendFactor =
					_GFX_GET_VK_BLEND_FACTOR(blendColor->dstFactor);
				pcbas.colorBlendOp =
					_GFX_GET_VK_BLEND_OP(blendColor->op);
			}

			if (blendAlpha->op != GFX_BLEND_NO_OP)
			{
				pcbas.blendEnable = VK_TRUE;
				pcbas.srcAlphaBlendFactor =
					_GFX_GET_VK_BLEND_FACTOR(blendAlpha->srcFactor);
				pcbas.dstAlphaBlendFactor =
					_GFX_GET_VK_BLEND_FACTOR(blendAlpha->dstFactor);
				pcbas.alphaBlendOp =
					_GFX_GET_VK_BLEND_OP(blendAlpha->op);
			}

			gfx_vec_push(&rPass->vk.blends, 1, &pcbas);
		}
	}

	// Ok now create the Vulkan render pass.
	VkSubpassDescription sd = {
		.flags                   = 0,
		.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.inputAttachmentCount    = (uint32_t)numInputs,
		.pInputAttachments       = numInputs > 0 ? input : NULL,
		.colorAttachmentCount    = (uint32_t)numColors,
		.pColorAttachments       = numColors > 0 ? color : NULL,
		.pResolveAttachments     = numColors > 0 ? resolve : NULL,
		.pDepthStencilAttachment =
			(depSten.attachment != VK_ATTACHMENT_UNUSED) ? &depSten : NULL,
		.preserveAttachmentCount = 0,
		.pPreserveAttachments    = NULL
	};

	VkRenderPassCreateInfo rpci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,

		.pNext           = NULL,
		.flags           = 0,
		.attachmentCount = (uint32_t)rPass->vk.views.size,
		.pAttachments    = rPass->vk.views.size > 0 ? ad : NULL,
		.subpassCount    = 1,
		.pSubpasses      = &sd,
		.dependencyCount = 0,
		.pDependencies   = NULL
	};

	// Remember the cache element for locality!
	rPass->build.pass = _gfx_cache_get(&rend->cache, &rpci.sType, NULL);
	if (rPass->build.pass == NULL) return 0;

	rPass->vk.pass = rPass->build.pass->vk.pass;

	return 1;
}

/****************************/
bool _gfx_pass_build(_GFXRenderPass* rPass)
{
	assert(rPass != NULL);
	assert(rPass->base.type == GFX_PASS_RENDER);

	GFXRenderer* rend = rPass->base.renderer;
	_GFXContext* context = rend->cache.context;

	// TODO:GRA: Skip all this if this is not master.
	// We somehow want to propagate the dimensions to all subpasses.

	// Already built.
	if (_GFX_PASS_IS_BUILT(rPass))
		return 1;

	// Do a warmup, i.e. make sure the Vulkan render pass is built.
	// This will log an error for us!
	if (!_gfx_pass_warmup(rPass))
		return 0;

	// We're gonna need to create all image views.
	// Keep track of the window used as backing so we can build framebuffers.
	// Also in here we're gonna get the dimensions (i.e. size) of the pass.
	VkImageView views[rPass->vk.views.size > 0 ? rPass->vk.views.size : 1];
	const _GFXAttach* backing = NULL;
	size_t backingInd = SIZE_MAX;

	for (size_t i = 0; i < rPass->vk.views.size; ++i)
	{
		_GFXViewElem* view = gfx_vec_at(&rPass->vk.views, i);
		const _GFXConsume* con = view->consume;
		const _GFXAttach* at = gfx_vec_at(&rend->backing.attachs, con->view.index);

		// Swapchain.
		if (at->type == _GFX_ATTACH_WINDOW)
		{
			// To be filled in below.
			backing = at;
			backingInd = i;
			views[i] = VK_NULL_HANDLE;

			// Validate dimensions.
			_GFX_VALIDATE_DIMS(rPass,
				at->window.window->frame.width,
				at->window.window->frame.height, 1,
				goto skip_pass);
		}

		// Non-swapchain.
		else
		{
			// Validate dimensions.
			// Do this first to avoid creating a non-existing image view.
			_GFX_VALIDATE_DIMS(rPass,
				at->image.width, at->image.height,
				(con->view.range.numLayers == 0) ?
					at->image.base.layers - con->view.range.layer :
					con->view.range.numLayers,
				goto skip_pass);

			// Resolve whole aspect from format,
			// then fix the consumed aspect as promised by gfx_pass_consume.
			const GFXFormat fmt = at->image.base.format;
			const GFXImageAspect aspect =
				con->view.range.aspect &
				(GFXImageAspect)(GFX_FORMAT_HAS_DEPTH_OR_STENCIL(fmt) ?
					(GFX_FORMAT_HAS_DEPTH(fmt) ? GFX_IMAGE_DEPTH : 0) |
					(GFX_FORMAT_HAS_STENCIL(fmt) ? GFX_IMAGE_STENCIL : 0) :
					GFX_IMAGE_COLOR);

			VkImageViewCreateInfo ivci = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,

				.pNext    = NULL,
				.flags    = 0,
				.image    = at->image.vk.image,
				.format   = at->image.vk.format,
				.viewType = (con->flags & _GFX_CONSUME_VIEWED) ?
					_GFX_GET_VK_IMAGE_VIEW_TYPE(con->view.type) :
					// Go head and translate from image to view type inline.
					(at->image.base.type == GFX_IMAGE_1D ? VK_IMAGE_VIEW_TYPE_1D :
					at->image.base.type == GFX_IMAGE_2D ? VK_IMAGE_VIEW_TYPE_2D :
					at->image.base.type == GFX_IMAGE_3D ? VK_IMAGE_VIEW_TYPE_3D :
					at->image.base.type == GFX_IMAGE_3D_SLICED ? VK_IMAGE_VIEW_TYPE_3D :
					at->image.base.type == GFX_IMAGE_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE :
					VK_IMAGE_VIEW_TYPE_2D),

				.components = {
					.r = VK_COMPONENT_SWIZZLE_IDENTITY,
					.g = VK_COMPONENT_SWIZZLE_IDENTITY,
					.b = VK_COMPONENT_SWIZZLE_IDENTITY,
					.a = VK_COMPONENT_SWIZZLE_IDENTITY
				},

				.subresourceRange = {
					.aspectMask     = _GFX_GET_VK_IMAGE_ASPECT(aspect),
					.baseMipLevel   = con->view.range.mipmap,
					.baseArrayLayer = con->view.range.layer,

					.levelCount = con->view.range.numMipmaps == 0 ?
						VK_REMAINING_MIP_LEVELS : con->view.range.numMipmaps,
					.layerCount = con->view.range.numLayers == 0 ?
						VK_REMAINING_ARRAY_LAYERS : con->view.range.numLayers
				}
			};

			VkImageView* vkView = views + i;
			_GFX_VK_CHECK(
				context->vk.CreateImageView(
					context->vk.device, &ivci, NULL, vkView),
				goto clean);

			view->view = *vkView; // So it's made stale later on.
		}
	}

	// Ok now we need to create all the framebuffers.
	// We either have one for each window image, or just a single one.
	// Reserve the exact amount, it's probably not gonna change.
	const size_t frames =
		(backingInd != SIZE_MAX) ?
		backing->window.window->frame.images.size : 1;

	if (!gfx_vec_reserve(&rPass->vk.frames, frames))
		goto clean;

	for (size_t i = 0; i < frames; ++i)
	{
		_GFXFrameElem elem = { .view = VK_NULL_HANDLE };

		// If there is a swapchain ..
		if (backingInd != SIZE_MAX)
		{
			// .. create another image view for each swapchain image.
			_GFXWindow* window = backing->window.window;
			VkImage image = *(VkImage*)gfx_vec_at(&window->frame.images, i);

			VkImageViewCreateInfo ivci = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,

				.pNext    = NULL,
				.flags    = 0,
				.image    = image,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format   = window->frame.format,

				.components = {
					.r = VK_COMPONENT_SWIZZLE_IDENTITY,
					.g = VK_COMPONENT_SWIZZLE_IDENTITY,
					.b = VK_COMPONENT_SWIZZLE_IDENTITY,
					.a = VK_COMPONENT_SWIZZLE_IDENTITY
				},

				.subresourceRange = {
					.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel   = 0,
					.levelCount     = 1,
					.baseArrayLayer = 0,
					.layerCount     = 1
				}
			};

			_GFX_VK_CHECK(
				context->vk.CreateImageView(
					context->vk.device, &ivci, NULL, &elem.view),
				goto clean);

			// Fill in the left-empty image view from above.
			views[backingInd] = elem.view;
		}

		// Create a Vulkan framebuffer.
		VkFramebufferCreateInfo fci = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,

			.pNext           = NULL,
			.flags           = 0,
			.renderPass      = rPass->vk.pass,
			.attachmentCount = (uint32_t)rPass->vk.views.size,
			.pAttachments    = rPass->vk.views.size > 0 ? views : NULL,
			.width           = GFX_MAX(1, rPass->build.fWidth),
			.height          = GFX_MAX(1, rPass->build.fHeight),
			.layers          = GFX_MAX(1, rPass->build.fLayers)
		};

		_GFX_VK_CHECK(
			context->vk.CreateFramebuffer(
				context->vk.device, &fci, NULL, &elem.buffer),
			{
				// Nvm immediately destroy the view.
				context->vk.DestroyImageView(
					context->vk.device, elem.view, NULL);
				goto clean;
			});

		// It was already reserved :)
		gfx_vec_push(&rPass->vk.frames, 1, &elem);
	}

	return 1;


	// Cleanup on failure.
clean:
	gfx_log_error("Could not build framebuffers for a pass.");

	// Get rid of built things; avoid dangling views.
	_gfx_pass_destruct_partial(rPass, _GFX_RECREATE);
	return 0;


	// Identical cleanup on skip.
skip_pass:
	_gfx_pass_destruct_partial(rPass, _GFX_RECREATE);
	return 1;
}

/****************************/
bool _gfx_pass_rebuild(_GFXRenderPass* rPass, _GFXRecreateFlags flags)
{
	assert(rPass != NULL);
	assert(rPass->base.type == GFX_PASS_RENDER);
	assert(flags & _GFX_RECREATE);

	// Remember if we're warmed or entirely built.
	const bool warmed = _GFX_PASS_IS_WARMED(rPass);
	const bool built = _GFX_PASS_IS_BUILT(rPass);

	// Then we destroy the things we want to recreate.
	_gfx_pass_destruct_partial(rPass, flags);

	// Then re-perform the remembered bits :)
	if (built)
		return _gfx_pass_build(rPass);
	if (warmed)
		return _gfx_pass_warmup(rPass);

	return 1;
}

/****************************/
void _gfx_pass_destruct(_GFXRenderPass* rPass)
{
	assert(rPass != NULL);
	assert(rPass->base.type == GFX_PASS_RENDER);

	// Destruct all partial things.
	_gfx_pass_destruct_partial(rPass, _GFX_RECREATE_ALL);

	// Need to re-calculate what window is consumed.
	rPass->build.backing = SIZE_MAX;

	// Clear memory.
	gfx_vec_clear(&rPass->vk.clears);
	gfx_vec_clear(&rPass->vk.blends);
	gfx_vec_clear(&rPass->vk.views);
	gfx_vec_clear(&rPass->vk.frames);
}

/****************************/
GFX_API GFXPassType gfx_pass_get_type(GFXPass* pass)
{
	assert(pass != NULL);

	return pass->type;
}

/****************************/
GFX_API bool gfx_pass_consume(GFXPass* pass, size_t index,
                              GFXAccessMask mask, GFXShaderStage stage)
{
	// Relies on stand-in function for asserts.

	_GFXConsume consume = {
		.flags = 0,
		.mask = mask,
		.stage = stage,
		// Take the entire reference.
		.view = {
			.index = index,
			.range = (GFXRange){
				// Specify all aspect flags, will be filtered later on.
				.aspect = GFX_IMAGE_COLOR | GFX_IMAGE_DEPTH | GFX_IMAGE_STENCIL,
				.mipmap = 0,
				.numMipmaps = 0,
				.layer = 0,
				.numLayers = 0
			}
		}
	};

	return _gfx_pass_consume(pass, &consume);
}

/****************************/
GFX_API bool gfx_pass_consumea(GFXPass* pass, size_t index,
                               GFXAccessMask mask, GFXShaderStage stage,
                               GFXRange range)
{
	// Relies on stand-in function for asserts.

	_GFXConsume consume = {
		.flags = 0,
		.mask = mask,
		.stage = stage,
		.view = {
			.index = index,
			.range = range
		}
	};

	return _gfx_pass_consume(pass, &consume);
}

/****************************/
GFX_API bool gfx_pass_consumev(GFXPass* pass, size_t index,
                               GFXAccessMask mask, GFXShaderStage stage,
                               GFXView view)
{
	// Relies on stand-in function for asserts.

	view.index = index; // Purely for function call consistency.

	_GFXConsume consume = {
		.flags = _GFX_CONSUME_VIEWED,
		.mask = mask,
		.stage = stage,
		.view = view
	};

	return _gfx_pass_consume(pass, &consume);
}

/****************************/
GFX_API void gfx_pass_clear(GFXPass* pass, size_t index,
                            GFXImageAspect aspect, GFXClear value)
{
	assert(pass != NULL);
	assert(!pass->renderer->recording);
	assert(!(aspect & GFX_IMAGE_COLOR) || aspect == GFX_IMAGE_COLOR);

	// Find and set.
	for (size_t i = pass->consumes.size; i > 0; --i)
	{
		_GFXConsume* con = gfx_vec_at(&pass->consumes, i-1);
		if (con->view.index == index)
		{
			// Set clear value, preserve other if only 1 of depth/stencil.
			if (aspect == GFX_IMAGE_DEPTH)
				value.stencil = con->clear.gfx.stencil;
			else if (aspect == GFX_IMAGE_STENCIL)
				value.depth = con->clear.gfx.depth;

			con->cleared = aspect;
			con->clear.gfx = value; // Type-punned into a VkClearValue!

			// Same as _gfx_pass_consume, invalidate for destruction.
			_gfx_render_graph_invalidate(pass->renderer);
			break;
		}
	}
}

/****************************/
GFX_API void gfx_pass_blend(GFXPass* pass, size_t index,
                            GFXBlendOpState color, GFXBlendOpState alpha)
{
	assert(pass != NULL);
	assert(!pass->renderer->recording);

	// Ignore if no-op.
	if (color.op == GFX_BLEND_NO_OP)
		color.srcFactor = GFX_FACTOR_ONE,
		color.dstFactor = GFX_FACTOR_ZERO;

	if (alpha.op == GFX_BLEND_NO_OP)
		alpha.srcFactor = GFX_FACTOR_ONE,
		alpha.dstFactor = GFX_FACTOR_ZERO;

	// Find and set.
	for (size_t i = pass->consumes.size; i > 0; --i)
	{
		_GFXConsume* con = gfx_vec_at(&pass->consumes, i-1);
		if (con->view.index == index)
		{
			con->flags |= _GFX_CONSUME_BLEND;
			con->color = color;
			con->alpha = alpha;

			// Same as _gfx_pass_consume, invalidate for destruction.
			_gfx_render_graph_invalidate(pass->renderer);
			break;
		}
	}
}

/****************************/
GFX_API void gfx_pass_resolve(GFXPass* pass, size_t index, size_t resolve)
{
	assert(pass != NULL);
	assert(!pass->renderer->recording);

	// Check that resolve is consumed.
	size_t i;
	for (i = pass->consumes.size; i > 0; --i)
	{
		_GFXConsume* con = gfx_vec_at(&pass->consumes, i-1);
		if (con->view.index == resolve)
			break;
	}

	// If it is, find and set.
	if (i > 0) for (i = pass->consumes.size; i > 0; --i)
	{
		_GFXConsume* con = gfx_vec_at(&pass->consumes, i-1);
		if (con->view.index == index)
		{
			con->resolve = resolve;

			// Same as _gfx_pass_consume, invalidate for destruction.
			_gfx_render_graph_invalidate(pass->renderer);
			break;
		}
	}
}

/****************************/
GFX_API void gfx_pass_release(GFXPass* pass, size_t index)
{
	assert(pass != NULL);
	assert(!pass->renderer->recording);

	// Find any that resolve to index.
	for (size_t i = pass->consumes.size; i > 0; --i)
	{
		_GFXConsume* con = gfx_vec_at(&pass->consumes, i-1);
		if (con->resolve == index)
		{
			con->resolve = SIZE_MAX;

			// Same as below, invalidate for destruction.
			_gfx_render_graph_invalidate(pass->renderer);
		}
	}

	// Find and erase.
	for (size_t i = pass->consumes.size; i > 0; --i)
	{
		_GFXConsume* con = gfx_vec_at(&pass->consumes, i-1);
		if (con->view.index == index)
		{
			gfx_vec_erase(&pass->consumes, 1, i-1);

			// Same as _gfx_pass_consume, invalidate for destruction.
			_gfx_render_graph_invalidate(pass->renderer);
			break;
		}
	}
}

/****************************/
GFX_API void gfx_pass_set_state(GFXPass* pass, GFXRenderState state)
{
	assert(pass != NULL);

	// No-op if not a render pass.
	_GFXRenderPass* rPass = (_GFXRenderPass*)pass;
	if (pass->type != GFX_PASS_RENDER) return;

	// Firstly check blend state, as new blend operations should cause the
	// `pass->vk.blends` vector to update, we do this by graph invalidation!
	bool newBlends = 0;

	if (state.blend != NULL)
		newBlends = !_gfx_cmp_blend(&rPass->state.blend, state.blend),
		rPass->state.blend = *state.blend;

	// Set new values, check if changed.
	bool gen = newBlends;

	if (state.raster != NULL)
		gen = gen || !_gfx_cmp_raster(&rPass->state.raster, state.raster),
		rPass->state.raster = *state.raster,
		// Fix sample count.
		rPass->state.raster.samples =
			_GFX_GET_VK_SAMPLE_COUNT(rPass->state.raster.samples);

	if (state.depth != NULL)
		gen = gen || !_gfx_cmp_depth(&rPass->state.depth, state.depth),
		rPass->state.depth = *state.depth;

	if (state.stencil != NULL)
		gen = gen ||
			!_gfx_cmp_stencil(&rPass->state.stencil.front, &state.stencil->front) ||
			!_gfx_cmp_stencil(&rPass->state.stencil.back, &state.stencil->back),
		rPass->state.stencil = *state.stencil;

	// If changed, increase generation to invalidate pipelines.
	// Unless we invalidate the graph, it implicitly destructs & increases.
	if (newBlends)
		_gfx_render_graph_invalidate(pass->renderer);
	else if (gen)
		_gfx_pass_gen(rPass);
}

/****************************/
GFX_API GFXRenderState gfx_pass_get_state(GFXPass* pass)
{
	assert(pass != NULL);

	if (pass->type == GFX_PASS_RENDER)
		return (GFXRenderState){
			.raster = &((_GFXRenderPass*)pass)->state.raster,
			.blend = &((_GFXRenderPass*)pass)->state.blend,
			.depth = &((_GFXRenderPass*)pass)->state.depth,
			.stencil = &((_GFXRenderPass*)pass)->state.stencil
		};
	else
		return (GFXRenderState){
			.raster = NULL,
			.blend = NULL,
			.depth = NULL,
			.stencil = NULL
		};
}

/****************************/
GFX_API size_t gfx_pass_get_num_parents(GFXPass* pass)
{
	assert(pass != NULL);

	return
		(pass->type == GFX_PASS_RENDER) ?
			((_GFXRenderPass*)pass)->numParents :
			((_GFXComputePass*)pass)->numParents;
}

/****************************/
GFX_API GFXPass* gfx_pass_get_parent(GFXPass* pass, size_t parent)
{
	assert(pass != NULL);
	assert(pass->type == GFX_PASS_RENDER ?
		parent < ((_GFXRenderPass*)pass)->numParents :
		parent < ((_GFXComputePass*)pass)->numParents);

	return
		(pass->type == GFX_PASS_RENDER) ?
			((_GFXRenderPass*)pass)->parents[parent] :
			((_GFXComputePass*)pass)->parents[parent];
}
