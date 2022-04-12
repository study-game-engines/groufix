/**
 * This file is part of groufix.
 * Copyright (c) Stef Velzel. All rights reserved.
 *
 * groufix : graphics engine produced by Stef Velzel.
 * www     : <www.vuzzel.nl>
 */

#include "groufix/core/mem.h"
#include <assert.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>


// 'Randomized' magic number (generated by human imagination).
#define _GFX_HEADER_MAGIC ((uint32_t)0xff60af14)


// Pushes an lvalue to a hash key being built.
#define _GFX_KEY_PUSH(value) \
	do { \
		if (_gfx_hash_builder_push( \
			&builder, sizeof(value), &(value)) == NULL) \
		{ \
			goto clean; \
		} \
	} while (0)

// Pushes a handle into a hash key being built.
#define _GFX_KEY_PUSH_HANDLE() \
	do { \
		if (_gfx_hash_builder_push( \
			&builder, sizeof(*handles), &handles[currHandle++]) == NULL) \
		{ \
			goto clean; \
		} \
	} while (0)


/****************************
 * Unpacked groufix pipeline cache header.
 */
typedef struct _GFXPipelineCacheHeader
{
	uint32_t magic; // Equal to _GFX_HEADER_MAGIC.

	// Data size & hash including this header (with hash set to 0).
	uint32_t dataSize;
	uint64_t dataHash;

	// Vulkan values to validate compatibility.
	uint32_t vendorID;
	uint32_t deviceID;
	uint32_t driverVersion;
	uint32_t driverABI; // Equal to sizeof(void*).
	uint8_t  uuid[VK_UUID_SIZE];

} _GFXPipelineCacheHeader;


/****************************
 * Allocates & builds a hashable key value from a Vk*CreateInfo struct
 * with given replace handles for non-hashable fields.
 * @return Key value, must call free() on success (NULL on failure).
 */
