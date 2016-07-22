#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#define PP_MAX_STEP 2
#include "tdm_sprd.h"

#define SPRD_C(b,m)              (((b) >> (m)) & 0xFF)
#define SPRD_FOURCC_STR(id)      SPRD_C(id,0), SPRD_C(id,8), SPRD_C(id,16), SPRD_C(id,24)
typedef struct _tdm_sprd_pp_buffer {
	int index;
	tbm_surface_h src;
	tbm_surface_h dst;
	struct list_head link;
} tdm_sprd_pp_buffer;

typedef enum _tdm_sprd_pp_ctrl {
	IPP_STOP = 0,
	IPP_RUN = 1,
	IPP_PAUSE = 2
} tdm_sprd_pp_ctrl;

typedef enum _tdm_sprd_task_status {
	TASK_WAITING,
	TASK_CONVERTING,
	TASK_DONE
} tdm_sprd_task_status;

typedef struct _tdm_sprd_prop_id {
	enum drm_sprd_ipp_ctrl status;
	unsigned int prop_id;
	struct list_head link;
} tdm_sprd_prop_id;

typedef struct _tdm_sprd_pp_task {
	int stamp;
	unsigned int prop_id[PP_MAX_STEP];
	tdm_sprd_pp_buffer buffers[PP_MAX_STEP];
	unsigned int max_step;
	unsigned int current_step;
	int status;
	tdm_pp_done_handler done_func;
	void *done_user_data;
	struct list_head link;
} tdm_sprd_pp_task;

typedef struct _tdm_sprd_pp_roadmap {
	unsigned int prop_id[PP_MAX_STEP];
	tdm_info_pp step_info[PP_MAX_STEP];
	unsigned int max_step;
} tdm_sprd_pp_roadmap;
typedef struct _tdm_sprd_pp_data {
	tdm_sprd_data *sprd_data;
	int stamp;
	struct list_head pending_buffer_list;
	struct list_head pending_tasks_list;
	struct list_head prop_id_list;
	tdm_sprd_pp_roadmap roadmap;
	tdm_sprd_pp_roadmap new_roadmap;
	tdm_sprd_pp_task *current_task_p;
	tdm_pp_done_handler done_func;
	void *done_user_data;
	int roadmap_changed;
	int new_buffers;
	int first_event;
	struct list_head link;
} tdm_sprd_pp_data;

#if 1
static tbm_format pp_formats[] = {
	TBM_FORMAT_XRGB8888,
	TBM_FORMAT_ARGB8888,
	TBM_FORMAT_RGB888,
	TBM_FORMAT_RGB565,
	TBM_FORMAT_NV12,
	TBM_FORMAT_YUV420,
	TBM_FORMAT_YUV422,
};
#else
static tbm_format *pp_formats = NULL;
#endif

#if 1
#define NUM_PP_FORMAT   (sizeof(pp_formats) / sizeof(pp_formats[0]))
#else
#define NUM_PP_FORMAT 0
#endif

static int pp_list_init = 0;
static int pp_stamp = 1001;
static int task_stamp = 10001;
static struct list_head pp_list;

static tdm_sprd_prop_id *
_find_prop_id (tdm_sprd_pp_data *pp_data, unsigned int prop_id)
{
	tdm_sprd_prop_id *prop = NULL;
	LIST_FOR_EACH_ENTRY (prop, &pp_data->prop_id_list, link) {
		if (prop->prop_id == prop_id)
			return prop;
	}
	return NULL;
}

int _tdm_sprd_pp_check_struct (tdm_sprd_pp_data *pp_data)
{
	RETURN_VAL_IF_FAIL(pp_list_init == 1, 0);
	if (pp_data == NULL) {
		TDM_ERR("pp nil(0). Received NULL pointer");
		return 0;
	}
	tdm_sprd_pp_data * pp_next = NULL;
	LIST_FOR_EACH_ENTRY(pp_next, &pp_list, link) {
		if (pp_next->stamp == pp_data->stamp) {
			return 1;
		}
	}
	TDM_ERR("pp %p(%d). Wrong ", pp_data, pp_data->stamp);
	return 0;
}

void _tdm_sprd_pp_roadmap_print(tdm_sprd_pp_roadmap *roadmap)
{
	RETURN_VOID_IF_FAIL(roadmap);
	int i;
	TDM_DBG("Count of steps %d", roadmap->max_step);
	for (i = 0; i < roadmap->max_step; i++) {
		TDM_DBG("Step %d, Prop_id %d", i+1, roadmap->prop_id[i]);
		TDM_DBG("format (%u)%c%c%c%c -> (%u)%c%c%c%c", roadmap->step_info[i].src_config.format,
				SPRD_FOURCC_STR(roadmap->step_info[i].src_config.format), roadmap->step_info[i].dst_config.format,
				SPRD_FOURCC_STR(roadmap->step_info[i].dst_config.format));
		TDM_DBG("rotate+flip is %u+%s", roadmap->step_info[i].transform % 4,
				roadmap->step_info[i].transform > 3?"Horizontal":"None");
		TDM_DBG("size src->dst (w)x(h) (%u)x(%u) -> (%u)x(%u)", roadmap->step_info[i].src_config.size.h,
				roadmap->step_info[i].src_config.size.v, roadmap->step_info[i].dst_config.size.h,
				roadmap->step_info[i].dst_config.size.v);
		TDM_DBG("crop src->dst (x)(y)+(w)+(h) (%u)x(%u)+(%u)+(%u) -> (%u)x(%u)+(%u)+(%u)", roadmap->step_info[i].src_config.pos.x,
				roadmap->step_info[i].src_config.pos.y, roadmap->step_info[i].src_config.pos.w,
				roadmap->step_info[i].src_config.pos.h, roadmap->step_info[i].dst_config.pos.x,
				roadmap->step_info[i].dst_config.pos.y, roadmap->step_info[i].dst_config.pos.w,
				roadmap->step_info[i].dst_config.pos.h);
	}
	TDM_DBG("-------------------------------------------------------------------");
}

