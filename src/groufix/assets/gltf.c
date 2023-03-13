/**
 * This file is part of groufix.
 * Copyright (c) Stef Velzel. All rights reserved.
 *
 * groufix : graphics engine produced by Stef Velzel.
 * www     : <www.vuzzel.nl>
 */

#include "groufix/assets/gltf.h"
#include "groufix/containers/vec.h"
#include "groufix/core/log.h"
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"


#define _GFX_GET_GLTF_ERROR_STRING(result) \
	((result) == cgltf_result_success ? \
		"success" : \
	(result) == cgltf_result_data_too_short ? \
		"data too short" : \
	(result) == cgltf_result_unknown_format ? \
		"unknown format" : \
	(result) == cgltf_result_invalid_json ? \
		"invalid JSON" : \
	(result) == cgltf_result_invalid_gltf ? \
		"invalid glTF" : \
	(result) == cgltf_result_out_of_memory ? \
		"out of memory" : \
	(result) == cgltf_result_legacy_gltf ? \
		"legacy glTF" : \
		"unknown error")

#define _GFX_GET_GLTF_TOPOLOGY(topo) \
	((topo) == cgltf_primitive_type_points ? \
		GFX_TOPO_POINT_LIST : \
	(topo) == cgltf_primitive_type_lines ? \
		GFX_TOPO_LINE_LIST : \
	(topo) == cgltf_primitive_type_line_loop ? \
		GFX_TOPO_LINE_STRIP : \
	(topo) == cgltf_primitive_type_line_strip ? \
		GFX_TOPO_LINE_STRIP : \
	(topo) == cgltf_primitive_type_triangles ? \
		GFX_TOPO_TRIANGLE_LIST : \
	(topo) == cgltf_primitive_type_triangle_strip ? \
		GFX_TOPO_TRIANGLE_STRIP : \
	(topo) == cgltf_primitive_type_triangle_fan ? \
		GFX_TOPO_TRIANGLE_FAN : \
		GFX_TOPO_TRIANGLE_LIST)

#define _GFX_GET_GLTF_INDEX_SIZE(type) \
	((type) == cgltf_component_type_r_16u ? sizeof(uint16_t) : \
	(type) == cgltf_component_type_r_32u ? sizeof(uint32_t) : 0)

#define _GFX_GET_GLTF_MIN_FILTER(minFilter) \
	((minFilter) == 0x2600 ? GFX_FILTER_NEAREST : \
	(minFilter) == 0x2601 ? GFX_FILTER_LINEAR : GFX_FILTER_NEAREST)

#define _GFX_GET_GLTF_MAG_FILTER(magFilter) \
	((magFilter) == 0x2600 ? GFX_FILTER_NEAREST : \
	(magFilter) == 0x2601 ? GFX_FILTER_LINEAR : \
	(magFilter) == 0x2700 ? GFX_FILTER_NEAREST : \
	(magFilter) == 0x2701 ? GFX_FILTER_LINEAR : \
	(magFilter) == 0x2702 ? GFX_FILTER_NEAREST : \
	(magFilter) == 0x2703 ? GFX_FILTER_LINEAR : GFX_FILTER_NEAREST)

#define _GFX_GET_GLTF_MIP_FILTER(minFilter) \
	((minFilter) == 0x2600 ? GFX_FILTER_NEAREST : \
	(minFilter) == 0x2601 ? GFX_FILTER_NEAREST : \
	(minFilter) == 0x2700 ? GFX_FILTER_NEAREST : \
	(minFilter) == 0x2701 ? GFX_FILTER_NEAREST : \
	(minFilter) == 0x2702 ? GFX_FILTER_LINEAR : \
	(minFilter) == 0x2703 ? GFX_FILTER_LINEAR : GFX_FILTER_NEAREST)

