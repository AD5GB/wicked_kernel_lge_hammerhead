/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"VPU, %s: " fmt, __func__

#include <linux/videodev2.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ctrls.h>
#include <linux/stddef.h>

#include "vpu_configuration.h"
#include "vpu_v4l2.h"
#include "vpu_ioctl_internal.h"
#include "vpu_translate.h"
#include "vpu_property.h"
#include "vpu_channel.h"


/*
 * Private controls support
 */
enum vpu_ctrl_type {
	CTRL_TYPE_NONE = 0,
	CTRL_TYPE_VALUE,
	CTRL_TYPE_STANDARD,
	CTRL_TYPE_AUTO_MANUAL,
	CTRL_TYPE_TWO_VALUE,
};

struct vpu_ctrl_desc; /* forward declaration */

#define call_op(d, op, args...) (((d)->op) ? ((d)->op(args)) : 0)
typedef int (*get_custom_size)(const void *api_arg);
typedef int (*bounds_check)(const struct vpu_ctrl_desc *desc,
		const void *api_arg);
typedef void (*translate_to_hfi) (const void *api_data, void *hfi_data);
typedef void (*translate_to_api) (const void *hfi_data, void *api_data);
typedef int (*custom_set_function)(struct vpu_dev_session *session,
		const void *api_arg);
typedef int (*custom_get_function)(struct vpu_dev_session *session,
		void *ret_arg);

struct ctrl_bounds {
	s32 min;
	s32 max;
	s32 def;
	u32 step;
};

/**
 * struct vpu_ctrl_desc - describes the operation of of VPU controls
 * @name: [Optional] Control name used for logging purposes.
 * @type: Can be set if control is a generic type to avoid setting other fields.
 *		If set, then size, hfi_size, bounds_check, translate_to_hfi,
 *		and translate_to_api are automatically set accordingly.
 * @hfi_id: [Required] Identifies corresponding HFI property ID.
 *		If value is zero, then control is not sent to HFI.
 * @size: [Required] Size of API struct used with this control.
 * @hfi_size: [Required] Size of data sent to HFI. Auto set if type is valid.
 * @cached: Set to 1 if control cannot be read from HFI and needs to be
 *		returned from cache instead.
 * @cache_offset: [Don't set]. Calculated offset where control cache is stored.
 * @bounds: [Optional] Set if control has bounds.
 * @bounds2: [Optional] A second set of bounds if needed.
 * @custom_size: Custom function to calculate hfi_size if it is dynamic.
 * @bounds_check: [Optional] Function that performs the bound checking.
 *		Auto set if type is valid.
 * @translate_to_hfi: [Required for set control]. Translates from API to HFI.
 *		Auto set if type is valid.
 * @translate_to_api: [Required for get control]. Translates from HFI to API.
 *		Auto set if type is valid.
 * @set_function: [Optional] Custom control set function.
 * @get_function: [Optional] Custom control get function.
 */
struct vpu_ctrl_desc {
	const char *name;
	enum vpu_ctrl_type type;
	u32 hfi_id;
	u32 size;
	u32 hfi_size;
	u32 cached;
	u32 cache_offset;
	struct ctrl_bounds bounds;
	struct ctrl_bounds bounds2;
	/* function pointers */
	get_custom_size custom_size;
	bounds_check bounds_check;
	translate_to_hfi translate_to_hfi;
	translate_to_api translate_to_api;
	custom_set_function set_function;
	custom_get_function get_function;
};

static int __check_bounds(const struct ctrl_bounds *bounds, s32 val)
{
	if (!bounds->min && !bounds->max && !bounds->step)
		return 0; /* no bounds specified */

	if (val < bounds->min || val > bounds->max || !bounds->step)
		return -EINVAL;

	val = val - bounds->min;
	if (((val / bounds->step) * bounds->step) != val)
		return -EINVAL;

	return 0;
}

static int value_bounds_check(const struct vpu_ctrl_desc *desc,
		const void *api_arg)
{
	const __s32 *value = api_arg;

	return __check_bounds(&desc->bounds, *value);
}

static int standard_bounds_check(const struct vpu_ctrl_desc *desc,
		const void *api_arg)
{
	const struct vpu_ctrl_standard *arg = api_arg;

	if (arg->enable)
		return __check_bounds(&desc->bounds, arg->value);
	else
		return 0; /* ctrl not enabled, no need to check value */
}