int _tdm_sprd_pp_roadmap_copy(tdm_sprd_pp_roadmap *to_roadmap, tdm_sprd_pp_roadmap *from_roadmap)
{
	int i = 0;
	for (i = 0; i < PP_MAX_STEP; i++) {
		memcpy(&to_roadmap->step_info[i], &from_roadmap->step_info[i], sizeof(tdm_info_pp));
	}
	to_roadmap->max_step = from_roadmap->max_step;
	return 1;
}

#if 1
static void
_tdm_sprd_pp_property_format_check(struct drm_sprd_ipp_property *prop)
{
	int src_alpha = 0, dst_alpha = 0;
	__u32 src_fmt, dst_fmt, change_fmt;
	__u8 alpha;

	src_fmt = prop->config[0].fmt;
	dst_fmt = prop->config[1].fmt;

	/* no corresponding format to AYUV */
	if (src_fmt == TBM_FORMAT_AYUV || dst_fmt == TBM_FORMAT_AYUV)
		return;

	alpha = src_fmt & 0xff;
	if (alpha == 'A')
		src_alpha = 1;
	alpha = (src_fmt >> 8) & 0xff;
	if (alpha == 'A')
		src_alpha = 2;

	alpha = dst_fmt & 0xff;
	if (alpha == 'A')
		dst_alpha = 1;
	alpha = (dst_fmt >> 8) & 0xff;
	if (alpha == 'A')
		dst_alpha = 2;

	/* not chane format if both have alpha or not*/
	if ((src_alpha == 0 && dst_alpha == 0) ||
		(src_alpha != 0 && dst_alpha != 0))
		return;

	alpha = 'X';
	if (src_alpha == 1) {
		change_fmt = src_fmt;
		change_fmt = (change_fmt & 0xffffff00) | alpha;
		prop->config[0].fmt = change_fmt;
	} else if (src_alpha == 2) {
		change_fmt = src_fmt;
		change_fmt = (change_fmt & 0xffff00ff) | alpha << 8;
		prop->config[0].fmt = change_fmt;
	} else if (dst_alpha == 1) {
		change_fmt = dst_fmt;
		change_fmt = (change_fmt & 0xffffff00) | alpha;
		prop->config[1].fmt = change_fmt;
	} else if (dst_alpha == 2) {
		change_fmt = dst_fmt;
		change_fmt = (change_fmt & 0xffff00ff) | alpha << 8;
		prop->config[1].fmt = change_fmt;
	}

	if (src_alpha != 0)
		TDM_ERR("src format change fmt(%c%c%c%c) -> fmt(%c%c%c%c)",
				FOURCC_STR(src_fmt), FOURCC_STR(change_fmt));
	else
		TDM_ERR("dst format change fmt(%c%c%c%c) -> fmt(%c%c%c%c)",
				FOURCC_STR(dst_fmt), FOURCC_STR(change_fmt));
}