static _GFXHashKey* _gfx_cache_alloc_key(const VkStructureType* createInfo,
                                         const void** handles)
{
	assert(createInfo != NULL);

	// Initialize a hash key builder.
	_GFXHashBuilder builder;
	if (!_gfx_hash_builder(&builder)) goto error;

	// Based on type, push all the to-be-hashed data.
	// Here we try to minimize the data actually necessary to specify
	// a unique cache object, so everything will be packed tightly.
	// The elements of the Vk*CreateInfo struct will be pushed linearly,
	// such as the specs say, to avoid confusion.
	// Note we do not push any VkStructureType fields except for the main one.
	// Plus we insert the given handles for fields we cannot hash.
	size_t currHandle = 0;
	char temp;

	switch (*createInfo)
	{
	case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO:

		_GFX_KEY_PUSH(*createInfo);
		const VkDescriptorSetLayoutCreateInfo* dslci =
			(const VkDescriptorSetLayoutCreateInfo*)createInfo;

		// Ignore the pNext field.
		_GFX_KEY_PUSH(dslci->flags);
		_GFX_KEY_PUSH(dslci->bindingCount);

		for (size_t b = 0; b < dslci->bindingCount; ++b)
		{
			const VkDescriptorSetLayoutBinding* dslb = dslci->pBindings + b;
			_GFX_KEY_PUSH(dslb->binding);
			_GFX_KEY_PUSH(dslb->descriptorType);
			_GFX_KEY_PUSH(dslb->descriptorCount);
			_GFX_KEY_PUSH(dslb->stageFlags);

			// Insert bool 'has immutable samplers'.
			temp =
				dslb->descriptorCount > 0 &&
				dslb->pImmutableSamplers != NULL;

			_GFX_KEY_PUSH(temp);

			if (dslb->pImmutableSamplers != NULL)
				for (size_t s = 0; s < dslb->descriptorCount; ++s)
					_GFX_KEY_PUSH_HANDLE();
		}
		break;

	case VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO:

		_GFX_KEY_PUSH(*createInfo);
		const VkPipelineLayoutCreateInfo* plci =
			(const VkPipelineLayoutCreateInfo*)createInfo;

		// Ignore the pNext field.
		// Ignore pipeline layout flags.
		_GFX_KEY_PUSH(plci->setLayoutCount);

		for (size_t s = 0; s < plci->setLayoutCount; ++s)
			_GFX_KEY_PUSH_HANDLE();

		_GFX_KEY_PUSH(plci->pushConstantRangeCount);

		for (size_t p = 0; p < plci->pushConstantRangeCount; ++p)
		{
			_GFX_KEY_PUSH(plci->pPushConstantRanges[p].stageFlags);
			_GFX_KEY_PUSH(plci->pPushConstantRanges[p].offset);
			_GFX_KEY_PUSH(plci->pPushConstantRanges[p].size);
		}
		break;

	case VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO:

		_GFX_KEY_PUSH(*createInfo);
		const VkSamplerCreateInfo* sci =
			(const VkSamplerCreateInfo*)createInfo;

		// Insert bool 'has pNext'.
		temp = sci->pNext != NULL;
		_GFX_KEY_PUSH(temp);

		// Assume pNext is a VkSamplerReductionModeCreateInfo*.
		if (sci->pNext != NULL)
		{
			const VkSamplerReductionModeCreateInfo* srmci =
				(const VkSamplerReductionModeCreateInfo*)sci->pNext;

			// Ignore the pNext field.
			_GFX_KEY_PUSH(srmci->reductionMode);
		}

		// Ignore sampler flags.
		_GFX_KEY_PUSH(sci->magFilter);
		_GFX_KEY_PUSH(sci->minFilter);
		_GFX_KEY_PUSH(sci->mipmapMode);
		_GFX_KEY_PUSH(sci->addressModeU);
		_GFX_KEY_PUSH(sci->addressModeV);
		_GFX_KEY_PUSH(sci->addressModeW);
		_GFX_KEY_PUSH(sci->mipLodBias);
		_GFX_KEY_PUSH(sci->anisotropyEnable);
		_GFX_KEY_PUSH(sci->maxAnisotropy);
		_GFX_KEY_PUSH(sci->compareEnable);
		_GFX_KEY_PUSH(sci->compareOp);
		_GFX_KEY_PUSH(sci->minLod);
		_GFX_KEY_PUSH(sci->maxLod);
		_GFX_KEY_PUSH(sci->borderColor);
		_GFX_KEY_PUSH(sci->unnormalizedCoordinates);
		break;

	case VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO:

		_GFX_KEY_PUSH(*createInfo);
		const VkRenderPassCreateInfo* rpci =
			(const VkRenderPassCreateInfo*)createInfo;

		// TODO: Push compatibility info first (with a prepended byte length),
		// so when building pipeline keys we can insert that info?

		// Ignore the pNext field.
		// Ignore render pass flags.
		_GFX_KEY_PUSH(rpci->attachmentCount);

		for (size_t a = 0; a < rpci->attachmentCount; ++a)
		{
			_GFX_KEY_PUSH(rpci->pAttachments[a].flags);
			_GFX_KEY_PUSH(rpci->pAttachments[a].format);
			_GFX_KEY_PUSH(rpci->pAttachments[a].samples);
			_GFX_KEY_PUSH(rpci->pAttachments[a].loadOp);
			_GFX_KEY_PUSH(rpci->pAttachments[a].storeOp);
			_GFX_KEY_PUSH(rpci->pAttachments[a].stencilLoadOp);
			_GFX_KEY_PUSH(rpci->pAttachments[a].stencilStoreOp);
			_GFX_KEY_PUSH(rpci->pAttachments[a].initialLayout);
			_GFX_KEY_PUSH(rpci->pAttachments[a].finalLayout);
		}

		_GFX_KEY_PUSH(rpci->subpassCount);

		for (size_t s = 0; s < rpci->subpassCount; ++s)
		{
			const VkSubpassDescription* sd = rpci->pSubpasses + s;
			// Ignore subpass flags.
			_GFX_KEY_PUSH(sd->pipelineBindPoint);
			_GFX_KEY_PUSH(sd->inputAttachmentCount);

			for (size_t i = 0; i < sd->inputAttachmentCount; ++i)
			{
				_GFX_KEY_PUSH(sd->pInputAttachments[i].attachment);
				_GFX_KEY_PUSH(sd->pInputAttachments[i].layout);
			}

			_GFX_KEY_PUSH(sd->colorAttachmentCount);

			for (size_t c = 0; c < sd->colorAttachmentCount; ++c)
			{
				_GFX_KEY_PUSH(sd->pColorAttachments[c].attachment);
				_GFX_KEY_PUSH(sd->pColorAttachments[c].layout);
			}

			// Insert bool 'has resolve attachments'.
			temp =
				sd->colorAttachmentCount > 0 &&
				sd->pResolveAttachments != NULL;

			_GFX_KEY_PUSH(temp);

			if (sd->pResolveAttachments != NULL)
				for (size_t r = 0; r < sd->colorAttachmentCount; ++r)
				{
					_GFX_KEY_PUSH(sd->pResolveAttachments[r].attachment);
					_GFX_KEY_PUSH(sd->pResolveAttachments[r].layout);
				}

			// Insert bool 'has depth stencil attachment'
			temp = sd->pDepthStencilAttachment != NULL;
			_GFX_KEY_PUSH(temp);

			if (sd->pDepthStencilAttachment != NULL)
			{
				_GFX_KEY_PUSH(sd->pDepthStencilAttachment->attachment);
				_GFX_KEY_PUSH(sd->pDepthStencilAttachment->layout);
			}

			_GFX_KEY_PUSH(sd->preserveAttachmentCount);

			for (size_t p = 0; p < sd->preserveAttachmentCount; ++p)
				_GFX_KEY_PUSH(sd->pPreserveAttachments[p]);
		}

		_GFX_KEY_PUSH(rpci->dependencyCount);

		for (size_t d = 0; d < rpci->dependencyCount; ++d)
		{
			_GFX_KEY_PUSH(rpci->pDependencies[d].srcSubpass);
			_GFX_KEY_PUSH(rpci->pDependencies[d].dstSubpass);
			_GFX_KEY_PUSH(rpci->pDependencies[d].srcStageMask);
			_GFX_KEY_PUSH(rpci->pDependencies[d].dstStageMask);
			_GFX_KEY_PUSH(rpci->pDependencies[d].srcAccessMask);
			_GFX_KEY_PUSH(rpci->pDependencies[d].dstAccessMask);
			_GFX_KEY_PUSH(rpci->pDependencies[d].dependencyFlags);
		}
		break;

	case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO:

		_GFX_KEY_PUSH(*createInfo);
		const VkGraphicsPipelineCreateInfo* gpci =
			(const VkGraphicsPipelineCreateInfo*)createInfo;

		// Ignore the pNext field.
		_GFX_KEY_PUSH(gpci->flags);
		_GFX_KEY_PUSH(gpci->stageCount);

		for (size_t s = 0; s < gpci->stageCount; ++s)
		{
			const VkPipelineShaderStageCreateInfo* pssci = gpci->pStages + s;
			// Ignore the pNext field.
			// Ignore shader stage flags.
			_GFX_KEY_PUSH(pssci->stage);
			_GFX_KEY_PUSH_HANDLE();
			// Ignore the entry point name.

			// Insert bool 'has specialization info'.
			temp = pssci->pSpecializationInfo != NULL;
			_GFX_KEY_PUSH(temp);

			if (pssci->pSpecializationInfo != NULL)
			{
				const VkSpecializationInfo* si = pssci->pSpecializationInfo;
				_GFX_KEY_PUSH(si->mapEntryCount);

				for (size_t e = 0; e < si->mapEntryCount; ++e)
				{
					_GFX_KEY_PUSH(si->pMapEntries[e].constantID);
					_GFX_KEY_PUSH(si->pMapEntries[e].offset);
					_GFX_KEY_PUSH(si->pMapEntries[e].size);
				}

				_GFX_KEY_PUSH(si->dataSize);

				if (si->dataSize > 0 &&
					_gfx_hash_builder_push(
						&builder, si->dataSize, si->pData) == NULL)
				{
					goto clean;
				}
			}
		}

		const VkPipelineVertexInputStateCreateInfo* pvisci = gpci->pVertexInputState;
		// Ignore the pNext field.
		// Ignore vertex input state flags.
		_GFX_KEY_PUSH(pvisci->vertexBindingDescriptionCount);

		for (size_t b = 0; b < pvisci->vertexBindingDescriptionCount; ++b)
		{
			const VkVertexInputBindingDescription* vibd =
				pvisci->pVertexBindingDescriptions + b;

			_GFX_KEY_PUSH(vibd->binding);
			_GFX_KEY_PUSH(vibd->stride);
			_GFX_KEY_PUSH(vibd->inputRate);
		}

		_GFX_KEY_PUSH(pvisci->vertexAttributeDescriptionCount);

		for (size_t a = 0; a < pvisci->vertexAttributeDescriptionCount; ++a)
		{
			const VkVertexInputAttributeDescription* viad =
				pvisci->pVertexAttributeDescriptions + a;

			_GFX_KEY_PUSH(viad->location);
			_GFX_KEY_PUSH(viad->binding);
			_GFX_KEY_PUSH(viad->format);
			_GFX_KEY_PUSH(viad->offset);
		}

		const VkPipelineInputAssemblyStateCreateInfo* piasci = gpci->pInputAssemblyState;
		// Ignore the pNext field.
		// Ignore input assembly state flags.
		_GFX_KEY_PUSH(piasci->topology);
		_GFX_KEY_PUSH(piasci->primitiveRestartEnable);

		// Insert bool 'has tessellation state'.
		temp = gpci->pTessellationState != NULL;
		_GFX_KEY_PUSH(temp);

		if (gpci->pTessellationState != NULL)
		{
			const VkPipelineTessellationStateCreateInfo* ptsci = gpci->pTessellationState;
			// Ignore the pNext field.
			// Ignore tessellation state flags.
			_GFX_KEY_PUSH(ptsci->patchControlPoints);
		}

		// Insert bool 'has viewport state'.
		temp = gpci->pViewportState != NULL;
		_GFX_KEY_PUSH(temp);

		if (gpci->pViewportState != NULL)
		{
			const VkPipelineViewportStateCreateInfo* pvsci = gpci->pViewportState;
			// Ignore the pNext field.
			// Ignore viewport state flags.
			_GFX_KEY_PUSH(pvsci->viewportCount);

			// Insert bool 'has viewports'.
			temp =
				pvsci->viewportCount > 0 &&
				pvsci->pViewports != NULL;

			_GFX_KEY_PUSH(temp);

			if (pvsci->pViewports != NULL)
				for (size_t v = 0; v < pvsci->viewportCount; ++v)
				{
					_GFX_KEY_PUSH(pvsci->pViewports[v].x);
					_GFX_KEY_PUSH(pvsci->pViewports[v].y);
					_GFX_KEY_PUSH(pvsci->pViewports[v].width);
					_GFX_KEY_PUSH(pvsci->pViewports[v].height);
					_GFX_KEY_PUSH(pvsci->pViewports[v].minDepth);
					_GFX_KEY_PUSH(pvsci->pViewports[v].maxDepth);
				}

			_GFX_KEY_PUSH(pvsci->scissorCount);

			// Insert bool 'has scissors'.
			temp =
				pvsci->scissorCount > 0 &&
				pvsci->pScissors != NULL;

			_GFX_KEY_PUSH(temp);

			if (pvsci->pScissors != NULL)
				for (size_t s = 0; s < pvsci->scissorCount; ++s)
				{
					_GFX_KEY_PUSH(pvsci->pScissors[s].offset);
					_GFX_KEY_PUSH(pvsci->pScissors[s].extent);
				}
		}

		const VkPipelineRasterizationStateCreateInfo* prsci = gpci->pRasterizationState;
		// Ignore the pNext field.
		// Ignore rasterization state flags.
		_GFX_KEY_PUSH(prsci->depthClampEnable);
		_GFX_KEY_PUSH(prsci->rasterizerDiscardEnable);
		_GFX_KEY_PUSH(prsci->polygonMode);
		_GFX_KEY_PUSH(prsci->cullMode);
		_GFX_KEY_PUSH(prsci->frontFace);
		_GFX_KEY_PUSH(prsci->depthBiasEnable);
		_GFX_KEY_PUSH(prsci->depthBiasConstantFactor);
		_GFX_KEY_PUSH(prsci->depthBiasClamp);
		_GFX_KEY_PUSH(prsci->depthBiasSlopeFactor);
		_GFX_KEY_PUSH(prsci->lineWidth);

		// Insert bool 'has multisample state'.
		temp = gpci->pMultisampleState != NULL;
		_GFX_KEY_PUSH(temp);

		if (gpci->pMultisampleState != NULL)
		{
			const VkPipelineMultisampleStateCreateInfo* pmsci = gpci->pMultisampleState;
			// Ignore the pNext field.
			// Ignore multisample state flags.
			_GFX_KEY_PUSH(pmsci->rasterizationSamples);
			_GFX_KEY_PUSH(pmsci->sampleShadingEnable);
			_GFX_KEY_PUSH(pmsci->minSampleShading);
			// Ignore sample masks.
			_GFX_KEY_PUSH(pmsci->alphaToCoverageEnable);
			_GFX_KEY_PUSH(pmsci->alphaToOneEnable);
		}

		// Insert bool 'has depth stencil state'.
		temp = gpci->pDepthStencilState != NULL;
		_GFX_KEY_PUSH(temp);

		if (gpci->pDepthStencilState != NULL)
		{
			const VkPipelineDepthStencilStateCreateInfo* pdssci = gpci->pDepthStencilState;
			// Ignore the pNext field.
			// Ignore depth stencil state flags.
			_GFX_KEY_PUSH(pdssci->depthTestEnable);
			_GFX_KEY_PUSH(pdssci->depthWriteEnable);
			_GFX_KEY_PUSH(pdssci->depthCompareOp);
			_GFX_KEY_PUSH(pdssci->depthBoundsTestEnable);
			_GFX_KEY_PUSH(pdssci->stencilTestEnable);
			_GFX_KEY_PUSH(pdssci->front);
			_GFX_KEY_PUSH(pdssci->back);
			_GFX_KEY_PUSH(pdssci->minDepthBounds);
			_GFX_KEY_PUSH(pdssci->maxDepthBounds);
		}

		// Insert bool 'has color blend state'.
		temp = gpci->pColorBlendState != NULL;
		_GFX_KEY_PUSH(temp);

		if (gpci->pColorBlendState != NULL)
		{
			const VkPipelineColorBlendStateCreateInfo* pcbsci = gpci->pColorBlendState;
			// Ignore the pNext field.
			// Ignore color blend state flags.
			_GFX_KEY_PUSH(pcbsci->logicOpEnable);
			_GFX_KEY_PUSH(pcbsci->logicOp);
			_GFX_KEY_PUSH(pcbsci->attachmentCount);

			for (size_t a = 0; a < pcbsci->attachmentCount; ++a)
			{
				_GFX_KEY_PUSH(pcbsci->pAttachments[a].blendEnable);
				_GFX_KEY_PUSH(pcbsci->pAttachments[a].srcColorBlendFactor);
				_GFX_KEY_PUSH(pcbsci->pAttachments[a].dstColorBlendFactor);
				_GFX_KEY_PUSH(pcbsci->pAttachments[a].colorBlendOp);
				_GFX_KEY_PUSH(pcbsci->pAttachments[a].srcAlphaBlendFactor);
				_GFX_KEY_PUSH(pcbsci->pAttachments[a].dstAlphaBlendFactor);
				_GFX_KEY_PUSH(pcbsci->pAttachments[a].alphaBlendOp);
				_GFX_KEY_PUSH(pcbsci->pAttachments[a].colorWriteMask);
			}

			if (_gfx_hash_builder_push(
				&builder, sizeof(pcbsci->blendConstants), pcbsci->blendConstants) == NULL)
			{
				goto clean;
			}
		}

		// Insert bool 'has dynamic state'.
		temp = gpci->pDynamicState != NULL;
		_GFX_KEY_PUSH(temp);

		if (gpci->pDynamicState != NULL)
		{
			const VkPipelineDynamicStateCreateInfo* pdsci = gpci->pDynamicState;
			// Ignore the pNext field.
			// Ignore dynamic state flags.
			_GFX_KEY_PUSH(pdsci->dynamicStateCount);

			for (size_t d = 0; d < pdsci->dynamicStateCount; ++d)
				_GFX_KEY_PUSH(pdsci->pDynamicStates[d]);
		}

		_GFX_KEY_PUSH_HANDLE();
		_GFX_KEY_PUSH_HANDLE();
		_GFX_KEY_PUSH(gpci->subpass);
		// Ignore base pipeline.
		// Ignore pipeline index.
		break;

	case VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO:

		_GFX_KEY_PUSH(*createInfo);
		const VkComputePipelineCreateInfo* cpci =
			(const VkComputePipelineCreateInfo*)createInfo;

		// Ignore the pNext field.
		_GFX_KEY_PUSH(cpci->flags);
		// Ignore the pNext field.
		// Ignore shader stage flags.
		_GFX_KEY_PUSH(cpci->stage.stage);
		_GFX_KEY_PUSH_HANDLE();
		// Ignore the entry point name.

		// Insert bool 'has specialization info'.
		temp = cpci->stage.pSpecializationInfo != NULL;
		_GFX_KEY_PUSH(temp);

		if (cpci->stage.pSpecializationInfo != NULL)
		{
			const VkSpecializationInfo* si = cpci->stage.pSpecializationInfo;
			_GFX_KEY_PUSH(si->mapEntryCount);

			for (size_t e = 0; e < si->mapEntryCount; ++e)
			{
				_GFX_KEY_PUSH(si->pMapEntries[e].constantID);
				_GFX_KEY_PUSH(si->pMapEntries[e].offset);
				_GFX_KEY_PUSH(si->pMapEntries[e].size);
			}

			_GFX_KEY_PUSH(si->dataSize);

			if (si->dataSize > 0 &&
				_gfx_hash_builder_push(
					&builder, si->dataSize, si->pData) == NULL)
			{
				goto clean;
			}
		}

		_GFX_KEY_PUSH_HANDLE();
		// Ignore base pipeline.
		// Ignore pipeline index.
		break;

	default:
		goto clean;
	}

	// Return the key data.
	return _gfx_hash_builder_get(&builder);


	// Cleanup on failure.
clean:
	free(_gfx_hash_builder_get(&builder));
error:
	gfx_log_error("Could not allocate key for cached Vulkan object.");

	return NULL;
}