static int auto_manual_bounds_check(const struct vpu_ctrl_desc *desc,
		const void *api_arg)
{
	const struct vpu_ctrl_auto_manual *arg = api_arg;
	const struct ctrl_bounds *bounds;

	if (arg->enable && arg->auto_mode)
		bounds = &desc->bounds;
	else if (arg->enable && !arg->auto_mode)
		bounds = &desc->bounds2;
	else
		return 0; /* ctrl not enabled, no need to check value */

	return __check_bounds(bounds, arg->value);
}

static int two_value_bounds_check(const struct vpu_ctrl_desc *desc,
		const void *api_arg)
{
	const struct vpu_ctrl_two_value *arg = api_arg;
	int ret;

	ret = __check_bounds(&desc->bounds, arg->value1);
	ret |= __check_bounds(&desc->bounds2, arg->value2);
	return ret;
}

int configure_nr_buffers(struct vpu_dev_session *session,
		const struct vpu_ctrl_auto_manual *nr)
{
	struct vpu_controller *controller = session->controller;
	struct v4l2_pix_format_mplane *in_fmt;
	const struct vpu_format_desc *vpu_format;
	u32 buf_size = 0;
	int ret = 0, i, new_bufs = 0;

	if (nr->enable) {
		/* Get NR buffer size. NR buffers are YUV422 format */
		vpu_format = query_supported_formats(PIXEL_FORMAT_YUYV);
		in_fmt = &session->port_info[INPUT_PORT].format;

		if (!in_fmt->width || !in_fmt->height)
			return 0; /* input resolution not configured yet */

		buf_size = get_bytesperline(in_fmt->width,
				vpu_format->plane[0].bitsperpixel, 0);
		buf_size *= in_fmt->height;

		for (i = 0; i < NUM_NR_BUFFERS; i++) {
			if (vpu_mem_size(controller->nr_buffers[i]) >= buf_size)
				continue;

			ret = vpu_mem_alloc(controller->nr_buffers[i], buf_size,
				session->port_info[INPUT_PORT].secure_content);
			if (ret) {
				pr_err("Failed allocate NR buffers size (%d)\n",
						buf_size);
				goto exit_nr_bufs;
			}
			new_bufs = 1;
		}
		if (!new_bufs)
			return 0; /* no new buffers */

		/* send buffers to fw */
		ret = vpu_hw_session_nr_buffer_config(session->id,
			vpu_mem_addr(controller->nr_buffers[0], MEM_VPU_ID),
			vpu_mem_addr(controller->nr_buffers[1], MEM_VPU_ID));
		if (ret) {
			pr_err("Fail to send NR buffers to fw\n");
			goto exit_nr_bufs;
		}
	}

exit_nr_bufs:
	return ret;
}

static int __active_region_param_get_size(const void *api_arg)
{
	const struct vpu_ctrl_active_region_param *arg = api_arg;

	int size = sizeof(struct vpu_prop_session_active_region_detect);
	/* add the size of exclusion regions */
	if (arg->num_exclusions <= VPU_ACTIVE_REGION_N_EXCLUSIONS)
		size += arg->num_exclusions * sizeof(struct rect);

	return size;
}

static int __active_region_param_bounds_check(const struct vpu_ctrl_desc *desc,
		const void *api_arg)
{
	const struct vpu_ctrl_active_region_param *arg = api_arg;

	return (arg->num_exclusions > VPU_ACTIVE_REGION_N_EXCLUSIONS) ?
			-EINVAL : 0;
}

static int vpu_set_content_protection(struct vpu_dev_session *session,
		const u32 *cp_arg)
{
	int i;
	u32 cp = 0;

	if (!cp_arg)
		return -EINVAL;
	if (*cp_arg)
		cp = 1;

	for (i = 0; i < NUM_VPU_PORTS; i++) {
		/* only change CP status if all ports are in reset */
		if (session->io_client[i] != NULL)
			return -EBUSY;
	}
	for (i = 0; i < NUM_VPU_PORTS; i++)
		session->port_info[i].secure_content = cp;

	return 0;
}

static int vpu_get_content_protection(struct vpu_dev_session *session, u32 *cp)
{
	if (!cp)
		return -EINVAL;
	*cp = session->port_info[INPUT_PORT].secure_content;

	return 0;
}