#define _GFX_GET_GLTF_WRAPPING(wrap) \
	((wrap) == 0x2901 ? GFX_WRAP_REPEAT : \
	(wrap) == 0x8370 ? GFX_WRAP_REPEAT_MIRROR : \
	(wrap) == 0x812f ? GFX_WRAP_CLAMP_TO_EDGE : \
	(wrap) == 0x8743 ? GFX_WRAP_CLAMP_TO_EDGE_MIRROR : \
	(wrap) == 0x812d ? GFX_WRAP_CLAMP_TO_BORDER : GFX_WRAP_REPEAT)


// Decode a hexadecimal digit.
#define _GFX_UNHEX(digit) \
	(unsigned char)( \
		(unsigned)(digit - '0') < 10 ? (digit - '0') : \
		(unsigned)(digit - 'A') < 6 ? (digit - 'A') + 10 : \
		(unsigned)(digit - 'a') < 6 ? (digit - 'a') + 10 : \
		UCHAR_MAX)


/****************************
 * Compares (case insensitive) two NULL-terminated strings.
 * One of the strings may terminate with '_', its remains will be ignored.
 */
static bool _gfx_gltf_cmp_attributes(const char* l, const char* r)
{
	if (l == NULL || r == NULL)
		return 0;

	while (*l != '\0' && *r != '\0')
		if (tolower(*(l++)) != tolower(*(r++)))
			return 0;

	return *l == *r || *l == '_' || *r == '_';
}

/****************************
 * Constructs a vertex attribute format from the glTF accessor type,
 * component type and normalized flag.
 */
static GFXFormat _gfx_gltf_attribute_fmt(cgltf_component_type cType,
                                         cgltf_type type,
                                         cgltf_bool normalized)
{
	// Compute #components and their properties.
	const size_t comps =
		type == cgltf_type_scalar ? 1 :
		type == cgltf_type_vec2 ? 2 :
		type == cgltf_type_vec3 ? 3 :
		type == cgltf_type_vec4 ? 4 : 0;

	const unsigned char depth =
		cType == cgltf_component_type_r_8 ? 8 :
		cType == cgltf_component_type_r_8u ? 8 :
		cType == cgltf_component_type_r_16 ? 16 :
		cType == cgltf_component_type_r_16u ? 16 :
		cType == cgltf_component_type_r_32u ? 32 :
		cType == cgltf_component_type_r_32f ? 32 : 0;

	const GFXFormatType fType =
		// Signed integer.
		(cType == cgltf_component_type_r_8 ||
		cType == cgltf_component_type_r_16) ?
			(normalized ? GFX_SNORM : GFX_SSCALED) :
		// Unsigned integer.
		(cType == cgltf_component_type_r_8u ||
		cType == cgltf_component_type_r_16u ||
		cType == cgltf_component_type_r_32u) ?
			(normalized ? GFX_UNORM : GFX_USCALED) :
		// Floating point.
		cType == cgltf_component_type_r_32f ?
			GFX_SFLOAT : 0;

	const GFXOrder order =
		type == cgltf_type_scalar ? GFX_ORDER_R :
		type == cgltf_type_vec2 ? GFX_ORDER_RG :
		type == cgltf_type_vec3 ? GFX_ORDER_RGB :
		type == cgltf_type_vec4 ? GFX_ORDER_RGBA : 0;

	return (GFXFormat){
		{
			comps > 0 ? depth : 0,
			comps > 1 ? depth : 0,
			comps > 2 ? depth : 0,
			comps > 3 ? depth : 0
		},
		fType,
		order
	};
}

/****************************
 * Decodes an encoded URI into a newly allocated string.
 * @return Must call free() on success!
 */
static char* _gfx_gltf_decode_uri(const char* uri)
{
	const size_t len = strlen(uri);

	// Make a copy URI.
	char* buf = malloc(len + 1);
	if (buf == NULL) return NULL;

	strcpy(buf, uri);

	// Decode all %-encodings inline.
	char* w = buf;
	char* i = buf;

	while (*i != '\0')
	{
		if (*i == '%')
		{
			unsigned char c0 = _GFX_UNHEX(i[1]);
			if (c0 < 16)
			{
				unsigned char c1 = _GFX_UNHEX(i[2]);
				if (c1 < 16)
				{
					*(w++) = (char)(c0 * 16 + c1);
					i += 3;
					continue;
				}
			}
		}

		*(w++) = *(i++);
	}

	*w = '\0';
	return buf;
}