/****************************
 * Creates a new Vulkan object using the given Vk*CreateInfo struct and
 * outputs to the given _GFXCacheElem struct.
 * @return Non-zero on success.
 */
static bool _gfx_cache_create_elem(_GFXCache* cache, _GFXCacheElem* elem,
                                   const VkStructureType* createInfo)
{
	assert(cache != NULL);
	assert(elem != NULL);
	assert(createInfo != NULL);

	_GFXContext* context = cache->context;

	// Firstly, set type.
	elem->type = *createInfo;

	// Then call the appropriate create function.
	switch (elem->type)
	{
	case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO:
		_GFX_VK_CHECK(
			context->vk.CreateDescriptorSetLayout(context->vk.device,
				(const VkDescriptorSetLayoutCreateInfo*)createInfo, NULL,
				&elem->vk.setLayout),
			goto error);

		// Go ahead and just create an update template inline.
		// This is as simple as creating an update entry for each binding.
		// We always update descriptor sets as a whole.
		{
			const VkDescriptorSetLayoutCreateInfo* dslci =
				(const VkDescriptorSetLayoutCreateInfo*)createInfo;

			VkDescriptorUpdateTemplateEntry entries[dslci->bindingCount];
			uint32_t count = dslci->bindingCount;
			size_t offset = 0;

			for (uint32_t b = 0; b < dslci->bindingCount; ++b)
			{
				if (
					dslci->pBindings[b].descriptorCount == 0 ||
					(dslci->pBindings[b].pImmutableSamplers != NULL &&
					dslci->pBindings[b].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER))
				{
					// Skip empty bindings or immutable samplers.
					--count;
					continue;
				}

				// Entry index.
				uint32_t e = b - (dslci->bindingCount - count);

				entries[e] = (VkDescriptorUpdateTemplateEntry){
					.dstBinding      = dslci->pBindings[b].binding,
					.dstArrayElement = 0,
					.descriptorCount = dslci->pBindings[b].descriptorCount,
					.descriptorType  = dslci->pBindings[b].descriptorType,
					.offset          = offset,
					.stride          = cache->templateStride
				};
				offset +=
					cache->templateStride *
					entries[e].descriptorCount;
			}

			// If no bindings remain, do not create an update template!
			if (count == 0)
			{
				elem->vk.template = VK_NULL_HANDLE;
				break;
			}

			VkDescriptorUpdateTemplateCreateInfo dutci = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,

				.pNext                      = NULL,
				.flags                      = 0,
				.descriptorUpdateEntryCount = count,
				.pDescriptorUpdateEntries   = entries,
				.descriptorSetLayout        = elem->vk.setLayout,

				.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET
			};

			_GFX_VK_CHECK(
				context->vk.CreateDescriptorUpdateTemplate(
					context->vk.device, &dutci, NULL, &elem->vk.template),
				{
					context->vk.DestroyDescriptorSetLayout(
						context->vk.device, elem->vk.setLayout, NULL);
					goto error;
				});
		}
		break;

	case VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO:
		_GFX_VK_CHECK(
			context->vk.CreatePipelineLayout(context->vk.device,
				(const VkPipelineLayoutCreateInfo*)createInfo, NULL,
				&elem->vk.layout),
			goto error);
		break;

	case VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO:
		// For samplers we have to check against Vulkan's allocation limit.
		// We have to lock such that two concurrent allocations both fail
		// properly if the limit only allows one more sampler.
		_gfx_mutex_lock(&context->limits.samplerLock);

		if (atomic_load(&context->limits.samplers) >= context->limits.maxSamplers)
		{
			gfx_log_error(
				"Cannot allocate sampler because physical device limit "
				"of %"PRIu32" sampler allocations has been reached.",
				context->limits.maxSamplers);

			_gfx_mutex_unlock(&context->limits.samplerLock);
			goto error;
		}

		// Increase the count & unlock.
		// Just unlock early, just like with memory allocations.
		atomic_fetch_add(&context->limits.samplers, 1);

		_gfx_mutex_unlock(&context->limits.samplerLock);

		_GFX_VK_CHECK(
			context->vk.CreateSampler(context->vk.device,
				(const VkSamplerCreateInfo*)createInfo, NULL,
				&elem->vk.sampler),
			{
				// Undo...
				atomic_fetch_sub(&context->limits.samplers, 1);
				goto error;
			});
		break;

	case VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO:
		_GFX_VK_CHECK(
			context->vk.CreateRenderPass(context->vk.device,
				(const VkRenderPassCreateInfo*)createInfo, NULL,
				&elem->vk.pass),
			goto error);
		break;

	case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO:
		_GFX_VK_CHECK(
			context->vk.CreateGraphicsPipelines(context->vk.device,
				cache->vk.cache, 1,
				(const VkGraphicsPipelineCreateInfo*)createInfo, NULL,
				&elem->vk.pipeline),
			goto error);
		break;

	case VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO:
		_GFX_VK_CHECK(
			context->vk.CreateComputePipelines(context->vk.device,
				cache->vk.cache, 1,
				(const VkComputePipelineCreateInfo*)createInfo, NULL,
				&elem->vk.pipeline),
			goto error);
		break;

	default:
		goto error;
	}

	return 1;


	// Error on failure.