/* controls description array */
static struct vpu_ctrl_desc ctrl_descriptors[VPU_CTRL_ID_MAX] = {
	[VPU_CTRL_NOISE_REDUCTION] = {
		.name = "Noise Reduction",
		.type = CTRL_TYPE_AUTO_MANUAL,
		.hfi_id = VPU_PROP_SESSION_NOISE_REDUCTION,
		.bounds = {
				.min = VPU_AUTO_NR_LEVEL_MIN,
				.max = VPU_AUTO_NR_LEVEL_MAX,
				.step = VPU_AUTO_NR_LEVEL_STEP,
		},
		.bounds2 = {
				.min = VPU_NOISE_REDUCTION_LEVEL_MIN,
				.max = VPU_NOISE_REDUCTION_LEVEL_MAX,
				.step = VPU_NOISE_REDUCTION_STEP,
		},
		.set_function = (custom_set_function) configure_nr_buffers,
	},
	[VPU_CTRL_IMAGE_ENHANCEMENT] = {
		.name = "Image Enhancement",
		.type = CTRL_TYPE_AUTO_MANUAL,
		.hfi_id = VPU_PROP_SESSION_IMAGE_ENHANCEMENT,
		.bounds = {
				.min = VPU_AUTO_IE_LEVEL_MIN,
				.max = VPU_AUTO_IE_LEVEL_MAX,
				.step = VPU_AUTO_IE_LEVEL_STEP,
		},
		.bounds2 = {
				.min = VPU_IMAGE_ENHANCEMENT_LEVEL_MIN,
				.max = VPU_IMAGE_ENHANCEMENT_LEVEL_MAX,
				.step = VPU_IMAGE_ENHANCEMENT_STEP,
		},
	},
	[VPU_CTRL_ANAMORPHIC_SCALING] = {
		.name = "Anamorphic Scaling",
		.type = CTRL_TYPE_STANDARD,
		.hfi_id = VPU_PROP_SESSION_ANAMORPHIC_SCALING,
		.bounds = {
				.min = VPU_ANAMORPHIC_SCALE_VALUE_MIN,
				.max = VPU_ANAMORPHIC_SCALE_VALUE_MAX,
				.step = VPU_ANAMORPHIC_SCALE_VALUE_STEP,
				.def = VPU_ANAMORPHIC_SCALE_VALUE_DEF,
		},
	},
	[VPU_CTRL_DIRECTIONAL_INTERPOLATION] = {
		.name = "Directional Interpolation",
		.type = CTRL_TYPE_STANDARD,
		.hfi_id = VPU_PROP_SESSION_DI,
		.bounds = {
				.min = VPU_DI_VALUE_MIN,
				.max = VPU_DI_VALUE_MAX,
				.step = 1,
		},
	},
	[VPU_CTRL_RANGE_MAPPING] = {
		.name = "Y/UV Range Mapping",
		.type = CTRL_TYPE_TWO_VALUE,
		.hfi_id = VPU_PROP_SESSION_RANGE_MAPPING,
		.bounds = {
				.min = VPU_RANGE_MAPPING_MIN,
				.max = VPU_RANGE_MAPPING_MAX,
				.step = VPU_RANGE_MAPPING_STEP,
		},
		.bounds2 = {
				.min = VPU_RANGE_MAPPING_MIN,
				.max = VPU_RANGE_MAPPING_MAX,
				.step = VPU_RANGE_MAPPING_STEP,
		},
	},
	[VPU_CTRL_DEINTERLACING] = {
		.name = "Deinterlacing Mode",
		.type = CTRL_TYPE_TWO_VALUE,
		.hfi_id = VPU_PROP_SESSION_DEINTERLACING,
	},
	[VPU_CTRL_ACTIVE_REGION_PARAM] = {
		.name = "Active Region Detection Parameters",
		.hfi_id = VPU_PROP_SESSION_ACTIVE_REGION_DETECT,
		.size = sizeof(struct vpu_ctrl_active_region_param),
		.custom_size = __active_region_param_get_size,
		.cached = 1,
		.bounds_check = __active_region_param_bounds_check,
		.translate_to_hfi = translate_active_region_param_to_hfi,
	},
	[VPU_CTRL_ACTIVE_REGION_RESULT] = {
		.name = "Active Region Detection Result",
		.size = sizeof(struct v4l2_rect),
		.hfi_size = sizeof(struct rect),
		.cached = 1,
		.translate_to_api = translate_active_region_result_to_api,
	},
	[VPU_CTRL_PRIORITY] = {
		.name = "Session Priority",
		.type = CTRL_TYPE_VALUE,
		.hfi_id = VPU_PROP_SESSION_PRIORITY,
	},
	[VPU_CTRL_CONTENT_PROTECTION] = {
		.name = "Content Protection",
		.set_function =
			(custom_set_function) vpu_set_content_protection,
		.get_function =
			(custom_get_function) vpu_get_content_protection,
	},
};