static unsigned int
_tdm_sprd_pp_set(tdm_sprd_pp_data *pp_data, tdm_info_pp *info,
				 unsigned int prop_id)
{
	tdm_sprd_data *sprd_data = pp_data->sprd_data;
	struct drm_sprd_ipp_property property;
	int ret = 0;

	CLEAR(property);
	property.config[0].ops_id = SPRD_DRM_OPS_SRC;
	property.config[0].fmt = tdm_sprd_format_to_drm_format(info->src_config.format);
	memcpy(&property.config[0].sz, &info->src_config.size, sizeof(tdm_size));
	memcpy(&property.config[0].pos, &info->src_config.pos, sizeof(tdm_pos));
	property.config[1].ops_id = SPRD_DRM_OPS_DST;
	property.config[1].degree = info->transform % 4;
	property.config[1].flip = (info->transform > 3) ? SPRD_DRM_FLIP_HORIZONTAL : 0;
	property.config[1].fmt = tdm_sprd_format_to_drm_format(info->dst_config.format);
	memcpy(&property.config[1].sz, &info->dst_config.size, sizeof(tdm_size));
	memcpy(&property.config[1].pos, &info->dst_config.pos, sizeof(tdm_pos));
	property.cmd = IPP_CMD_M2M;
	property.prop_id = prop_id;
	property.type = IPP_EVENT_DRIVEN;

	_tdm_sprd_pp_property_format_check(&property);

	TDM_DBG("pp %p(%d). src : flip(%x) deg(%d) fmt(%c%c%c%c) sz(%dx%d) pos(%d,%d %dx%d)  ",
			pp_data, pp_data->stamp,
			property.config[0].flip, property.config[0].degree,
			FOURCC_STR(property.config[0].fmt),
			property.config[0].sz.hsize, property.config[0].sz.vsize,
			property.config[0].pos.x, property.config[0].pos.y, property.config[0].pos.w,
			property.config[0].pos.h);
	TDM_DBG("pp %p(%d). dst : flip(%x) deg(%d) fmt(%c%c%c%c) sz(%dx%d) pos(%d,%d %dx%d)  ",
			pp_data, pp_data->stamp,
			property.config[1].flip, property.config[1].degree,
			FOURCC_STR(property.config[1].fmt),
			property.config[1].sz.hsize, property.config[1].sz.vsize,
			property.config[1].pos.x, property.config[1].pos.y, property.config[1].pos.w,
			property.config[1].pos.h);

	ret = ioctl(sprd_data->drm_fd, DRM_IOCTL_SPRD_IPP_SET_PROPERTY, &property);
	if (ret) {
		TDM_ERR("pp %p(%d). failed: %m", pp_data, pp_data->stamp);
		return 0;
	}

	TDM_DBG("pp %p(%d). success. prop_id(%u) ", pp_data, pp_data->stamp, property.prop_id);
	return property.prop_id;
}
#endif
#if 1
static tdm_error
_tdm_sprd_pp_queue(tdm_sprd_pp_data *pp_data, unsigned int prop_id,
				   tbm_surface_h src, tbm_surface_h dst, enum drm_sprd_ipp_buf_type type)
{
	tdm_sprd_data *sprd_data = pp_data->sprd_data;
	struct drm_sprd_ipp_queue_buf buf;
	int i, bo_num, ret = 0;
	uint32_t size= 0 , stride = 0, offset = 0;

	CLEAR(buf);
	buf.prop_id = prop_id;
	buf.ops_id = SPRD_DRM_OPS_SRC;
	buf.buf_type = type;
	buf.buf_id = 0;
	buf.user_data = (__u64)(uintptr_t)pp_data;
	bo_num = tbm_surface_internal_get_num_bos(src);
	for (i = 0; i < SPRD_DRM_PLANAR_MAX && i < bo_num; i++) {
		tbm_bo bo = tbm_surface_internal_get_bo(src, i);
		buf.handle[i] = (__u32)tbm_bo_get_handle(bo, TBM_DEVICE_DEFAULT).u32;
		tbm_surface_internal_get_plane_data(src, i, &size, &offset, &stride);
		TDM_DBG("pp %p(%d). prop_id(%d) src plane(%d) size(%u) offset(%u) stride(%u) ",
				pp_data, pp_data->stamp, buf.prop_id, i, size, offset, stride);
	}

	TDM_DBG("pp %p(%d). prop_id(%d) ops_id(%d) ctrl(%d) id(%d) handles(%x %x %x). ",
			pp_data, pp_data->stamp,
			buf.prop_id, buf.ops_id, buf.buf_type, buf.buf_id,
			buf.handle[0], buf.handle[1], buf.handle[2]);

	ret = ioctl(sprd_data->drm_fd, DRM_IOCTL_SPRD_IPP_QUEUE_BUF, &buf);
	if (ret) {
		TDM_ERR("pp %p(%d). src failed. prop_id(%d) op(%d) buf(%d) id(%d). %m",
				pp_data, pp_data->stamp,
				buf.prop_id, buf.ops_id, buf.buf_type, buf.buf_id);
		return TDM_ERROR_OPERATION_FAILED;
	}

	CLEAR(buf);
	buf.prop_id = prop_id;
	buf.ops_id = SPRD_DRM_OPS_DST;
	buf.buf_type = type;
	buf.buf_id = 0;
	buf.user_data = (__u64)(uintptr_t)pp_data;
	bo_num = tbm_surface_internal_get_num_bos(dst);
	for (i = 0; i < SPRD_DRM_PLANAR_MAX && i < bo_num; i++) {
		tbm_bo bo = tbm_surface_internal_get_bo(dst, i);
		buf.handle[i] = (__u32)tbm_bo_get_handle(bo, TBM_DEVICE_DEFAULT).u32;
		tbm_surface_internal_get_plane_data(dst, i, &size, &offset, &stride);
		TDM_DBG("pp %p(%d). prop_id(%d) dst plane(%d) size(%u) offset(%u) stride(%u) ",
				pp_data, pp_data->stamp, buf.prop_id, i, size, offset, stride);
	}

	TDM_DBG("pp %p(%d). prop_id(%d) ops_id(%d) ctrl(%d) id(%d) handles(%x %x %x). ",
			pp_data, pp_data->stamp,
			buf.prop_id, buf.ops_id, buf.buf_type, buf.buf_id,
			buf.handle[0], buf.handle[1], buf.handle[2]);

	ret = ioctl(sprd_data->drm_fd, DRM_IOCTL_SPRD_IPP_QUEUE_BUF, &buf);
	if (ret) {
		TDM_ERR("pp %p(%d). dst failed. prop_id(%d) op(%d) buf(%d) id(%d). %m",
				pp_data, pp_data->stamp,
				buf.prop_id, buf.ops_id, buf.buf_type, buf.buf_id);
		return TDM_ERROR_OPERATION_FAILED;
	}

	TDM_DBG("pp %p(%d). Success. prop_id(%d)", pp_data, pp_data->stamp, buf.prop_id);
	return TDM_ERROR_NONE;
}
#endif
#if 1
static tdm_error
_tdm_sprd_pp_cmd(tdm_sprd_pp_data *pp_data, unsigned int prop_id,
				 tdm_sprd_pp_ctrl cmd)
{
	tdm_sprd_data *sprd_data = pp_data->sprd_data;
	struct drm_sprd_ipp_cmd_ctrl ctrl;
	int ret = 0;
	tdm_sprd_prop_id * found_prop = _find_prop_id(pp_data, prop_id);
	if (found_prop == NULL) {
		if ((found_prop = calloc(1, sizeof (tdm_sprd_prop_id))) == NULL) {
			TDM_ERR("pp %p(%d). Out of memory", pp_data, pp_data->stamp);
			return TDM_ERROR_OUT_OF_MEMORY;
		}
		found_prop->prop_id = prop_id;
		found_prop->status = IPP_CTRL_STOP;
		LIST_ADDTAIL(&found_prop->link, &pp_data->prop_id_list);
	}
	ctrl.prop_id = prop_id;
	switch(cmd) {
		case IPP_RUN:
			if (found_prop->status == IPP_CTRL_STOP)
				ctrl.ctrl = IPP_CTRL_PLAY;
			else if (found_prop->status == IPP_CTRL_PAUSE)
				ctrl.ctrl = IPP_CTRL_RESUME;
			else if (found_prop->status == IPP_CTRL_PLAY ||
					 found_prop->status ==IPP_CTRL_RESUME)
				return TDM_ERROR_NONE;
			break;
		case IPP_PAUSE:
			if (found_prop->status == IPP_CTRL_PLAY ||
				found_prop->status ==IPP_CTRL_RESUME)
				ctrl.ctrl = IPP_CTRL_PAUSE;
			else if (found_prop->status == IPP_CTRL_PAUSE ||
					 found_prop->status == IPP_CTRL_STOP)
				return TDM_ERROR_NONE;
			break;
		case IPP_STOP:
			if (found_prop->status == IPP_CTRL_STOP) {
				LIST_DEL(&found_prop->link);
				free(found_prop);
				return TDM_ERROR_NONE;
			}
			LIST_DEL(&found_prop->link);
			free(found_prop);
			ctrl.ctrl = IPP_CTRL_STOP;
			found_prop = NULL;
			break;
		default:
			break;
	}

	TDM_DBG("pp %p(%d). prop_id(%d) ctrl(%d). ", pp_data, pp_data->stamp, ctrl.prop_id, ctrl.ctrl);

	ret = ioctl(sprd_data->drm_fd, DRM_IOCTL_SPRD_IPP_CMD_CTRL, &ctrl);
	if (ret) {
		TDM_ERR("pp %p(%d). Failed. prop_id(%d) ctrl(%d). %m", pp_data, pp_data->stamp, ctrl.prop_id, ctrl.ctrl);
		return TDM_ERROR_OPERATION_FAILED;
	}
	if (found_prop) {
		found_prop->status = ctrl.ctrl;
	}
	TDM_DBG("pp %p(%d). Success. prop_id(%d) ", pp_data, pp_data->stamp, ctrl.prop_id);
	return TDM_ERROR_NONE;
}
#endif
static void
_tdm_sprd_pp_destroy_task (tdm_sprd_pp_data *pp_data, tdm_sprd_pp_task * task)
{
	int i;

	for (i = 0; i < task->max_step; i++) {
		if (task->buffers[i].src)
			tbm_surface_internal_unref(task->buffers[i].src);
		if (task->buffers[i].dst)
			tbm_surface_internal_unref(task->buffers[i].dst);
	}

	if (task->done_func) {
		TDM_DBG("pp %p(%d). Return src %p dst %p", pp_data, pp_data->stamp,
				task->buffers[0].src, task->buffers[task->max_step-1].dst);
		task->done_func(pp_data, task->buffers[0].src,
						task->buffers[task->max_step-1].dst,
						task->done_user_data);
	}
	TDM_DBG("pp %p(%d). Task %p(%d) released", pp_data, pp_data->stamp, task, task->stamp);
	free(task);
}

