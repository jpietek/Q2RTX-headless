/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "device_selection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
	exit(1); \
} } while (0)

static vkpt_device_candidate_t card(unsigned int id, bool pipeline, bool query, VkDriverId driver)
{
	vkpt_device_candidate_t result = {
		.uuid = { 0x36, 0x75, 0xcb, 0x07, 0xa5, 0x28, 0x1d, 0x90,
			0x6e, 0xd0, 0x2b, 0x9c, 0x5e, 0xb8, 0x4c, 0xaf },
		.driver = driver, .ray_pipeline = pipeline, .ray_query = query,
	};
	result.uuid[0] = (uint8_t)id;
	return result;
}

static void uuid_text(const vkpt_device_candidate_t *device, char text[33])
{
	for (unsigned int i = 0; i < VK_UUID_SIZE; i++)
		snprintf(text + 2 * i, 3, "%02x", device->uuid[i]);
}

static void check_pick(const vkpt_device_candidate_t *devices, uint32_t count,
	const char *target, vkpt_rt_api_t api, int expected, bool expected_query)
{
	bool query = !expected_query;
	CHECK(vkpt_select_device(devices, count, target, api, &query) == expected);
	CHECK(query == expected_query);
}

static void test_uuid_formats(void)
{
	vkpt_device_candidate_t device = card(0x36, true, true, VK_DRIVER_ID_NVIDIA_PROPRIETARY);
	const char *valid[] = {
		"3675cb07a5281d906ed02b9c5eb84caf",
		"3675CB07A5281D906ED02B9C5EB84CAF",
		"3675cb07-a528-1d90-6ed0-2b9c5eb84caf",
		"GPU-3675cb07-a528-1d90-6ed0-2b9c5eb84caf",
		"gpu-3675CB07-A528-1D90-6ED0-2B9C5EB84CAF",
		"GPU-3675cb07a5281d906ed02b9c5eb84caf",
	};
	for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
		check_pick(&device, 1, valid[i], VKPT_RT_AUTO, 0, true);
	const char *invalid[] = {
		"G", "GP", "GPU", "GPU-", "-", "None",
		"3675cb07a5281d906ed02b9c5eb84ca",
		"3675cb07a5281d906ed02b9c5eb84caff",
		"3675cb07a5281d906ed02b9c5eb84cag",
		"3675cb07a5281d906ed02b9c5eb84caf!",
		"3675cb07_a528_1d90_6ed0_2b9c5eb84caf",
		"3675cb0-7a528-1d90-6ed0-2b9c5eb84caf",
		" 3675cb07a5281d906ed02b9c5eb84caf",
		"3675cb07a5281d906ed02b9c5eb84caf ",
	};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		check_pick(&device, 1, invalid[i], VKPT_RT_AUTO, VKPT_DEVICE_INVALID_UUID, false);
	check_pick(&device, 1, "00000000000000000000000000000000", VKPT_RT_AUTO,
		VKPT_DEVICE_UUID_NOT_FOUND, false);
}

static void test_four_identical_cards(void)
{
	/* All 24 enumeration orders, each of four physical UUIDs, all API modes. */
	for (unsigned int a = 0; a < 4; a++)
	for (unsigned int b = 0; b < 4; b++)
	for (unsigned int c = 0; c < 4; c++)
	for (unsigned int d = 0; d < 4; d++) {
		if (a == b || a == c || a == d || b == c || b == d || c == d) continue;
		unsigned int order[] = { a, b, c, d };
		vkpt_device_candidate_t devices[4];
		for (int i = 0; i < 4; i++)
			devices[i] = card(order[i], true, true, VK_DRIVER_ID_NVIDIA_PROPRIETARY);
		for (int i = 0; i < 4; i++) {
			char target[33];
			uuid_text(&devices[i], target);
			check_pick(devices, 4, target, VKPT_RT_AUTO, i, true);
			check_pick(devices, 4, target, VKPT_RT_QUERY, i, true);
			check_pick(devices, 4, target, VKPT_RT_PIPELINE, i, false);
		}
		check_pick(devices, 4, NULL, VKPT_RT_AUTO, 0, true);
		check_pick(devices, 4, "", VKPT_RT_AUTO, 0, true);
	}
}

static void test_single_card(void)
{
	/* Pinning the sole card must preserve all legacy API/fallback choices. */
	for (unsigned int caps = 0; caps < 4; caps++)
	for (int nvidia = 0; nvidia < 2; nvidia++)
	for (int api = VKPT_RT_AUTO; api <= VKPT_RT_QUERY; api++) {
		bool pipeline = (caps & 1) != 0;
		bool query = (caps & 2) != 0;
		vkpt_device_candidate_t device = card(0x36, pipeline, query,
			nvidia ? VK_DRIVER_ID_NVIDIA_PROPRIETARY : VK_DRIVER_ID_MESA_RADV);
		bool expected_query = query && (!pipeline || api == VKPT_RT_QUERY ||
			(api == VKPT_RT_AUTO && nvidia));
		int expected = caps ? 0 : VKPT_DEVICE_NO_RAY_TRACING;
		check_pick(&device, 1, NULL, (vkpt_rt_api_t)api, expected, expected_query);
		check_pick(&device, 1, "", (vkpt_rt_api_t)api, expected, expected_query);
		check_pick(&device, 1, "GPU-3675cb07-a528-1d90-6ed0-2b9c5eb84caf",
			(vkpt_rt_api_t)api, expected, expected_query);
	}
}

static void test_mixed_capabilities(void)
{
	vkpt_device_candidate_t devices[] = {
		card(0, true, true, VK_DRIVER_ID_NVIDIA_PROPRIETARY),
		card(1, true, false, VK_DRIVER_ID_MESA_RADV),
		card(2, false, true, VK_DRIVER_ID_NVIDIA_PROPRIETARY),
		card(3, false, false, VK_DRIVER_ID_MESA_RADV),
	};
	char target[33];
	uuid_text(&devices[1], target);
	check_pick(devices, 4, target, VKPT_RT_AUTO, 1, false);
	check_pick(devices, 4, target, VKPT_RT_QUERY, 1, false);
	uuid_text(&devices[2], target);
	check_pick(devices, 4, target, VKPT_RT_AUTO, 2, true);
	check_pick(devices, 4, target, VKPT_RT_PIPELINE, 2, true);
	uuid_text(&devices[3], target);
	check_pick(devices, 4, target, VKPT_RT_AUTO, VKPT_DEVICE_NO_RAY_TRACING, false);
	/* No pin retains NVIDIA's ray-query preference and first-capable fallback. */
	check_pick(devices, 4, NULL, VKPT_RT_AUTO, 0, true);
	devices[0].ray_query = false;
	check_pick(devices, 4, NULL, VKPT_RT_AUTO, 2, true);
	check_pick(devices, 4, NULL, VKPT_RT_PIPELINE, 0, false);
	devices[2].driver = VK_DRIVER_ID_MESA_RADV;
	check_pick(devices, 4, NULL, VKPT_RT_AUTO, 0, false);
	check_pick(devices, 4, NULL, VKPT_RT_QUERY, 2, true);
	check_pick(devices, 0, NULL, VKPT_RT_AUTO, VKPT_DEVICE_NO_RAY_TRACING, false);
	check_pick(devices, 0, target, VKPT_RT_AUTO, VKPT_DEVICE_UUID_NOT_FOUND, false);
}

int main(void)
{
	test_uuid_formats();
	test_four_identical_cards();
	test_single_card();
	test_mixed_capabilities();
	puts("Device selection: UUID formats, four-card permutations, single-card parity and mixed capabilities passed.");
	return 0;
}