static u32 controller_cache_size;

/*
 * Initializes some control descriptors if type has been set.
 * And calculates required cache size and offests for controls caches
 */
void setup_vpu_controls(void)
{
	struct vpu_ctrl_desc *desc, *prev_desc;
	int i;
	controller_cache_size = 0;

	/* First entry is an invalid control. Ensure it's zeroed out */
	memset(&ctrl_descriptors[0], 0, sizeof(struct vpu_ctrl_desc));

	for (i = 1; i < VPU_CTRL_ID_MAX; i++) {
		desc = &ctrl_descriptors[i];
		prev_desc = &ctrl_descriptors[i - 1];

		switch (desc->type) {
		case CTRL_TYPE_VALUE:
			desc->size = sizeof(__s32);
			desc->hfi_size = sizeof(struct vpu_s_data_value);
			desc->bounds_check = value_bounds_check;
			desc->translate_to_api = translate_ctrl_value_to_api;
			desc->translate_to_hfi = translate_ctrl_value_to_hfi;
			break;
		case CTRL_TYPE_STANDARD:
			desc->size = sizeof(struct vpu_ctrl_standard);
			desc->hfi_size = sizeof(struct vpu_s_data_value);
			desc->bounds_check = standard_bounds_check;
			desc->translate_to_api = translate_ctrl_standard_to_api;
			desc->translate_to_hfi = translate_ctrl_standard_to_hfi;
			break;
		case CTRL_TYPE_AUTO_MANUAL:
			desc->size = sizeof(struct vpu_ctrl_auto_manual);
			desc->hfi_size = sizeof(struct vpu_s_data_value);
			desc->bounds_check = auto_manual_bounds_check;
			desc->translate_to_api =
					translate_ctrl_auto_manual_to_api;
			desc->translate_to_hfi =
					translate_ctrl_auto_manual_to_hfi;
			break;
		case CTRL_TYPE_TWO_VALUE:
			desc->size = sizeof(struct vpu_ctrl_two_value);
			desc->hfi_size = sizeof(struct vpu_ctrl_two_value);
			desc->bounds_check = two_value_bounds_check;
			desc->translate_to_api =
					translate_ctrl_two_value_to_api;
			desc->translate_to_hfi =
					translate_ctrl_two_value_to_hfi;
			break;
		default:
			break;
		}

		controller_cache_size += desc->size;
		desc->cache_offset = prev_desc->cache_offset + prev_desc->size;
	}
}

struct vpu_controller *init_vpu_controller(struct vpu_platform_resources *res)
{
	int i;
	struct vpu_controller *controller =
			kzalloc(sizeof(*controller), GFP_KERNEL);

	if (!controller) {
		pr_err("Failed memory allocation\n");
		return NULL;
	}

	controller->cache_size = controller_cache_size;
	if (controller->cache_size) {
		controller->cache = kzalloc(controller->cache_size, GFP_KERNEL);
		if (!controller->cache) {
			pr_err("Failed cache allocation\n");
			deinit_vpu_controller(controller);
			return NULL;
		}
	}

	for (i = 0; i < NUM_NR_BUFFERS; i++)
		controller->nr_buffers[i] =
				vpu_mem_create_handle(res->mem_client);

	if (i < NUM_NR_BUFFERS) {
		deinit_vpu_controller(controller);
		return NULL;
	}

	return controller;
}

void deinit_vpu_controller(struct vpu_controller *controller)
{
	int i;

	if (!controller)
		return;

	for (i = 0; i < NUM_NR_BUFFERS; i++) {
		vpu_mem_destroy_handle(controller->nr_buffers[i]);
		controller->nr_buffers[i] = NULL;
	}

	kfree(controller->cache);
	controller->cache = NULL;
	kfree(controller);
}

void *get_control(struct vpu_controller *controller, u32 id)
{
	struct vpu_ctrl_desc *desc;
	if (!controller || !controller->cache  ||
		id <= VPU_CTRL_ID_MIN || id >= VPU_CTRL_ID_MAX)
		return NULL;

	desc = &ctrl_descriptors[id];

	if (!desc->size || desc->cache_offset > controller->cache_size ||
	    (desc->cache_offset + desc->size) > controller->cache_size)
		return NULL;
	else
		return controller->cache + desc->cache_offset;
}