static int
_tdm_sprd_pp_make_new_tasks(tdm_sprd_pp_data *pp_data)
{
	tdm_sprd_pp_buffer *main_buffer = NULL, *b = NULL, *bb = NULL;
	tdm_sprd_pp_task *new_task = NULL;
	int i;
	if (LIST_IS_EMPTY(&pp_data->pending_buffer_list)) {
		TDM_WRN("pp %p(%d) buffer list is empty. Nothing to do", pp_data, pp_data->stamp);
		return 0;
	}
	LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->pending_buffer_list, link) {
		main_buffer = b;
		if ((new_task = calloc(1, sizeof(tdm_sprd_pp_task))) == NULL) {
			TDM_ERR("Out of memory");
			return -1;
		}
		LIST_DEL(&b->link);
		memcpy(new_task->prop_id, pp_data->roadmap.prop_id, sizeof(unsigned int)*PP_MAX_STEP);
		new_task->stamp = task_stamp++;
		new_task->max_step = pp_data->roadmap.max_step;
		new_task->current_step = 0;
		new_task->done_func = pp_data->done_func;
		new_task->done_user_data = pp_data->done_user_data;
		tbm_surface_internal_ref(main_buffer->src);
		new_task->buffers[0].src = main_buffer->src;
		tbm_surface_internal_ref(main_buffer->dst);
		new_task->buffers[new_task->max_step-1].dst = main_buffer->dst;

#if 0
		if (new_task->max_step > 1) {
			tbm_surface_info_s src_buf_info;
			tbm_surface_info_s dst_buf_info;
			tbm_surface_get_info(main_buffer->src, &src_buf_info);
			tbm_surface_get_info(main_buffer->dst ,&dst_buf_info);
			unsigned int max_width = (src_buf_info.width > dst_buf_info.width) ? src_buf_info.width : dst_buf_info.width;
			unsigned int max_height = (src_buf_info.height > dst_buf_info.height) ? src_buf_info.height : dst_buf_info.height;
			tbm_format temp_buf_fmt = dst_buf_info.format;
			for (i = 1; i < new_task->max_step; i++) {
				new_task->buffers[i-1].dst = tbm_surface_create (max_width, max_height, temp_buf_fmt);
				tbm_surface_internal_ref(new_task->buffers[i-1].dst);
				new_task->buffers[i].src = new_task->buffers[i-1].dst;
			}
		}
#endif
		for (i = 1; i < new_task->max_step; i++) {
			new_task->buffers[i-1].dst = tbm_surface_create (pp_data->roadmap.step_info[i-1].dst_config.size.h,
															 pp_data->roadmap.step_info[i-1].dst_config.size.v,
															 pp_data->roadmap.step_info[i-1].dst_config.format);
			tbm_surface_internal_ref(new_task->buffers[i-1].dst);
			new_task->buffers[i].src = new_task->buffers[i-1].dst;
			}
		LIST_ADDTAIL(&new_task->link, &pp_data->pending_tasks_list);
		TDM_DBG("pp %p(%d). Add new src %p dst %p buffer", pp_data, pp_data->stamp, main_buffer->src, main_buffer->dst);
		TDM_DBG("To New task %p(%d)", new_task, new_task->stamp);
		free(main_buffer);
		main_buffer = NULL;
	}
	return 1;
}