/****************************
 * Decodes a base64 string into a newly allocated binary buffer.
 * @param size Size of the output buffer (_NOT_ of src) in bytes, fails if 0.
 * @return Must call free() on success!
 */
static void* _gfx_gltf_decode_base64(size_t size, const char* src)
{
	if (size == 0) return NULL; // Empty is explicit error.

	unsigned char* bin = malloc(size);
	if (bin == NULL) return NULL;

	unsigned int buff = 0;
	unsigned int bits = 0;

	// Loop over all output bytes.
	for (size_t i = 0; i < size; ++i)
	{
		// Loop over base64 digits until we have at least 8 bits.
		while (bits < 8)
		{
			char digit = *(src++);
			unsigned char index = (unsigned char)(
				(unsigned)(digit - 'A') < 26 ? (digit - 'A') :
				(unsigned)(digit - 'a') < 26 ? (digit - 'a') + 26 :
				(unsigned)(digit - '0') < 10 ? (digit - '0') + 52 :
				(digit == '+') ? 62 :
				(digit == '/') ? 63 :
				UCHAR_MAX);

			if (index > 63)
			{
				free(bin);
				return NULL;
			}

			buff = (buff << 6) | index;
			bits += 6;
		}

		// Output the oldest 8 bits.
		bits -= 8;
		bin[i] = (unsigned char)(buff >> bits);
	}

	return bin;
}

/****************************
 * Allocates a new buffer and fills it with given data.
 * @param size Must be > 0.
 * @return NULL on failure.
 */
static GFXBuffer* _gfx_gltf_alloc_buffer(GFXHeap* heap, GFXDependency* dep,
                                         size_t size, const void* bin)
{
	assert(heap != NULL);
	assert(dep != NULL);
	assert(size > 0);
	assert(bin != NULL);

	// Allocate.
	GFXBuffer* buffer = gfx_alloc_buffer(heap,
		GFX_MEMORY_WRITE,
		GFX_BUFFER_VERTEX | GFX_BUFFER_INDEX,
		size);

	if (buffer == NULL) return NULL;

	// Write data.
	const GFXRegion region = {
		.offset = 0,
		.size = size
	};

	const GFXInject inject =
		gfx_dep_sig(dep,
			GFX_ACCESS_VERTEX_READ | GFX_ACCESS_INDEX_READ, GFX_STAGE_ANY);

	if (!gfx_write(bin, gfx_ref_buffer(buffer),
		GFX_TRANSFER_ASYNC,
		1, 1, &region, &region, &inject))
	{
		gfx_free_buffer(buffer);
		return NULL;
	}

	return buffer;
}

/****************************
 * Resolves and reads a buffer URI.
 * @param inc  Includer to use, may be NULL.
 * @param uri  Data URI to resolve, cannot be NULL, must be NULL-terminated.
 * @return NULL on failure.
 */