int apply_vpu_control(struct vpu_dev_session *session, int set,
		struct vpu_control *control)
{
	struct vpu_controller *controller = session->controller;
	struct vpu_ctrl_desc *desc;
	void *cache = NULL;
	int ret = 0;

	u8 hfi_data[128]; /* temporary store of data translated to HFI */
	u32 hfi_size = 0; /* size of HFI data */
	memset(hfi_data, 0, 128);

	if (!controller || !control)
		return -ENOMEM;

	if (!control->control_id || control->control_id >= VPU_CTRL_ID_MAX) {
		pr_err("control id %d is invalid\n", control->control_id);
		return -EINVAL;
	}

	desc = &ctrl_descriptors[control->control_id];
	if (!desc->hfi_id && !desc->size &&
			!desc->set_function && !desc->get_function) {
		pr_err("control id %d is not supported\n", control->control_id);
		return -EINVAL;
	}
	pr_debug("%s control %s\n", set ? "Set" : "Get", desc->name);

	cache = get_control(controller, control->control_id);
	if (!cache) {
		pr_err("control %s has no cache\n", desc->name);
		return -ENOMEM;
	}

	hfi_size = (desc->custom_size) ?
		call_op(desc, custom_size, &control->data) : desc->hfi_size;

	if (set) {
		ret = call_op(desc, bounds_check, desc, &control->data);
		if (ret) {
			pr_err("bounds check failed for %s\n", desc->name);
			return ret;
		}

		if (desc->translate_to_hfi) {
			call_op(desc, translate_to_hfi,
					&control->data, hfi_data);
		} else {
			hfi_size = desc->size;
			memcpy(hfi_data, &control->data, desc->size);
		}

		mutex_lock(&session->lock);

		if (desc->hfi_id) { /* set property */
			ret = vpu_hw_session_s_property(session->id,
					desc->hfi_id, hfi_data, hfi_size);
			if (ret) {
				pr_err("%s s_property failed\n", desc->name);
				goto err_apply_control;
			}
		}


		if (desc->set_function) {/* custom set function */
			ret = call_op(desc, set_function,
					session, &control->data);
			if (ret) {
				pr_err("%s set_function failed\n", desc->name);
				goto err_apply_control;
			}
		}

		if (desc->hfi_id) { /* commit control */
			ret = commit_control(session, 0);
			if (ret)
				goto err_apply_control;
		}

		/* update controls cache */
		memcpy(cache, &control->data, desc->size);

		mutex_unlock(&session->lock);
	} else {
		/* If cached is set, copy control data from cache */
		if (desc->cached) {
			memcpy(&control->data, cache, desc->size);
			return 0;
		}

		if (desc->get_function) /* custom get function */
			return call_op(desc, get_function,
					session, &control->data);

		if (desc->hfi_id) {

			ret = vpu_hw_session_g_property(session->id,
					desc->hfi_id, hfi_data, hfi_size);
			if (ret)
				return ret;

			if (desc->translate_to_api)
				call_op(desc, translate_to_api,
						hfi_data, &control->data);
			else
				memcpy(&control->data, hfi_data, desc->size);
		}
	}

	return ret;

err_apply_control:
	mutex_unlock(&session->lock);
	return ret;
}

int apply_vpu_control_extended(struct vpu_client *client, int set,
		struct vpu_control_extended *control)
{
	struct vpu_dev_session *session = client->session;
	int ret = 0;
	if (!control)
		return -ENOMEM;

	if (control->data_len > VPU_MAX_EXT_DATA_SIZE) {
		pr_err("data_len (%d) > than max (%d)\n",
				control->data_len, VPU_MAX_EXT_DATA_SIZE);
		return -EINVAL;
	} else if (!set && control->buf_size > VPU_MAX_EXT_DATA_SIZE) {
		pr_err("buf_size (%d) > than max (%d)\n",
				control->buf_size, VPU_MAX_EXT_DATA_SIZE);
		return -EINVAL;
	}