static tdm_error
_tdm_sprd_pp_worker (tdm_sprd_pp_data *pp_data)
{
	tdm_sprd_pp_task *next_task = NULL, *done_task = pp_data->current_task_p;
	if (done_task) {
		if (done_task->status == TASK_DONE) {
			++(done_task->current_step);
			if (done_task->current_step < done_task->max_step) {
				TDM_DBG("pp %p(%d). Task %p(%d) setup next step %d of %d",
						pp_data, pp_data->stamp, done_task, done_task->stamp,
						done_task->current_step+1, done_task->max_step);
				if (_tdm_sprd_pp_queue(pp_data, done_task->prop_id[done_task->current_step],
						done_task->buffers[done_task->current_step].src,
						done_task->buffers[done_task->current_step].dst,
						IPP_BUF_ENQUEUE) != TDM_ERROR_NONE) {
					return TDM_ERROR_OPERATION_FAILED;
				}
				done_task->status = TASK_CONVERTING;
				if (_tdm_sprd_pp_cmd(pp_data,
									 done_task->prop_id[done_task->current_step],
									 IPP_RUN) != TDM_ERROR_NONE) {
					return TDM_ERROR_OPERATION_FAILED;
				}
				return TDM_ERROR_NONE;
			}
			pp_data->current_task_p = NULL;
/*
			if (done_task->done_func) {
					TDM_DBG("pp %p(%d). Return src %p dst %p", pp_data, pp_data->stamp,
							done_task->buffers[0].src, done_task->buffers[done_task->max_step-1].dst);
					done_task->done_func(pp_data, done_task->buffers[0].src,
										 done_task->buffers[done_task->max_step-1].dst,
										 done_task->done_user_data);
				}
			else {
				TDM_WRN("pp %p(%d). No done func", pp_data, pp_data->stamp);
			}
*/
			_tdm_sprd_pp_destroy_task(pp_data, done_task);
		}
		else {
			TDM_INFO("pp %p(%d). Still converting, add task to queue", pp_data, pp_data->stamp);
			return TDM_ERROR_NONE;
		}
	}

	if (pp_data->roadmap_changed == 1) {
		tdm_sprd_pp_task *temp_task = NULL, *temp_task_next = NULL;
		int i = 0;
		LIST_FOR_EACH_ENTRY_SAFE(temp_task, temp_task_next, &pp_data->pending_tasks_list, link) {
			LIST_DEL(&temp_task->link);
			_tdm_sprd_pp_destroy_task(pp_data, temp_task);
		}
		_tdm_sprd_pp_roadmap_copy(&pp_data->roadmap, &pp_data->new_roadmap);
		for (i = 0; i < pp_data->roadmap.max_step; i++) {
			pp_data->roadmap.prop_id[i] = _tdm_sprd_pp_set(pp_data, &pp_data->roadmap.step_info[i], pp_data->roadmap.prop_id[i]);
			if (pp_data->roadmap.prop_id[i] <= 0) {
				TDM_ERR("pp %p(%d). Can't setup converter", pp_data, pp_data->stamp);
				return TDM_ERROR_BAD_REQUEST;
			}
		}
		pp_data->roadmap_changed = 0;
		_tdm_sprd_pp_roadmap_print(&pp_data->roadmap);
		if (pp_data->new_buffers == 1) {
			if (_tdm_sprd_pp_make_new_tasks(pp_data) < 0) {
				TDM_ERR("pp %p(%d). Can't create new task", pp_data, pp_data->stamp);
				return TDM_ERROR_BAD_REQUEST;
			}
			pp_data->new_buffers = 0;
		}
	}

	if (!LIST_IS_EMPTY(&pp_data->pending_tasks_list)) {
		next_task = (tdm_sprd_pp_task * )container_of(pp_data->pending_tasks_list.next, next_task, link);
		LIST_DEL(&next_task->link);
	}
	if (next_task) {
		TDM_DBG("pp %p(%d). Task %p(%d) setup next step %d of %d",
				pp_data, pp_data->stamp, next_task, next_task->stamp,
				next_task->current_step+1, next_task->max_step);
		if (_tdm_sprd_pp_queue(pp_data, next_task->prop_id[next_task->current_step],
							   next_task->buffers[next_task->current_step].src,
							   next_task->buffers[next_task->current_step].dst,
							   IPP_BUF_ENQUEUE) != TDM_ERROR_NONE) {
			return TDM_ERROR_OPERATION_FAILED;
		}
		if (_tdm_sprd_pp_cmd(pp_data,
							 next_task->prop_id[next_task->current_step],
							 IPP_RUN) != TDM_ERROR_NONE) {
			return TDM_ERROR_OPERATION_FAILED;
		}
		next_task->status = TASK_CONVERTING;
		pp_data->current_task_p = next_task;
		return TDM_ERROR_NONE;
	}
	else {
		TDM_DBG("pp %p(%d). Nothing to do", pp_data, pp_data->stamp);
	}
	return TDM_ERROR_NONE;
}