error:
	gfx_log_error("Could not create cached Vulkan object.");
	return 0;
}

/****************************
 * Destroys the Vulkan object stored in the given _GFXCacheElem struct.
 */
static void _gfx_cache_destroy_elem(_GFXCache* cache, _GFXCacheElem* elem)
{
	assert(cache != NULL);
	assert(elem != NULL);

	_GFXContext* context = cache->context;

	// Call the appropriate destroy function from type.
	switch (elem->type)
	{
	case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO:
		context->vk.DestroyDescriptorUpdateTemplate(
			context->vk.device, elem->vk.template, NULL);
		context->vk.DestroyDescriptorSetLayout(
			context->vk.device, elem->vk.setLayout, NULL);
		break;

	case VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO:
		context->vk.DestroyPipelineLayout(
			context->vk.device, elem->vk.layout, NULL);
		break;

	case VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO:
		// We actually do decrease the sampler allocation count afterwards.
		context->vk.DestroySampler(
			context->vk.device, elem->vk.sampler, NULL);

		atomic_fetch_sub(&context->limits.samplers, 1);
		break;

	case VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO:
		context->vk.DestroyRenderPass(
			context->vk.device, elem->vk.pass, NULL);
		break;

	case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO:
	case VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO:
		context->vk.DestroyPipeline(
			context->vk.device, elem->vk.pipeline, NULL);
		break;

	default:
		break;
	}
}