	if (control->type == 0) { /* system */
		if (set)
			ret = vpu_hw_sys_s_property_ext(control->data_ptr,
					control->data_len);
		else
			ret = vpu_hw_sys_g_property_ext(control->data_ptr,
					control->data_len, control->buf_ptr,
					control->buf_size);
	} else if (control->type == 1) { /* session */
		if (!session)
			return -EPERM;

		if (set)
			ret = vpu_hw_session_s_property_ext(session->id,
					control->data_ptr, control->data_len);
		else
			ret = vpu_hw_session_g_property_ext(session->id,
					control->data_ptr, control->data_len,
					control->buf_ptr, control->buf_size);
	} else {
		pr_err("control type %d is invalid\n", control->type);
		return -EINVAL;
	}

	return ret;
}

/*
 * Descriptions of supported formats
 */
static const struct vpu_format_desc vpu_port_formats[] = {
	[PIXEL_FORMAT_RGB888] = {
		.description = "RGB-8-8-8",
		.fourcc = V4L2_PIX_FMT_RGB24,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 24, .heightfactor = 1},
	},
	[PIXEL_FORMAT_XRGB8888] = {
		.description = "ARGB-8-8-8-8",
		.fourcc = V4L2_PIX_FMT_RGB32,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 32, .heightfactor = 1},
	},
	[PIXEL_FORMAT_XRGB2] = {
		.description = "ARGB-2-10-10-10",
		.fourcc = V4L2_PIX_FMT_XRGB2,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 32, .heightfactor = 1},
	},
	[PIXEL_FORMAT_BGR888] = {
		.description = "BGR-8-8-8",
		.fourcc = V4L2_PIX_FMT_BGR24,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 24, .heightfactor = 1},
	},
	[PIXEL_FORMAT_BGRX8888] = {
		.description = "BGRX-8-8-8-8",
		.fourcc = V4L2_PIX_FMT_BGR32,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 32, .heightfactor = 1},
	},
	[PIXEL_FORMAT_XBGR2] = {
		.description = "XBGR-2-10-10-10",
		.fourcc = V4L2_PIX_FMT_XBGR2,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 32, .heightfactor = 1},
	},
	[PIXEL_FORMAT_NV12] = {
		.description = "YUV 4:2:0 semi-planar",
		.fourcc = V4L2_PIX_FMT_NV12,
		.num_planes = 2,
		.plane[0] = { .bitsperpixel = 8, .heightfactor = 1},
		.plane[1] = { .bitsperpixel = 8, .heightfactor = 2},
	},
	[PIXEL_FORMAT_NV21] = {
		.description = "YVU 4:2:0 semi-planar",
		.fourcc = V4L2_PIX_FMT_NV21,
		.num_planes = 2,
		.plane[0] = { .bitsperpixel = 8, .heightfactor = 1},
		.plane[1] = { .bitsperpixel = 8, .heightfactor = 2},
	},
	[PIXEL_FORMAT_YUYV] = {
		.description = "YUYV 4:2:2 intlvd",
		.fourcc = V4L2_PIX_FMT_YUYV,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 16, .heightfactor = 1},
	},
	[PIXEL_FORMAT_YVYU] = {
		.description = "YVYU 4:2:2 intlvd",
		.fourcc = V4L2_PIX_FMT_YVYU,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 16, .heightfactor = 1},
	},
	[PIXEL_FORMAT_VYUY] = {
		.description = "VYUY 4:2:2 intlvd",
		.fourcc = V4L2_PIX_FMT_VYUY,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 16, .heightfactor = 1},
	},
	[PIXEL_FORMAT_UYVY] = {
		.description = "UYVY 4:2:2 intlvd",
		.fourcc = V4L2_PIX_FMT_UYVY,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 16, .heightfactor = 1},
	},
	[PIXEL_FORMAT_YUYV_LOOSE] = {
		.description = "YUYV 4:2:2 10bit intlvd loose",
		.fourcc = V4L2_PIX_FMT_YUYV10,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 24, .heightfactor = 1},
	},
	[PIXEL_FORMAT_YUV_8BIT_INTERLEAVED_DENSE] = {
		.description = "YUV 4:4:4 8bit intlvd dense",
		.fourcc = V4L2_PIX_FMT_YUV8,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 24, .heightfactor = 1},
	},
	[PIXEL_FORMAT_YUV_10BIT_INTERLEAVED_LOOSE] = {
		.description = "YUV 4:4:4 10bit intlvd loose",
		.fourcc = V4L2_PIX_FMT_YUV10,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 32, .heightfactor = 1},
	},
	[PIXEL_FORMAT_COMPRESSED_YUYV422] = {
		.description = "YUYV422 Compressed",
		.fourcc = V4L2_PIX_FMT_YUYV10BWC,
		.num_planes = 1,
		.plane[0] = { .bitsperpixel = 10, .heightfactor = 1},
	},
};

