/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VKPT_DEVICE_SELECTION_H
#define VKPT_DEVICE_SELECTION_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
	uint8_t uuid[VK_UUID_SIZE];
	VkDriverId driver;
	bool ray_pipeline;
	bool ray_query;
} vkpt_device_candidate_t;

typedef enum {
	VKPT_RT_AUTO,
	VKPT_RT_PIPELINE,
	VKPT_RT_QUERY
} vkpt_rt_api_t;

enum {
	VKPT_DEVICE_NO_RAY_TRACING = -1,
	VKPT_DEVICE_INVALID_UUID = -2,
	VKPT_DEVICE_UUID_NOT_FOUND = -3
};

/* Empty target preserves automatic selection. An explicit target never falls
 * back to a different physical device. Returns a device index or error above. */
int vkpt_select_device(const vkpt_device_candidate_t *devices, uint32_t count,
	const char *target_uuid, vkpt_rt_api_t requested_api, bool *use_ray_query);

#endif