void
tdm_sprd_pp_handler(struct drm_sprd_ipp_event *hw_ipp_p)
{
	RETURN_VOID_IF_FAIL(hw_ipp_p);
	tdm_sprd_pp_data *pp_data = (tdm_sprd_pp_data *)(unsigned long) hw_ipp_p->user_data;
	tdm_sprd_pp_task *done_task = NULL;
	if (!pp_data || !hw_ipp_p->buf_id) {
		TDM_ERR("invalid params");
		return;
	}
	RETURN_VOID_IF_FAIL(_tdm_sprd_pp_check_struct(pp_data));
	TDM_DBG("pp %p(%d) index(%d, %d) prop_id(%u)", pp_data, pp_data->stamp, hw_ipp_p->buf_id[0],
			hw_ipp_p->buf_id[1], hw_ipp_p->prop_id);
	if (!pp_data->first_event) {
		TDM_DBG("pp %p(%d) got a first event. ", pp_data, pp_data->stamp);
		pp_data->first_event = 1;
	}
	if ((done_task = pp_data->current_task_p) == NULL) {
		TDM_ERR("pp %p(%d) received wrong event", pp_data, pp_data->stamp);
		return;
	}
	if (done_task->prop_id[done_task->current_step] != hw_ipp_p->prop_id) {
		TDM_ERR("pp %p(%d) received wrong event. prop_id expected %u prop_id received %u", pp_data, pp_data->stamp,
				done_task->prop_id[done_task->current_step],
				hw_ipp_p->prop_id);
		return;
	}
	done_task->status = TASK_DONE;
	TDM_DBG("pp %p(%d). Task %p(%d) done step %d of %d", pp_data, pp_data->stamp, done_task, done_task->stamp,
			done_task->current_step+1, done_task->max_step);
	if (_tdm_sprd_pp_cmd(pp_data,
						 done_task->prop_id[done_task->current_step],
						 IPP_PAUSE) != TDM_ERROR_NONE) {
		TDM_ERR("pp %p(%d). Can't PAUSE conveter. Prop_id %d", pp_data,
				pp_data->stamp, done_task->prop_id[done_task->current_step]);
		return;
	}
	if (_tdm_sprd_pp_worker(pp_data) != TDM_ERROR_NONE) {
		TDM_ERR("pp %p(%d) worker return ERROR", pp_data,
				pp_data->stamp);
	}
}

tdm_error
tdm_sprd_pp_get_capability(tdm_sprd_data *sprd_data, tdm_caps_pp *caps)
{
	int i;

	if (!caps) {
		TDM_ERR("invalid params");
		return TDM_ERROR_INVALID_PARAMETER;
	}

	caps->capabilities = TDM_PP_CAPABILITY_ASYNC;

	caps->format_count = NUM_PP_FORMAT;
	caps->formats = NULL;
	if (NUM_PP_FORMAT) {
		/* will be freed in frontend */
		caps->formats = calloc(1, sizeof pp_formats);
		if (!caps->formats) {
			TDM_ERR("alloc failed");
			return TDM_ERROR_OUT_OF_MEMORY;
		}
		for (i = 0; i < caps->format_count; i++)
			caps->formats[i] = pp_formats[i];
	}

	caps->min_w = 16;
	caps->min_h = 8;
	caps->max_w = -1;   /* not defined */
	caps->max_h = -1;
	caps->preferred_align = 2;

	return TDM_ERROR_NONE;
}

tdm_pp *
tdm_sprd_pp_create(tdm_sprd_data *sprd_data, tdm_error *error)
{
	tdm_sprd_pp_data *pp_data = calloc(1, sizeof(tdm_sprd_pp_data));
	if (!pp_data) {
		TDM_ERR("alloc failed");
		if (error)
			*error = TDM_ERROR_OUT_OF_MEMORY;
		return NULL;
	}

	pp_data->sprd_data = sprd_data;
	pp_data->stamp = pp_stamp++;
	if (!pp_list_init) {
		pp_list_init = 1;
		LIST_INITHEAD(&pp_list);
	}
	LIST_INITHEAD(&pp_data->pending_buffer_list);
	LIST_INITHEAD(&pp_data->pending_tasks_list);
	LIST_INITHEAD(&pp_data->prop_id_list);
	LIST_ADDTAIL(&pp_data->link, &pp_list);
	TDM_DBG("pp %p(%d). Create", pp_data, pp_data->stamp);
	return pp_data;
}

void
sprd_pp_destroy(tdm_pp *pp)
{
	tdm_sprd_pp_data *pp_data = (tdm_sprd_pp_data *) pp;
	tdm_sprd_pp_buffer *b = NULL, *bb = NULL;
	tdm_sprd_pp_task *task = NULL, *next_task = NULL;
	tdm_sprd_prop_id *prop_id = NULL, *next_prop_id = NULL;
	RETURN_VOID_IF_FAIL(_tdm_sprd_pp_check_struct(pp_data));
	TDM_DBG("pp %p(%d). Destroy", pp_data, pp_data->stamp);
	LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->pending_buffer_list, link) {
		LIST_DEL(&b->link);
		free(b);
	}
	if (pp_data->current_task_p && pp_data->current_task_p->status == TASK_CONVERTING) {
		_tdm_sprd_pp_queue(pp_data, pp_data->current_task_p->prop_id[pp_data->current_task_p->current_step],
						   pp_data->current_task_p->buffers[pp_data->current_task_p->current_step].src,
						   pp_data->current_task_p->buffers[pp_data->current_task_p->current_step].dst, IPP_BUF_DEQUEUE);
		_tdm_sprd_pp_destroy_task(pp_data, pp_data->current_task_p);
	}
	LIST_FOR_EACH_ENTRY_SAFE(task, next_task, &pp_data->pending_tasks_list, link) {
		LIST_DEL(&task->link);
		_tdm_sprd_pp_destroy_task(pp_data, task);
	}
	LIST_FOR_EACH_ENTRY_SAFE (prop_id, next_prop_id, &pp_data->prop_id_list, link) {
		_tdm_sprd_pp_cmd(pp_data, prop_id->prop_id, IPP_STOP);
	}
	LIST_DEL(&pp_data->link);
	free(pp_data);
}