#define PADDING 128
#define CEIL(x, y) (((x) + ((y)-1)) / (y))
u32 get_bytesperline(u32 width, u32 bitsperpixel, u32 input_bytesperline)
{
	u32 bytesperline = CEIL(width * bitsperpixel, 8);
	u32 padding_factor = CEIL(bytesperline, PADDING);
	u32 min_bytesperline = PADDING * padding_factor;

	if (input_bytesperline < min_bytesperline) /* input too small */
		return min_bytesperline;
	else if (input_bytesperline % PADDING) /* must be multiple of padding */
		return min_bytesperline;
	else if (input_bytesperline > SZ_32K) /* not too big */
		return min_bytesperline;
	else
		return input_bytesperline; /* input bytesperline was fine */
}

int is_format_valid(struct v4l2_format *fmt)
{
#define VPU_HEIGHT_MINIMUM 72
#define VPU_HEIGHT_MULTIPLE 8
	u32 height = fmt->fmt.pix_mp.height;
	u32 width = fmt->fmt.pix_mp.width;

	if (height < VPU_HEIGHT_MINIMUM)
		return 0;
	if ((height / VPU_HEIGHT_MULTIPLE) * VPU_HEIGHT_MULTIPLE != height)
		return 0;
	if ((height & 1) || (width & 1)) /* must be even */
		return 0;

	return 1;
}

const struct vpu_format_desc *query_supported_formats(int index)
{
	if (index < 0 || index >= ARRAY_SIZE(vpu_port_formats))
		return NULL;
	if (vpu_port_formats[index].fourcc == 0)
		return NULL; /* fourcc of 0 indicates no support in API */
	else
		return &vpu_port_formats[index];
}

/*
 * Configuration commit handling
 */
static int __get_bits_per_pixel(u32 api_pix_fmt)
{
	int i, bpp = 0;
	const struct vpu_format_desc *vpu_format;
	u32 hfi_pix_fmt = translate_pixelformat_to_hfi(api_pix_fmt);

	vpu_format = query_supported_formats(hfi_pix_fmt);
	if (!vpu_format)
		return -EINVAL;

	for (i = 0; i < vpu_format->num_planes; i++)
		bpp  += vpu_format->plane[i].bitsperpixel /
			vpu_format->plane[i].heightfactor;

	return bpp;
}

/* compute the average load value for all VPU sessions (in bits per second) */
static u32 __get_vpu_load(struct vpu_dev_core *core)
{
	int i;
	u32 load_bps = 0;
	struct vpu_dev_session *session = NULL;

	for (i = 0; i < VPU_NUM_SESSIONS; i++) {
		session = core->sessions[i];
		if (!session)
			break;
		pr_debug("session %d load: %dbps", i, session->load);
		load_bps = max(load_bps, session->load);
	}

	pr_debug("Calculated load = %d bps\n", load_bps);
	return 500000; /* TODO replace by load */
}

/* returns the session load in bits per second */
int __calculate_session_load(struct vpu_dev_session *session)
{
	u32 bits_per_stream;
	u32 load_bits_per_sec;
	u32 frame_rate;
	bool b_interlaced, b_nr, b_bbroi;
	u32 bpp_in, bpp_vpu, h_in, w_in, bpp_out, h_out, w_out;
	struct vpu_port_info *in = &session->port_info[INPUT_PORT];
	struct vpu_port_info *out = &session->port_info[OUTPUT_PORT];
	struct vpu_controller *controller = session->controller;
	struct vpu_ctrl_auto_manual *nr_cache =
		(struct vpu_ctrl_auto_manual *) get_control(controller,
				VPU_CTRL_NOISE_REDUCTION);

	/*
	 * get required info before computation
	 */
	b_interlaced = (in->scan_mode == LINESCANINTERLACED);
	b_nr = nr_cache ? (nr_cache->enable == PROP_TRUE) : false;
	b_bbroi = false; /*TODO: how to calculate? */
	bpp_in = __get_bits_per_pixel(in->format.pixelformat);
	if (bpp_in < 0)
		bpp_in = 0;
	bpp_vpu = 16; /* 8bpc422 pixel format */
	h_in = in->format.height;
	w_in = in->format.width;
	bpp_out = __get_bits_per_pixel(out->format.pixelformat);
	if (bpp_out < 0)
		bpp_out = 0;

	h_out = out->format.height;
	w_out = out->format.width;
	frame_rate = max(in->framerate, out->framerate);

	/*
	 * compute the current session's load
	 */
	bits_per_stream =  ((1 + b_interlaced) * (bpp_in)
			+ (2 * b_nr + 2 * b_bbroi) * (bpp_vpu)) * (h_in * w_in)
			+ (bpp_out) * (h_out * w_out);
	load_bits_per_sec = bits_per_stream * frame_rate;

	pr_debug("Calculated load (%d bps)\n", load_bits_per_sec);
	return load_bits_per_sec;
}