/****************************
 * Stand-in function for _gfx_cache_get when given anything other than
 * a Vk*PipelineCreateInfo struct, i.e. we use the simple cache.
 */
static _GFXCacheElem* _gfx_cache_get_simple(_GFXCache* cache,
                                            const VkStructureType* createInfo,
                                            const void** handles)
{
	assert(cache != NULL);
	assert(createInfo != NULL);
	assert(
		*createInfo != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO &&
		*createInfo != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);

	// Firstly we create a key value & hash it.
	_GFXHashKey* key = _gfx_cache_alloc_key(createInfo, handles);
	if (key == NULL) return NULL;

	const uint64_t hash = cache->simple.hash(key);

	// Here we do need to lock the simple cache, as we want the function
	// to be reentrant. And we have a dedicated lock!
	_gfx_mutex_lock(&cache->simpleLock);

	// Try to find a matching element first.
	_GFXCacheElem* elem = gfx_map_hsearch(&cache->simple, key, hash);
	if (elem == NULL)
	{
		// If not found, create and insert a new element.
		elem = gfx_map_hinsert(
			&cache->simple, NULL, _gfx_hash_size(key), key, hash);

		if (elem != NULL && !_gfx_cache_create_elem(cache, elem, createInfo))
		{
			// On failure, erase & set return to NULL.
			gfx_map_erase(&cache->simple, elem);
			elem = NULL;
		}
	}

	// Unlock, free data & return.
	_gfx_mutex_unlock(&cache->simpleLock);
	free(key);
	return elem;
}