static GFXBuffer* _gfx_gltf_include_buffer(const GFXIncluder* inc, const char* uri,
                                           GFXHeap* heap, GFXDependency* dep)
{
	assert(uri != NULL);
	assert(heap != NULL);
	assert(dep != NULL);

	// Cannot do anything without an includer.
	if (inc == NULL)
	{
		gfx_log_error("Cannot load buffer URIs without an includer.");
		return NULL;
	}

	// Resolve the URI.
	char* dec = _gfx_gltf_decode_uri(uri);
	if (dec == NULL)
	{
		gfx_log_error("Could not decode buffer URI: %s.", uri);
		return NULL;
	}

	const GFXReader* src = gfx_io_resolve(inc, dec);
	free(dec); // Immediately free.

	if (src == NULL)
	{
		gfx_log_error("Could not resolve buffer URI: %s.", uri);
		return NULL;
	}

	// Allocate binary buffer.
	long long len = gfx_io_len(src);
	if (len <= 0)
	{
		gfx_log_error(
			"Zero or unknown stream length, cannot load URI: %s.", uri);

		gfx_io_release(inc, src);
		return NULL;
	}

	void* bin = malloc((size_t)len);
	if (bin == NULL)
	{
		gfx_log_error(
			"Could not allocate buffer to load URI: %s.", uri);

		gfx_io_release(inc, src);
		return NULL;
	}

	// Read source.
	len = gfx_io_read(src, bin, (size_t)len);
	if (len <= 0)
	{
		gfx_log_error(
			"Could not read data from stream to load URI: %s.", uri);

		gfx_io_release(inc, src);
		free(bin);
		return NULL;
	}

	// Release the stream & allocate buffer.
	gfx_io_release(inc, src);

	GFXBuffer* buffer = _gfx_gltf_alloc_buffer(heap, dep, (size_t)len, bin);
	if (buffer == NULL)
		gfx_log_error("Failed to load buffer URI: %s", uri);

	// Free memory & output.
	free(bin);

	return buffer;
}

/****************************
 * Resolves and reads an image URI.
 * @param inc Includer to use, may be NULL.
 * @param uri Data URI to resolve, cannot be NULL, must be NULL-terminated.
 * @return NULL on failure.
 */
static GFXImage* _gfx_gltf_include_image(const GFXIncluder* inc, const char* uri,
                                         GFXHeap* heap, GFXDependency* dep,
                                         GFXImageFlags flags, GFXImageUsage usage)
{
	assert(uri != NULL);
	assert(heap != NULL);
	assert(dep != NULL);

	// Cannot do anything without an includer.
	if (inc == NULL)
	{
		gfx_log_error("Cannot load image URIs without an includer.");
		return NULL;
	}

	// Resolve the URI.
	char* dec = _gfx_gltf_decode_uri(uri);
	if (dec == NULL)
	{
		gfx_log_error("Could not decode image URI: %s.", uri);
		return NULL;
	}

	const GFXReader* src = gfx_io_resolve(inc, dec);
	free(dec); // Immediately free.

	if (src == NULL)
	{
		gfx_log_error("Could not resolve image URI: %s.", uri);
		return NULL;
	}

	// Simply load the image.
	GFXImage* image = gfx_load_image(heap, dep, flags, usage, src);
	if (image == NULL)
		gfx_log_error("Failed to load image URI: %s", uri);

	// Release the stream & output.
	gfx_io_release(inc, src);

	return image;
}