static tdm_error
_sprd_pp_get_scale_leap(unsigned int src, unsigned int dst,
						unsigned int *leap_array, unsigned int *size)
{
	unsigned int i = 0;
	unsigned int ratio, next_value = src;
	TDM_DBG("pp. scale src %u", src);
	for (i = 0; i < PP_MAX_STEP; i++) {
		ratio = PP_RATIO(next_value, dst);
		if ((ratio >= PP_UP_MAX_RATIO) && (ratio <= PP_DOWN_MIN_RATIO))
			break;
		if (ratio < PP_UP_MAX_RATIO) {
			next_value = PP_RATIO(next_value, PP_UP_MAX_RATIO);
		}
		else if (ratio > PP_DOWN_MIN_RATIO) {
			next_value =  PP_RATIO(next_value, PP_DOWN_MIN_RATIO);
		}
		if (leap_array)
			leap_array[i] = next_value;
		TDM_DBG("[%u] => %u", i, next_value);
	}
	if (i == PP_MAX_STEP) {
		TDM_ERR("Can't scale. Reaching maximum iteration count %d", PP_MAX_STEP);
		return TDM_ERROR_OPERATION_FAILED;
	}
	TDM_DBG("[%u] dst => %u", i, dst);
	if (leap_array)
		leap_array[i] = dst;
	if (size)
		*size = i + 1;
	return TDM_ERROR_NONE;
}

static tdm_error
_sprd_pp_make_roadmap(tdm_sprd_pp_data *pp_data, tdm_info_pp *info)
{
	unsigned int height_leap[PP_MAX_STEP], width_leap[PP_MAX_STEP],
			 height_leap_size = 0, width_leap_size = 0, max_size = 0, i,
			 src_height = (info->transform % 2) ? info->src_config.pos.w : info->src_config.pos.h,
			 src_width = (info->transform % 2) ? info->src_config.pos.h : info->src_config.pos.w;
	TDM_DBG("Height %u", info->src_config.pos.h);
	if (_sprd_pp_get_scale_leap(src_height,
								info->dst_config.pos.h,
								height_leap,
								&height_leap_size) != TDM_ERROR_NONE) {
		TDM_ERR("height %u -> %u ratio out of range", info->src_config.pos.h,
				info->dst_config.pos.h);
		return TDM_ERROR_OPERATION_FAILED;
	}
	TDM_DBG("Width %u", info->src_config.pos.w);
	if (_sprd_pp_get_scale_leap(src_width,
								info->dst_config.pos.w,
								width_leap,
								&width_leap_size) != TDM_ERROR_NONE) {
		TDM_ERR("width %u -> %u ratio out of range", info->src_config.pos.w,
				info->dst_config.pos.w);
		return TDM_ERROR_OPERATION_FAILED;
	}
	max_size = (width_leap_size > height_leap_size ? width_leap_size :
				height_leap_size);
	if ((src_height < height_leap[height_leap_size -1] &&
		src_width > width_leap[width_leap_size -1]) ||
		(src_height > height_leap[height_leap_size -1] &&
		 src_width < width_leap[width_leap_size - 1])) {
		if (max_size > 1 || max_size == PP_MAX_STEP) {
			TDM_ERR ("pp %p(%d). Unsupported scale", pp_data, pp_data->stamp);
			return TDM_ERROR_OPERATION_FAILED;
		}
		height_leap[1] = height_leap[0];
		height_leap[0] = src_height;
		height_leap_size = 2;
/*
		width_leap[1] = width_leap[0];
		width_leap[0] = src_width;
		width_leap_size = 2;
*/
	}
	memcpy(&pp_data->new_roadmap.step_info[0], info, sizeof(tdm_info_pp));
	pp_data->new_roadmap.step_info[0].dst_config.pos.h = height_leap[0];
	pp_data->new_roadmap.step_info[0].dst_config.pos.w = width_leap[0];
	max_size = (width_leap_size > height_leap_size ? width_leap_size :
				height_leap_size);
	for (i = 1; i < max_size; i++) {
		pp_data->new_roadmap.step_info[i - 1].dst_config.pos.w =
								width_leap[(((i - 1) < width_leap_size) ? (i - 1) : (width_leap_size - 1))];
		pp_data->new_roadmap.step_info[i - 1].dst_config.pos.h =
								height_leap[(((i - 1) < height_leap_size) ? (i - 1) : (height_leap_size - 1))];
		pp_data->new_roadmap.step_info[i - 1].dst_config.size.h = pp_data->new_roadmap.step_info[i - 1].dst_config.pos.w;
		pp_data->new_roadmap.step_info[i - 1].dst_config.size.v = pp_data->new_roadmap.step_info[i - 1].dst_config.pos.h;
		pp_data->new_roadmap.step_info[i].transform = TDM_TRANSFORM_NORMAL;
		pp_data->new_roadmap.step_info[i].src_config.format = pp_data->new_roadmap.step_info[i - 1].dst_config.format;
		pp_data->new_roadmap.step_info[i].dst_config.format = pp_data->new_roadmap.step_info[i - 1].dst_config.format;
		pp_data->new_roadmap.step_info[i].src_config = pp_data->new_roadmap.step_info[i - 1].dst_config;
		pp_data->new_roadmap.step_info[i].dst_config = pp_data->new_roadmap.step_info[i - 1].dst_config;
		pp_data->new_roadmap.step_info[i].src_config.pos.x = 0;
		pp_data->new_roadmap.step_info[i].src_config.pos.y = 0;
		pp_data->new_roadmap.step_info[i].dst_config.pos.x = 0;
		pp_data->new_roadmap.step_info[i].dst_config.pos.y = 0;
		pp_data->new_roadmap.step_info[i].sync = pp_data->new_roadmap.step_info[i - 1].sync;
		pp_data->new_roadmap.step_info[i].flags = pp_data->new_roadmap.step_info[i - 1].flags;
	}
	memcpy(&pp_data->new_roadmap.step_info[max_size - 1].dst_config, &info->dst_config, sizeof(tdm_info_config));
	pp_data->new_roadmap.step_info[max_size-1].dst_config.pos.h = height_leap[height_leap_size - 1];
	pp_data->new_roadmap.step_info[max_size-1].dst_config.pos.w = width_leap[width_leap_size - 1];
	pp_data->new_roadmap.max_step = max_size;
	//_sprd_pp_make_roadmap_transform (pp_data, info);
	return TDM_ERROR_NONE;
}