/****************************
 * Stand-in function for _gfx_cache_get when given
 * a Vk*PipelineCreateInfo struct.
 */
static _GFXCacheElem* _gfx_cache_get_pipeline(_GFXCache* cache,
                                              const VkStructureType* createInfo,
                                              const void** handles)
{
	assert(cache != NULL);
	assert(createInfo != NULL);
	assert(
		*createInfo == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
		*createInfo == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);

	// Again, create a key value & hash it.
	_GFXHashKey* key = _gfx_cache_alloc_key(createInfo, handles);
	if (key == NULL) return NULL;

	const uint64_t hash = cache->immutable.hash(key);

	// First we check the immutable cache.
	// This function does not need to run concurrent with _gfx_cache_warmup
	// and we do not modify, therefore we do not lock this cache :)
	_GFXCacheElem* elem = gfx_map_hsearch(&cache->immutable, key, hash);
	if (elem != NULL) goto found;

	// If not found in the immutable cache, check the mutable cache.
	// For this lookup we obviously do lock.
	_gfx_mutex_lock(&cache->lookupLock);
	elem = gfx_map_hsearch(&cache->mutable, key, hash);
	_gfx_mutex_unlock(&cache->lookupLock);

	if (elem != NULL) goto found;

	// If we did not find it yet, we need to insert a new element in the
	// mutable cache. We want other threads to still be able to query while
	// creating, so we lock for 'creation' separately.
	// But then we need to immediately check if the element already exists.
	// This because multiple threads could simultaneously decide to create
	// the same new element.
	_gfx_mutex_lock(&cache->createLock);

	_gfx_mutex_lock(&cache->lookupLock);
	elem = gfx_map_hsearch(&cache->mutable, key, hash);
	_gfx_mutex_unlock(&cache->lookupLock);

	if (elem != NULL)
	{
		_gfx_mutex_unlock(&cache->createLock);
		goto found;
	}

	// At this point we are the thread to actually create the new element.
	// We first create, then insert, so other threads don't accidentally
	// pick an incomplete element.
	_GFXCacheElem newElem;
	if (!_gfx_cache_create_elem(cache, &newElem, createInfo))
	{
		// Uh oh failed to create :(
		_gfx_mutex_unlock(&cache->createLock);
		free(key);
		return NULL;
	}

	// We created the thing, now insert the thing.
	// For this we block any lookups again.
	// When we're done we can also unlock for creation tho :)
	_gfx_mutex_lock(&cache->lookupLock);

	elem = gfx_map_hinsert(
		&cache->mutable, &newElem, _gfx_hash_size(key), key, hash);

	_gfx_mutex_unlock(&cache->lookupLock);
	_gfx_mutex_unlock(&cache->createLock);

	if (elem != NULL) goto found;

	// Ah, well, it is not in the map, away with it then...
	_gfx_cache_destroy_elem(cache, &newElem);
	free(key);
	return NULL;


	// Free data & return when found.
found:
	free(key);
	return elem;
}

/****************************/
bool _gfx_cache_init(_GFXCache* cache, _GFXDevice* device, size_t templateStride)
{
	assert(cache != NULL);
	assert(device != NULL);
	assert(device->context != NULL);
	assert(templateStride > 0);

	_GFXContext* context = device->context;

	cache->context = context;
	cache->templateStride = templateStride;
	cache->vk.device = device->vk.device;

	// Initialize the locks.
	if (!_gfx_mutex_init(&cache->simpleLock))
		return 0;

	if (!_gfx_mutex_init(&cache->lookupLock))
		goto clean_simple;

	if (!_gfx_mutex_init(&cache->createLock))
		goto clean_lookup;

	// Create an empty pipeline cache.
	VkPipelineCacheCreateInfo pcci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,

		.pNext           = NULL,
		.flags           = 0,
		.initialDataSize = 0,
		.pInitialData    = NULL
	};

	_GFX_VK_CHECK(context->vk.CreatePipelineCache(
		context->vk.device, &pcci, NULL, &cache->vk.cache), goto clean);

	// Initialize the hashtables.
	// Take the largest alignment of the key and element types.
	const size_t align =
		GFX_MAX(alignof(_GFXHashKey), alignof(_GFXCacheElem));

	gfx_map_init(&cache->simple,
		sizeof(_GFXCacheElem), align, _gfx_hash_murmur3, _gfx_hash_cmp);

	gfx_map_init(&cache->immutable,
		sizeof(_GFXCacheElem), align, _gfx_hash_murmur3, _gfx_hash_cmp);

	gfx_map_init(&cache->mutable,
		sizeof(_GFXCacheElem), align, _gfx_hash_murmur3, _gfx_hash_cmp);

	return 1;


	// Cleanup on failure.
clean:
	_gfx_mutex_clear(&cache->createLock);
clean_lookup:
	_gfx_mutex_clear(&cache->lookupLock);
clean_simple:
	_gfx_mutex_clear(&cache->simpleLock);

	return 0;
}

