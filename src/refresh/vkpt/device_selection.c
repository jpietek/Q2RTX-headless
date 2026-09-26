/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "device_selection.h"

#include <string.h>

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool parse_uuid(const char *text, uint8_t uuid[VK_UUID_SIZE])
{
	/* Accept compact Vulkan UUIDs and the canonical NVML GPU-UUID form. */
	if (strlen(text) >= 4 &&
		(text[0] == 'G' || text[0] == 'g') &&
		(text[1] == 'P' || text[1] == 'p') &&
		(text[2] == 'U' || text[2] == 'u') && text[3] == '-')
		text += 4;

	size_t length = strlen(text);
	if (length != 32 && length != 36)
		return false;

	for (unsigned int byte = 0; byte < VK_UUID_SIZE; byte++) {
		if (length == 36 && (byte == 4 || byte == 6 || byte == 8 || byte == 10)) {
			if (*text++ != '-') return false;
		}
		int high = hex_digit(*text++);
		int low = hex_digit(*text++);
		if (high < 0 || low < 0) return false;
		uuid[byte] = (uint8_t)((high << 4) | low);
	}
	return true;
}

int vkpt_select_device(const vkpt_device_candidate_t *devices, uint32_t count,
	const char *target_uuid, vkpt_rt_api_t requested_api, bool *use_ray_query)
{
	bool targeted = target_uuid && *target_uuid;
	uint8_t uuid[VK_UUID_SIZE];
	*use_ray_query = false;
	if (targeted && !parse_uuid(target_uuid, uuid))
		return VKPT_DEVICE_INVALID_UUID;

	int pipeline = -1;
	int query = -1;
	VkDriverId query_driver = VK_DRIVER_ID_MAX_ENUM;
	bool matched = false;
	for (uint32_t i = 0; i < count; i++) {
		if (targeted && memcmp(devices[i].uuid, uuid, VK_UUID_SIZE))
			continue;
		matched = true;
		if (devices[i].ray_pipeline && pipeline < 0)
			pipeline = (int)i;
		if (devices[i].ray_query && query < 0) {
			query = (int)i;
			query_driver = devices[i].driver;
		}
	}
	if (targeted && !matched)
		return VKPT_DEVICE_UUID_NOT_FOUND;

	/* Preserve the renderer's existing API preference and fallback order, but
	 * consider only the selected GPU when a UUID was supplied. */
	if (requested_api == VKPT_RT_QUERY && query >= 0) {
		*use_ray_query = true;
		return query;
	}
	if (requested_api == VKPT_RT_PIPELINE && pipeline >= 0)
		return pipeline;
	if (query_driver == VK_DRIVER_ID_NVIDIA_PROPRIETARY && query >= 0) {
		*use_ray_query = true;
		return query;
	}
	if (pipeline >= 0)
		return pipeline;
	if (query >= 0) {
		*use_ray_query = true;
		return query;
	}
	return VKPT_DEVICE_NO_RAY_TRACING;
}