tdm_error
sprd_pp_set_info(tdm_pp *pp, tdm_info_pp *info)
{
	tdm_sprd_pp_data *pp_data = (tdm_sprd_pp_data *) pp;
	RETURN_VAL_IF_FAIL(info, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(_tdm_sprd_pp_check_struct(pp_data), TDM_ERROR_INVALID_PARAMETER);
	TDM_DBG("pp %p(%d). Set new info.", pp_data, pp_data->stamp);
	if (info->sync) {
		TDM_ERR("pp %p(%d). not support sync mode currently", pp_data, pp_data->stamp);
		return TDM_ERROR_INVALID_PARAMETER;
	}
	if (_sprd_pp_make_roadmap(pp_data, info) != TDM_ERROR_NONE) {
		TDM_ERR("pp %p(%d). Wrong convertation settings", pp_data, pp_data->stamp);
		return TDM_ERROR_INVALID_PARAMETER;
	}
	pp_data->roadmap_changed = 1;
	return TDM_ERROR_NONE;
}

tdm_error
sprd_pp_attach(tdm_pp *pp, tbm_surface_h src, tbm_surface_h dst)
{
	tdm_sprd_pp_data *pp_data = (tdm_sprd_pp_data *) pp;
	tdm_sprd_pp_buffer *buffer;
	RETURN_VAL_IF_FAIL(src, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(dst, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(_tdm_sprd_pp_check_struct(pp_data), TDM_ERROR_INVALID_PARAMETER);
	buffer = calloc(1, sizeof(tdm_sprd_pp_buffer));
	if (!buffer) {
		TDM_ERR("pp %p(%d). Alloc failed", pp_data, pp_data->stamp);
		return TDM_ERROR_NONE;
	}
	buffer->index = 0;
	buffer->src = src;
	buffer->dst = dst;
	pp_data->new_buffers = 1;
	TDM_DBG("pp %p(%d). Attach src %p dst %p buffers", pp_data, pp_data->stamp, buffer->src, buffer->dst);
	LIST_ADDTAIL(&buffer->link, &pp_data->pending_buffer_list);

	return TDM_ERROR_NONE;
}



tdm_error
sprd_pp_commit(tdm_pp *pp)
{
	tdm_sprd_pp_data *pp_data = (tdm_sprd_pp_data *) pp;
	RETURN_VAL_IF_FAIL(_tdm_sprd_pp_check_struct(pp_data), TDM_ERROR_INVALID_PARAMETER);
	if (pp_data->roadmap_changed) {
		unsigned int i, need_postpond = 0;
		for (i = 0; i < pp_data->roadmap.max_step; i++) {
			if (pp_data->roadmap.prop_id[i] > 0) {
				tdm_sprd_prop_id * found_prop = _find_prop_id(pp_data, pp_data->roadmap.prop_id[i]);
				if (found_prop != NULL &&
						(found_prop->status == IPP_CTRL_PLAY || found_prop->status == IPP_CTRL_RESUME)) {
//					_tdm_sprd_pp_cmd(pp_data, pp_data->roadmap.prop_id[i], IPP_PAUSE);
					need_postpond = 1;
				}
			}
		}
		if (!need_postpond) {
			_tdm_sprd_pp_roadmap_copy(&pp_data->roadmap, &pp_data->new_roadmap);
			for (i = 0; i < pp_data->roadmap.max_step; i++) {
				pp_data->roadmap.prop_id[i] = _tdm_sprd_pp_set(pp_data, &pp_data->roadmap.step_info[i], pp_data->roadmap.prop_id[i]);
				if (pp_data->roadmap.prop_id[i] <= 0) {
					TDM_ERR("pp %p(%d). Can't setup converter", pp_data, pp_data->stamp);
					return TDM_ERROR_BAD_REQUEST;
				}
			}
			pp_data->roadmap_changed = 0;
			_tdm_sprd_pp_roadmap_print(&pp_data->roadmap);
		}
	}
	if (pp_data->new_buffers == 1 && pp_data->roadmap_changed == 0) {
		if (_tdm_sprd_pp_make_new_tasks(pp_data) < 0) {
			TDM_ERR("pp %p(%d). Can't create new task", pp_data, pp_data->stamp);
			return TDM_ERROR_BAD_REQUEST;
		}
		pp_data->new_buffers = 0;
		return _tdm_sprd_pp_worker(pp_data);
	}
	return TDM_ERROR_NONE;
}

tdm_error
sprd_pp_set_done_handler(tdm_pp *pp, tdm_pp_done_handler func, void *user_data)
{
	tdm_sprd_pp_data *pp_data = (tdm_sprd_pp_data *) pp;
	RETURN_VAL_IF_FAIL(func, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(_tdm_sprd_pp_check_struct(pp_data), TDM_ERROR_INVALID_PARAMETER);
	TDM_DBG("pp %p(%d). Set done handler func %p", pp_data, pp_data->stamp, func);
	pp_data->done_func = func;
	pp_data->done_user_data = user_data;
	return TDM_ERROR_NONE;
}