/****************************/
void _gfx_cache_clear(_GFXCache* cache)
{
	assert(cache != NULL);

	_GFXContext* context = cache->context;

	// Destroy all objects in the mutable cache.
	for (
		_GFXCacheElem* elem = gfx_map_first(&cache->mutable);
		elem != NULL;
		elem = gfx_map_next(&cache->mutable, elem))
	{
		_gfx_cache_destroy_elem(cache, elem);
	}

	// Destroy all objects in the immutable cache.
	for (
		_GFXCacheElem* elem = gfx_map_first(&cache->immutable);
		elem != NULL;
		elem = gfx_map_next(&cache->immutable, elem))
	{
		_gfx_cache_destroy_elem(cache, elem);
	}

	// Destroy all objects in the simple cache.
	for (
		_GFXCacheElem* elem = gfx_map_first(&cache->simple);
		elem != NULL;
		elem = gfx_map_next(&cache->simple, elem))
	{
		_gfx_cache_destroy_elem(cache, elem);
	}

	// Destroy the pipeline cache.
	context->vk.DestroyPipelineCache(
		context->vk.device, cache->vk.cache, NULL);

	// Clear all other things.
	gfx_map_clear(&cache->simple);
	gfx_map_clear(&cache->immutable);
	gfx_map_clear(&cache->mutable);

	_gfx_mutex_clear(&cache->simpleLock);
	_gfx_mutex_clear(&cache->lookupLock);
	_gfx_mutex_clear(&cache->createLock);
}

/****************************/
bool _gfx_cache_flush(_GFXCache* cache)
{
	assert(cache != NULL);

	// No need to lock anything, we just merge the tables.
	return gfx_map_merge(&cache->immutable, &cache->mutable);
}

/****************************/
_GFXCacheElem* _gfx_cache_get(_GFXCache* cache,
                              const VkStructureType* createInfo,
                              const void** handles)
{
	assert(cache != NULL);
	assert(createInfo != NULL);

	// Just route to the correct cache.
	const bool isPipeline =
		*createInfo == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
		*createInfo == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;

	if (isPipeline)
		return _gfx_cache_get_pipeline(cache, createInfo, handles);
	else
		return _gfx_cache_get_simple(cache, createInfo, handles);
}

/****************************/
bool _gfx_cache_warmup(_GFXCache* cache,
                       const VkStructureType* createInfo,
                       const void** handles)
{
	assert(cache != NULL);
	assert(createInfo != NULL);
	assert(
		*createInfo == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
		*createInfo == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);

	// Create a key value & hash it.
	_GFXHashKey* key = _gfx_cache_alloc_key(createInfo, handles);
	if (key == NULL) return 0;

	const uint64_t hash = cache->immutable.hash(key);

	// Here we do need to lock the immutable cache, as we want the function
	// to be reentrant. However we have no dedicated lock.
	// Luckily this function _does not_ need to be able to run concurrently
	// with _gfx_cache_get_pipeline, so we abuse the lookup lock :)
	_gfx_mutex_lock(&cache->lookupLock);

	// Try to find a matching element first.
	_GFXCacheElem* elem = gfx_map_hsearch(&cache->immutable, key, hash);
	if (elem != NULL)
		// Found one, done, we do not care if it is completely built yet.
		_gfx_mutex_unlock(&cache->lookupLock);
	else
	{
		// If not found, insert a new element.
		// Then immediately unlock so other warmups can be performed.
		elem = gfx_map_hinsert(
			&cache->immutable, NULL, _gfx_hash_size(key), key, hash);

		_gfx_mutex_unlock(&cache->lookupLock);

		// THEN create it :)
		if (elem == NULL || !_gfx_cache_create_elem(cache, elem, createInfo))
		{
			// Failed.. I suppose we erase the element.
			if (elem != NULL)
			{
				_gfx_mutex_lock(&cache->lookupLock);
				gfx_map_erase(&cache->immutable, elem);
				_gfx_mutex_unlock(&cache->lookupLock);
			}

			free(key);
			return 0;
		}
	}

	// Free data & return.
	free(key);
	return 1;
}