int commit_initial_config(struct vpu_dev_session *session)
{
	struct vpu_prop_session_input in_param;
	struct vpu_prop_session_output out_param;
	struct vpu_ctrl_auto_manual *nr;
	int ret = 0;

	if (session->commit_state == COMMITED)
		return 0;

	nr = get_control(session->controller, VPU_CTRL_NOISE_REDUCTION);
	ret = configure_nr_buffers(session, nr);
	if (ret) {
		pr_err("Failed to configure nr\n");
		goto exit_commit;
	}

	translate_input_format_to_hfi(&session->port_info[INPUT_PORT],
			&in_param);
	ret = vpu_hw_session_s_input_params(session->id, &in_param);
	if (ret) {
		pr_err("Failed to set port 0 config\n");
		goto exit_commit;
	}

	translate_output_format_to_hfi(&session->port_info[OUTPUT_PORT],
			&out_param);
	ret = vpu_hw_session_s_output_params(session->id, &out_param);
	if (ret) {
		pr_err("Failed to set port 1 config\n");
		goto exit_commit;
	}

	/* calculate and store the newly computed session load */
	session->load = __calculate_session_load(session);

	ret = vpu_hw_session_commit(session->id, CH_COMMIT_AT_ONCE,
			__get_vpu_load(session->core));
	if (ret) {
		pr_err("Commit Failed (err %d)\n", ret);
		notify_vpu_event_session(session, VPU_EVENT_INVALID_CONFIG,
				NULL, 0);
		goto exit_commit;
	}

	session->commit_state = COMMITED;
	pr_debug("Initial configuration committed successfully\n");

exit_commit:
	return ret;
}

int commit_port_config(struct vpu_dev_session *session,	int port, int new_load)
{
	struct vpu_prop_session_input in_param;
	struct vpu_prop_session_output out_param;
	int ret = 0;

	if (session->commit_state != COMMITED)
		return 0; /* wait for initial configuration */

	if (port == INPUT_PORT) {
		struct vpu_ctrl_auto_manual *nr = get_control(
			session->controller, VPU_CTRL_NOISE_REDUCTION);
		ret = configure_nr_buffers(session, nr);
		if (ret) {
			pr_err("Failed to configure nr\n");
			return ret;
		}

		translate_input_format_to_hfi(&session->port_info[INPUT_PORT],
				&in_param);
		ret = vpu_hw_session_s_input_params(session->id, &in_param);
	} else if (port == OUTPUT_PORT) {
		translate_output_format_to_hfi(&session->port_info[OUTPUT_PORT],
				&out_param);
		ret = vpu_hw_session_s_output_params(session->id, &out_param);
	} else
		return -EINVAL;

	if (ret) {
		pr_err("Failed to set port config\n");
		goto exit_commit;
	}

	if (new_load)
		session->load = __calculate_session_load(session);

	ret = vpu_hw_session_commit(session->id, CH_COMMIT_IN_ORDER,
			__get_vpu_load(session->core));
	if (ret) {
		pr_err("Commit Failed\n");
		if (ret == -EIO)
			ret = -EAGAIN; /* special runtime commit fail retval */
		goto exit_commit;
	}

exit_commit:
	return ret;
}

int commit_control(struct vpu_dev_session *session, int new_load)
{
	int ret = 0;

	if (session->commit_state != COMMITED)
		return 0; /* wait for initial configuration */

	if (new_load)
		session->load = __calculate_session_load(session);

	ret = vpu_hw_session_commit(session->id, CH_COMMIT_AT_ONCE,
			__get_vpu_load(session->core));
	if (ret) {
		pr_err("Commit Failed\n");
		if (ret == -EIO)
			ret = -EAGAIN; /* special runtime commit fail retval */
		goto exit_commit;
	}

exit_commit:
	return ret;
}