/****************************/
GFX_API bool gfx_load_gltf(GFXHeap* heap, GFXDependency* dep,
                           const GFXGltfOptions* options,
                           GFXImageFlags flags, GFXImageUsage usage,
                           const GFXReader* src,
                           const GFXIncluder* inc,
                           GFXGltfResult* result)
{
	assert(heap != NULL);
	assert(dep != NULL);
	assert(options == NULL || options->orderSize == 0 || options->attributeOrder != NULL);
	assert(src != NULL);
	assert(result != NULL);

	// Allocate source buffer.
	long long len = gfx_io_len(src);
	if (len <= 0)
	{
		gfx_log_error(
			"Zero or unknown stream length, cannot load glTF source.");

		return 0;
	}

	void* source = malloc((size_t)len);
	if (source == NULL)
	{
		gfx_log_error(
			"Could not allocate source buffer to load glTF source.");

		return 0;
	}

	// Read source.
	len = gfx_io_read(src, source, (size_t)len);
	if (len <= 0)
	{
		gfx_log_error(
			"Could not read glTF source from stream.");

		free(source);
		return 0;
	}

	// Parse the glTF source.
	cgltf_options opts = {0};
	cgltf_data* data = NULL;

	cgltf_result res = cgltf_parse(&opts, source, (size_t)len, &data);
	free(source); // Immediately free source buffer.

	// Some extra validation.
	if (res == cgltf_result_success) res = cgltf_validate(data);
	else data = NULL; // So we do not free garbage.

	// Fail on error.
	if (res != cgltf_result_success)
	{
		gfx_log_error(
			"Failed to load glTF, %s.",
			_GFX_GET_GLTF_ERROR_STRING(res));

		cgltf_free(data);
		return 0;
	}

	// Setup some output vectors.
	// We are going to fill them with groufix equivalents of the glTF.
	// From this point onwards we need to clean on failure.
	GFXVec buffers;
	GFXVec images;
	GFXVec samplers;
	GFXVec materials;
	GFXVec primitives;
	GFXVec meshes;
	gfx_vec_init(&buffers, sizeof(GFXBuffer*));
	gfx_vec_init(&images, sizeof(GFXImage*));
	gfx_vec_init(&samplers, sizeof(GFXSampler));
	gfx_vec_init(&materials, sizeof(GFXGltfMaterial));
	gfx_vec_init(&primitives, sizeof(GFXGltfPrimitive));
	gfx_vec_init(&meshes, sizeof(GFXGltfMesh));
	gfx_vec_reserve(&buffers, data->buffers_count);
	gfx_vec_reserve(&images, data->images_count);
	gfx_vec_reserve(&samplers, data->samplers_count);
	gfx_vec_reserve(&materials, data->materials_count);
	gfx_vec_reserve(&meshes, data->meshes_count);

	// Create all buffers.
	for (size_t b = 0; b < data->buffers_count; ++b)
	{
		GFXBuffer* buffer = NULL;
		const char* uri = data->buffers[b].uri;

		// Check if data URI.
		if (uri != NULL && strncmp(uri, "data:", 5) == 0)
		{
			const char* comma = strchr(uri, ',');

			// Check if base64.
			if (comma == NULL || comma - uri < 7 ||
				strncmp(comma - 7, ";base64", 7) != 0)
			{
				gfx_log_error("Data URIs can only be base64.");
				goto clean;
			}

			// Decode base64.
			void* bin = _gfx_gltf_decode_base64(
				data->buffers[b].size, comma + 1);

			if (bin == NULL)
			{
				gfx_log_error("Failed to decode base64 data URI.");
				goto clean;
			}

			// Allocate buffer.
			buffer = _gfx_gltf_alloc_buffer(
				heap, dep, data->buffers[b].size, bin);

			free(bin);
			if (buffer == NULL) goto clean;
		}

		// Check if actual URI.
		else if (uri != NULL)
		{
			buffer = _gfx_gltf_include_buffer(inc, uri, heap, dep);
			if (buffer == NULL) goto clean;
		}

		// Insert the buffer.
		if (!gfx_vec_push(&buffers, 1, &buffer))
		{
			gfx_free_buffer(buffer);
			goto clean;
		}
	}

	// Create all images.
	for (size_t i = 0; i < data->images_count; ++i)
	{
		GFXImage* image = NULL;
		const char* uri = data->images[i].uri;

		// Check if data URI.
		if (uri != NULL && strncmp(uri, "data:", 5) == 0)
		{
			gfx_log_error("Data URIs are not allowed for images.");
			goto clean;
		}

		// Check if actual URI.
		else if (uri != NULL)
		{
			image = _gfx_gltf_include_image(inc, uri, heap, dep, flags, usage);
			if (image == NULL) goto clean;
		}

		// Insert the image.
		if (!gfx_vec_push(&images, 1, &image))
		{
			gfx_free_image(image);
			goto clean;
		}
	}

	// Create all samplers.
	for (size_t s = 0; s < data->samplers_count; ++s)
	{
		// Insert sampler.
		GFXSampler sampler = {
			.binding = 0,
			.index = 0,

			.flags = GFX_SAMPLER_NONE,
			.mode = GFX_FILTER_MODE_AVERAGE,

			.minFilter = _GFX_GET_GLTF_MIN_FILTER(data->samplers[s].min_filter),
			.magFilter = _GFX_GET_GLTF_MAG_FILTER(data->samplers[s].mag_filter),
			.mipFilter = _GFX_GET_GLTF_MIP_FILTER(data->samplers[s].min_filter),

			.wrapU = _GFX_GET_GLTF_WRAPPING(data->samplers[s].wrap_s),
			.wrapV = _GFX_GET_GLTF_WRAPPING(data->samplers[s].wrap_t),
			.wrapW = GFX_WRAP_REPEAT,

			.mipLodBias = 0.0f,
			.minLod = 0.0f,
			.maxLod = 1.0f,
			.maxAnisotropy = 1.0f,

			.cmp = GFX_CMP_ALWAYS
		};

		if (!gfx_vec_push(&samplers, 1, &sampler))
			goto clean;
	}

	// Create all meshes and primitives.
	for (size_t m = 0; m < data->meshes_count; ++m)
	{
		// Insert mesh.
		GFXGltfMesh mesh = {
			.firstPrimitive = primitives.size,
			.numPrimitives = data->meshes[m].primitives_count
		};

		if (!gfx_vec_push(&meshes, 1, &mesh))
			goto clean;

		for (size_t p = 0; p < data->meshes[m].primitives_count; ++p)
		{
			// Gather all primitive data.
			const cgltf_primitive* cprim = &data->meshes[m].primitives[p];

			const size_t numIndices =
				cprim->indices != NULL ? cprim->indices->count : 0;
			const char indexSize =
				cprim->indices != NULL ?
				_GFX_GET_GLTF_INDEX_SIZE(cprim->indices->component_type) : 0;

			GFXBuffer* indexBuffer =
				cprim->indices != NULL ?
				*(GFXBuffer**)gfx_vec_at(&buffers,
					// Get index into the buffers array.
					(size_t)(cprim->indices->buffer_view->buffer -
						data->buffers)) : NULL;

			if (numIndices > 0 && indexSize == 0)
			{
				gfx_log_error("Index accessors must be sizeof(uint16_t|uint32_t).");
				goto clean;
			}

			if (cprim->attributes_count == 0)
			{
				gfx_log_error("Primitives must have attributes.");
				goto clean;
			}

			// Find actual number of attributes to consume & consume them.
			const size_t numAttributes =
				options != NULL && options->maxAttributes > 0 ?
					GFX_MIN(options->maxAttributes, cprim->attributes_count) :
					cprim->attributes_count;

			size_t numVertices = SIZE_MAX;
			GFXAttribute attributes[numAttributes];

			// Here we consider that attributes are named in glTF,
			// so they may not always appear in the same order in a file.
			// If we have the attributeOrder option, use it to reorder.
			size_t attribOrder[numAttributes];

			if (options == NULL || options->orderSize == 0)
				// Keep order if no options are given.
				for (size_t a = 0; a < numAttributes; ++a)
					attribOrder[a] = a;
			else
			{
				// Keep track of used glTF attributes.
				bool attribUsed[cprim->attributes_count];
				for (size_t ca = 0; ca < cprim->attributes_count; ++ca)
					attribUsed[ca] = 0;

				// Go over all given attribute order names (in order).
				size_t a = 0;
				for (size_t o = 0;
					o < options->orderSize && a < numAttributes; ++o)
				{
					// See if they match any glTF attributes.
					for (size_t ca = 0; ca < cprim->attributes_count; ++ca)
						if (!attribUsed[ca] && _gfx_gltf_cmp_attributes(
							options->attributeOrder[o],
							cprim->attributes[ca].name))
						{
							attribOrder[a++] = ca;
							attribUsed[ca] = 1;
							break;
						}
				}

				// Fill in the rest with remaining unused attributes.
				for (size_t ca = 0;
					ca < cprim->attributes_count && a < numAttributes; ++ca)
				{
					if (!attribUsed[ca])
						attribOrder[a++] = ca;
				}
			}

			// Fill attribute data.
			for (size_t a = 0; a < numAttributes; ++a)
			{
				const cgltf_attribute* cattr =
					&cprim->attributes[attribOrder[a]];

				numVertices = GFX_MIN(
					numVertices, cattr->data->count);

				GFXBuffer* buffer = *(GFXBuffer**)gfx_vec_at(&buffers,
					// Get index into the buffer array.
					(size_t)(cattr->data->buffer_view->buffer - data->buffers));

				attributes[a] = (GFXAttribute){
					.offset = (uint32_t)cattr->data->offset,
					.rate = GFX_RATE_VERTEX,

					.format = _gfx_gltf_attribute_fmt(
						cattr->data->component_type,
						cattr->data->type,
						cattr->data->normalized),

					.stride = (uint32_t)(
						cattr->data->buffer_view->stride == 0 ?
							cattr->data->stride :
							cattr->data->buffer_view->stride),

					.buffer = buffer != NULL ?
						gfx_ref_buffer_at(
							buffer, cattr->data->buffer_view->offset) :
						GFX_REF_NULL
				};
			}

			if (numVertices == 0)
			{
				gfx_log_error("Primitives must have vertices.");
				goto clean;
			}

			// Allocate primitive.
			GFXPrimitive* prim = gfx_alloc_prim(heap,
				0, 0, _GFX_GET_GLTF_TOPOLOGY(cprim->type),
				(uint32_t)numIndices, indexSize,
				(uint32_t)numVertices,
				indexBuffer != NULL ?
					gfx_ref_buffer_at(
						indexBuffer, cprim->indices->buffer_view->offset) :
					GFX_REF_NULL,
				numAttributes, attributes);

			if (prim == NULL) goto clean;

			// Insert primitive.
			GFXGltfPrimitive primitive = {
				.primitive = prim,
				// Get index into the material array.
				.material = cprim->material != NULL ?
					(size_t)(cprim->material - data->materials) : SIZE_MAX
			};

			if (!gfx_vec_push(&primitives, 1, &primitive))
			{
				gfx_free_prim(prim);
				goto clean;
			}
		}
	}

	// We are done building groufix objects, free gltf things.
	cgltf_free(data);

	// Claim all data and return.
	result->numBuffers = buffers.size;
	result->buffers = gfx_vec_claim(&buffers);

	result->numImages = images.size;
	result->images = gfx_vec_claim(&images);

	result->numSamplers = samplers.size;
	result->samplers = gfx_vec_claim(&samplers);

	result->numMaterials = materials.size;
	result->materials = gfx_vec_claim(&materials);

	result->numPrimitives = primitives.size;
	result->primitives = gfx_vec_claim(&primitives);

	result->numMeshes = meshes.size;
	result->meshes = gfx_vec_claim(&meshes);

	return 1;


	// Cleanup on failure.
clean:
	// Flush & block the heap so all memory transfers have been completed
	// and no command buffers reference the resources anymore!
	gfx_heap_flush(heap);
	gfx_heap_block(heap);

	for (size_t b = 0; b < buffers.size; ++b)
		gfx_free_buffer(*(GFXBuffer**)gfx_vec_at(&buffers, b));

	for (size_t i = 0; i < images.size; ++i)
		gfx_free_image(*(GFXImage**)gfx_vec_at(&images, i));

	for (size_t p = 0; p < primitives.size; ++p)
		gfx_free_prim(((GFXGltfPrimitive*)gfx_vec_at(&primitives, p))->primitive);

	gfx_vec_clear(&buffers);
	gfx_vec_clear(&images);
	gfx_vec_clear(&samplers);
	gfx_vec_clear(&materials);
	gfx_vec_clear(&primitives);
	gfx_vec_clear(&meshes);
	cgltf_free(data);

	gfx_log_error("Failed to load glTF from stream.");

	return 0;
}

/****************************/
GFX_API void gfx_release_gltf(GFXGltfResult* result)
{
	assert(result != NULL);

	free(result->buffers);
	free(result->images);
	free(result->samplers);
	free(result->materials);
	free(result->primitives);
	free(result->meshes);

	// Leave all values, result is invalidated.
}