/****************************/
bool _gfx_cache_load(_GFXCache* cache, const GFXReader* src)
{
	assert(_groufix.vk.instance != NULL);
	assert(cache != NULL);
	assert(src != NULL);

	_GFXContext* context = cache->context;

	// We use a hash key builder for pipeline caches too :o
	_GFXHashBuilder builder;
	if (!_gfx_hash_builder(&builder)) return 0;

	// Then stick empty data in it big enough for the source.
	long long len = gfx_io_len(src);
	if (len <= 0)
	{
		gfx_log_error(
			"Zero or unknown stream length, cannot load pipeline cache.");

		free(_gfx_hash_builder_get(&builder));
		return 0;
	}

	void* bData = _gfx_hash_builder_push(&builder, (size_t)len, NULL);
	if (bData == NULL)
	{
		gfx_log_error(
			"Could not allocate buffer to load pipeline cache.");

		free(_gfx_hash_builder_get(&builder));
		return 0;
	}

	// Read cache data.
	len = gfx_io_read(src, bData, (size_t)len);
	if (len <= 0)
	{
		gfx_log_error(
			"Could not read pipeline cache from stream.");

		free(_gfx_hash_builder_get(&builder));
		return 0;
	}

	// Claim builder data & unpack the groufix header.
	_GFXHashKey* key = _gfx_hash_builder_get(&builder);
	key->len = (size_t)len; // In case of shorter read.

	_GFXPipelineCacheHeader header;
	const size_t headerSize =
		sizeof(header.magic) + sizeof(header.dataSize) +
		sizeof(header.dataHash) + sizeof(header.vendorID) +
		sizeof(header.deviceID) + sizeof(header.driverVersion) +
		sizeof(header.driverABI) + sizeof(header.uuid);

	// What's this, not even a header >:(
	if (key->len < headerSize)
	{
		gfx_log_error(
			"Could not load pipeline cache; "
			"groufix header is incomplete.");

		free(key);
		return 0;
	}

	const uint64_t emptyHash = 0;
	char* head = key->bytes;

	memcpy(&header.magic, head, sizeof(header.magic));
	head += sizeof(header.magic);
	memcpy(&header.dataSize, head, sizeof(header.dataSize));
	head += sizeof(header.dataSize);
	memcpy(&header.dataHash, head, sizeof(header.dataHash));
	// Set dataHash to 0 in the received data so we can hash & compare it :)
	memcpy(head, &emptyHash, sizeof(header.dataHash));
	head += sizeof(header.dataHash);
	memcpy(&header.vendorID, head, sizeof(header.vendorID));
	head += sizeof(header.vendorID);
	memcpy(&header.deviceID, head, sizeof(header.deviceID));
	head += sizeof(header.deviceID);
	memcpy(&header.driverVersion, head, sizeof(header.driverVersion));
	head += sizeof(header.driverVersion);
	memcpy(&header.driverABI, head, sizeof(header.driverABI));
	head += sizeof(header.driverABI);
	memcpy(header.uuid, head, sizeof(header.uuid));
	head += sizeof(header.uuid);

	// Validate the received data.
	{
		// Get allocation limit in a scope so pdp gets freed :)
		VkPhysicalDeviceProperties pdp;
		_groufix.vk.GetPhysicalDeviceProperties(cache->vk.device, &pdp);

		if (
			header.magic != _GFX_HEADER_MAGIC ||
			header.dataSize != key->len ||
			header.dataHash != _gfx_hash_murmur3(key) ||
			header.vendorID != pdp.vendorID ||
			header.deviceID != pdp.deviceID ||
			header.driverVersion != pdp.driverVersion ||
			header.driverABI != (uint32_t)sizeof(void*) ||
			memcmp(header.uuid, pdp.pipelineCacheUUID, sizeof(header.uuid)) != 0)
		{
			gfx_log_error(
				"Could not load pipeline cache; "
				"data is invalid or incompatible.");

			free(key);
			return 0;
		}
	}

	// Create a temporary Vulkan pipeline cache.
	VkPipelineCacheCreateInfo pcci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,

		.pNext           = NULL,
		.flags           = 0,
		.initialDataSize = key->len - headerSize,
		.pInitialData    = head
	};

	VkPipelineCache vkCache;
	_GFX_VK_CHECK(
		context->vk.CreatePipelineCache(
			context->vk.device, &pcci, NULL, &vkCache),
		{
			gfx_log_error("Failed to load pipeline cache.");

			free(key);
			return 0;
		});

	free(key);

	// And finally, merge the temporary pipeline & destroy it.
	bool success = 1;

	_GFX_VK_CHECK(
		context->vk.MergePipelineCaches(
			context->vk.device, cache->vk.cache, 1, &vkCache),
		{
			gfx_log_error("Failed to merge pipeline cache.");
			success = 0;
		});

	context->vk.DestroyPipelineCache(
		context->vk.device, vkCache, NULL);

	// Some victory logs c:
	if (success)
		gfx_log_info(
			"Successfully loaded groufix pipeline cache:\n"
			"    Input size: %"GFX_PRIs" bytes.\n",
			key->len);

	return success;
}

/****************************/
bool _gfx_cache_store(_GFXCache* cache, const GFXWriter* dst)
{
	assert(_groufix.vk.instance != NULL);
	assert(cache != NULL);
	assert(dst != NULL);

	_GFXContext* context = cache->context;

	// Again with the hash key builder c:
	_GFXHashBuilder builder;
	if (!_gfx_hash_builder(&builder)) return 0;

	// Create & push a groufix header, needs to be packed!
	// Given this function follows the same makeup as _gfx_cache_alloc_key,
	// we are very much going to abuse the _GFX_KEY_PUSH macro.
	const uint32_t magic = _GFX_HEADER_MAGIC;
	const uint32_t emptySize = 0;
	const uint64_t emptyHash = 0;
	const uint32_t driverABI = (uint32_t)sizeof(void*);

	_GFX_KEY_PUSH(magic);
	_GFX_KEY_PUSH(emptySize);
	_GFX_KEY_PUSH(emptyHash);

	{
		// Get allocation limit in a scope so pdp gets freed :)
		VkPhysicalDeviceProperties pdp;
		_groufix.vk.GetPhysicalDeviceProperties(cache->vk.device, &pdp);

		_GFX_KEY_PUSH(pdp.vendorID);
		_GFX_KEY_PUSH(pdp.deviceID);
		_GFX_KEY_PUSH(pdp.driverVersion);
		_GFX_KEY_PUSH(driverABI);

		if (_gfx_hash_builder_push(
			&builder, sizeof(pdp.pipelineCacheUUID), pdp.pipelineCacheUUID) == NULL)
		{
			goto clean;
		}
	}

	// Get the size of the pipeline cache.
	// Then push a big enough chunk for the cache data & get the data.
	size_t vkSize;
	_GFX_VK_CHECK(context->vk.GetPipelineCacheData(
		context->vk.device, cache->vk.cache, &vkSize, NULL), goto clean);

	void* bData = _gfx_hash_builder_push(&builder, vkSize, NULL);
	if (bData == NULL) goto clean;

	_GFX_VK_CHECK(context->vk.GetPipelineCacheData(
		context->vk.device, cache->vk.cache, &vkSize, bData), goto clean);

	// Claim builder data.
	// Set its `dataSize` so we can hash.
	_GFXHashKey* key = _gfx_hash_builder_get(&builder);
	memcpy(
		(uint32_t*)key->bytes + 1, // Right after `magic`.
		&key->len,
		sizeof(uint32_t));

	// Then hash while `dataHash` is 0 and set it afterwards
	const uint64_t hash = _gfx_hash_murmur3(key);
	memcpy(
		(uint32_t*)key->bytes + 2, // Right after `dataSize`.
		&hash,
		sizeof(uint64_t));

	// Stream out the data.
	if (gfx_io_write(dst, key->bytes, key->len) <= 0)
	{
		gfx_log_error("Could not write pipeline cache to stream.");
		free(key);
		return 0;
	}

	// Yey we did it!
	gfx_log_info(
		"Written groufix pipeline cache to stream (%"GFX_PRIs" bytes).",
		key->len);

	free(key);
	return 1;


	// Cleanup on failure.
clean:
	gfx_log_error("Failed to store pipeline cache.");

	free(_gfx_hash_builder_get(&builder));
	return 0;
}
