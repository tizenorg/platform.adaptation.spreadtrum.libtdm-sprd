#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <tdm_helper.h>
#include <linux/fb.h>
#include <video/sprdfb.h>
#include <tbm_surface.h>
#include "tdm_sprd.h"

#define MIN_WIDTH   32
#define LAYER_COUNT_PER_OUTPUT   2
#ifdef HAVE_FB_VBLANK
/** @TODO fb event struct */
#else
typedef struct drm_event hw_event_t;
#endif

#define FB_DEV_LCD  "/dev/fb0"

typedef struct _tdm_sprd_output_data tdm_sprd_output_data;
typedef struct _tdm_sprd_layer_data tdm_sprd_layer_data;
typedef struct _tdm_sprd_vblank_data_s tdm_sprd_vblank_data;

typedef enum {
        VBLANK_TYPE_WAIT,
        VBLANK_TYPE_COMMIT,
} vblank_type_t;

struct _tdm_sprd_vblank_data_s {
	vblank_type_t type;
	tdm_sprd_output_data *output_data;
	void *user_data;
};

typedef struct _tdm_sprd_display_buffer {
	struct list_head link;

	unsigned int fb_id;
	tbm_surface_h buffer;
	int width;
	unsigned int height;
	unsigned int format;
	unsigned int handles[4];
	unsigned int name[4];
	unsigned int pitches[4];
	unsigned int offsets[4];
	unsigned int size;
	unsigned int count;
} tdm_sprd_display_buffer;

struct _tdm_sprd_output_data {
	struct list_head link;

	/* data which are fixed at initializing */
	tdm_sprd_data *sprd_data;
	uint32_t pipe;
	int count_modes;
	tdm_output_mode *output_modes;
	tdm_output_type connector_type;
	unsigned int connector_type_id;
	struct list_head layer_list;

	tdm_output_vblank_handler vblank_func;
	tdm_output_commit_handler commit_func;
	tdm_output_conn_status status;

	int mode_changed;
	const tdm_output_mode *current_mode;

	int waiting_vblank_event;

	char *fb_fd_name;
	int fb_fd;

	struct fb_var_screeninfo mi;

	tdm_output_dpms dpms_value;

};

struct _tdm_sprd_layer_data {
	struct list_head link;

	/* data which are fixed at initializing */
	tdm_sprd_data *sprd_data;
	tdm_sprd_output_data *output_data;
	tdm_layer_capability capabilities;
	int zpos;

	//list of sprd formats
	int format_count;
	tbm_format *formats;

	/* not fixed data below */
	tdm_info_layer info;
	int info_changed;

	tdm_sprd_display_buffer *display_buffer;
	int display_buffer_changed;
	// current hw overlay setting
	overlay_info ovi;
	int enabled_flag;
};

typedef struct _Drm_Event_Context {
	void (*vblank_handler)(int fd, unsigned int sequence, unsigned int tv_sec,
	                       unsigned int tv_usec, void *user_data);
	void (*pp_handler)(int fd, unsigned int  prop_id, unsigned int *buf_idx,
	                   unsigned int  tv_sec, unsigned int  tv_usec, void *user_data);
} Drm_Event_Context;

tbm_format img_layer_formats[] = {
	TBM_FORMAT_RGB565,
	TBM_FORMAT_XRGB8888,
	TBM_FORMAT_ARGB8888,
	TBM_FORMAT_NV12,
	TBM_FORMAT_YUV420
};

tbm_format osd_layer_formats[] = {
	TBM_FORMAT_RGB565,
	TBM_FORMAT_XRGB8888,
	TBM_FORMAT_ARGB8888
};

#if 0
static tdm_error
check_hw_restriction(unsigned int output_w, unsigned int buf_w,
                     unsigned int src_x, unsigned int src_w, unsigned int dst_x, unsigned int dst_w,
                     unsigned int *new_src_x, unsigned int *new_src_w,
                     unsigned int *new_dst_x, unsigned int *new_dst_w)
{
	int start, end, diff;
	int virtual_screen;

	*new_src_x = src_x;
	*new_src_w = src_w;
	*new_dst_x = dst_x;
	*new_dst_w = dst_w;

	if (buf_w < MIN_WIDTH || buf_w % 2) {
		TDM_ERR("buf_w(%d) not 2's multiple or less than %d", buf_w, MIN_WIDTH);
		return TDM_ERROR_BAD_REQUEST;
	}

	if (src_x > dst_x || ((dst_x - src_x) + buf_w) > output_w)
		virtual_screen = 1;
	else
		virtual_screen = 0;

	start = (dst_x < 0) ? 0 : dst_x;
	end = ((dst_x + dst_w) > output_w) ? output_w : (dst_x + dst_w);

	/* check window minimun width */
	if ((end - start) < MIN_WIDTH) {
		TDM_ERR("visible_w(%d) less than %d", end - start, MIN_WIDTH);
		return TDM_ERROR_BAD_REQUEST;
	}

	if (!virtual_screen) {
		/* Pagewidth of window (= 8 byte align / bytes-per-pixel ) */
		if ((end - start) % 2)
			end--;
	} else {
		/* You should align the sum of PAGEWIDTH_F and OFFSIZE_F double-word (8 byte) boundary. */
		if (end % 2)
			end--;
	}

	*new_dst_x = start;
	*new_dst_w = end - start;
	*new_src_w = *new_dst_w;
	diff = start - dst_x;
	*new_src_x += diff;

	RETURN_VAL_IF_FAIL(*new_src_w > 0, TDM_ERROR_BAD_REQUEST);
	RETURN_VAL_IF_FAIL(*new_dst_w > 0, TDM_ERROR_BAD_REQUEST);

	if (src_x != *new_src_x || src_w != *new_src_w || dst_x != *new_dst_x ||
	    dst_w != *new_dst_w)
		TDM_DBG("=> buf_w(%d) src(%d,%d) dst(%d,%d), virt(%d) start(%d) end(%d)",
		        buf_w, *new_src_x, *new_src_w, *new_dst_x, *new_dst_w, virtual_screen, start,
		        end);

	return TDM_ERROR_NONE;
}
#endif


static tdm_sprd_display_buffer *
_tdm_sprd_display_find_buffer(tdm_sprd_data *sprd_data, tbm_surface_h buffer)
{
	tdm_sprd_display_buffer *display_buffer = NULL;

	LIST_FOR_EACH_ENTRY(display_buffer, &sprd_data->buffer_list, link) {
		if (display_buffer->buffer == buffer)
			return display_buffer;
	}

	return NULL;
}


static inline uint32_t _get_refresh(struct fb_var_screeninfo *timing)
{
	uint32_t pixclock, hfreq, htotal, vtotal;

	pixclock = PICOS2KHZ(timing->pixclock) * 1000;

	htotal = timing->xres + timing->right_margin + timing->hsync_len +
	         timing->left_margin;
	vtotal = timing->yres + timing->lower_margin + timing->vsync_len +
	         timing->upper_margin;

	if (timing->vmode & FB_VMODE_INTERLACED)
		vtotal /= 2;
	if (timing->vmode & FB_VMODE_DOUBLE)
		vtotal *= 2;

	hfreq = pixclock / htotal;
	return hfreq / vtotal;
}

/*
 * Convert fb_var_screeninfo to tdm_output_mode
 */
static inline void _tdm_sprd_display_to_tdm_mode(struct fb_var_screeninfo
                *timing, tdm_output_mode  *mode)
{

	if (!timing->pixclock)
		return;

	mode->clock = timing->pixclock / 1000;
	mode->vrefresh = _get_refresh(timing);
	mode->hdisplay = timing->xres;
	mode->hsync_start = mode->hdisplay + timing->right_margin;
	mode->hsync_end = mode->hsync_start + timing->hsync_len;
	mode->htotal = mode->hsync_end + timing->left_margin;

	mode->vdisplay = timing->yres;
	mode->vsync_start = mode->vdisplay + timing->lower_margin;
	mode->vsync_end = mode->vsync_start + timing->vsync_len;
	mode->vtotal = mode->vsync_end + timing->upper_margin;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	if (timing->vmode & FB_VMODE_INTERLACED)
		mode->flags |= DRM_MODE_FLAG_INTERLACE;

	if (timing->vmode & FB_VMODE_DOUBLE)
		mode->flags |= DRM_MODE_FLAG_DBLSCAN;

	int interlaced = !!(mode->flags & DRM_MODE_FLAG_INTERLACE);
	snprintf(mode->name, DRM_DISPLAY_MODE_LEN, "%dx%d%s", mode->hdisplay,
	         mode->vdisplay,
	         interlaced ? "i" : "");
}

static int
_localdrmWaitVBlank(int fd, drmVBlank *vbl)
{
	struct timespec timeout, cur;
	int ret;

	ret = clock_gettime(CLOCK_MONOTONIC, &timeout);
	if (ret < 0) {
		TDM_ERR("clock_gettime failed: %s", strerror(errno));
		goto out;
	}
	timeout.tv_sec++;

	do {
		ret = ioctl(fd, DRM_IOCTL_WAIT_VBLANK, vbl);
		vbl->request.type &= ~DRM_VBLANK_RELATIVE;
		if (ret && errno == EINTR) {
			clock_gettime(CLOCK_MONOTONIC, &cur);
			/* Timeout after 1s */
			if (cur.tv_sec > timeout.tv_sec + 1 ||
			    (cur.tv_sec == timeout.tv_sec && cur.tv_nsec >=
			     timeout.tv_nsec)) {
				errno = EBUSY;
				ret = -1;
				break;
			}
		}
	} while (ret && errno == EINTR);

out:
	return ret;
}

static tdm_error
_tdm_sprd_display_get_cur_msc (int fd, int pipe, uint *msc)
{
	drmVBlank vbl;

	vbl.request.type = DRM_VBLANK_RELATIVE;
	if (pipe > 0)
		vbl.request.type |= DRM_VBLANK_SECONDARY;

	vbl.request.sequence = 0;
	if (_localdrmWaitVBlank(fd, &vbl)) {
		TDM_ERR("get vblank counter failed: %m");
		*msc = 0;
		return TDM_ERROR_OPERATION_FAILED;
	}

	*msc = vbl.reply.sequence;

	return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_wait_vblank(int fd, int pipe, uint *target_msc, void *data)
{
	drmVBlank vbl;

	vbl.request.type =  DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT;
	if (pipe > 0)
		vbl.request.type |= DRM_VBLANK_SECONDARY;

	vbl.request.sequence = *target_msc;
	vbl.request.signal = (unsigned long)(uintptr_t)data;

	if (_localdrmWaitVBlank(fd, &vbl)) {
		*target_msc = 0;
		TDM_ERR("wait vblank failed: %m");
		return TDM_ERROR_OPERATION_FAILED;
	}

	*target_msc = vbl.reply.sequence;

	return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_output_disable (tdm_sprd_output_data *output_data)
{
	tdm_sprd_layer_data *layer_data = NULL;
	LIST_FOR_EACH_ENTRY(layer_data, &output_data->layer_list, link) {
		memset(&layer_data->ovi, 0, sizeof(overlay_info));
	}
	TDM_DBG ("FB_BLANK_POWERDOWN\n");
	if (ioctl (output_data->fb_fd, FBIOBLANK, FB_BLANK_POWERDOWN) < 0) {
		TDM_ERR ("FB_BLANK_POWERDOWN is failed: %s\n", strerror (errno));
		return TDM_ERROR_OPERATION_FAILED;
	}
	return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_output_enable(tdm_sprd_output_data *output_data)
{
	TDM_DBG ("FB_BLANK_UNBLANK\n");
	tdm_sprd_layer_data *layer_data = NULL;
	LIST_FOR_EACH_ENTRY(layer_data, &output_data->layer_list, link) {
		memset(&layer_data->ovi, 0, sizeof(overlay_info));
	}
	if (ioctl (output_data->fb_fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) {
		TDM_ERR ("FB_BLANK_UNBLANK is failed: %s\n", strerror (errno));
		return TDM_ERROR_OPERATION_FAILED;
	}
	return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_layer_disable(tdm_layer *layer)
{
	tdm_sprd_layer_data *layer_data = layer;
	int layer_index = 0;
	RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_OPERATION_FAILED);
	if (layer_data->capabilities & TDM_LAYER_CAPABILITY_PRIMARY) {
		layer_index = SPRD_LAYER_OSD;
	} else if (layer_data->capabilities & TDM_LAYER_CAPABILITY_OVERLAY) {
		layer_index = SPRD_LAYER_IMG;
	} else {
		TDM_ERR ("layer capability (0x%x) not supported\n", layer_data->capabilities);
		return TDM_ERROR_OPERATION_FAILED;
	}
	if (layer_data->enabled_flag) {
		TDM_DBG ("SPRD_FB_UNSET_OVERLAY(%d)\n", layer_index);
		if (ioctl(layer_data->output_data->fb_fd, SPRD_FB_UNSET_OVERLAY,
		          &layer_index) == -1) {
			TDM_ERR ("SPRD_FB_UNSET_OVERLAY(%d) error:%s\n", layer_index, strerror (errno));
		}
		memset(&layer_data->ovi, 0, sizeof(overlay_info));
		layer_data->enabled_flag = 0;
	}
	return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_tbmformat_to_sprdformat(int tbm_format, overlay_info *ovi)
{
	ovi->rb_switch = 0;
	ovi->data_type = 0;
	ovi->endian.y  = 0;
	ovi->endian.u  = 0;
	ovi->endian.v  = 0;

	switch (tbm_format) {
	case TBM_FORMAT_RGB565:
		ovi->data_type = SPRD_DATA_FORMAT_RGB565;
		ovi->endian.y = SPRD_DATA_ENDIAN_B0B1B2B3;
		ovi->endian.u = SPRD_DATA_ENDIAN_B0B1B2B3;
		break;
	case TBM_FORMAT_ARGB8888:
	case TBM_FORMAT_XRGB8888:
		ovi->data_type = SPRD_DATA_FORMAT_RGB888;
		ovi->endian.y  = SPRD_DATA_ENDIAN_B0B1B2B3;
		ovi->endian.u  = SPRD_DATA_ENDIAN_B0B1B2B3;
		break;
	case TBM_FORMAT_NV12:
		ovi->data_type = SPRD_DATA_FORMAT_YUV420;
		ovi->endian.y  = SPRD_DATA_ENDIAN_B3B2B1B0;
		ovi->endian.u  = SPRD_DATA_ENDIAN_B3B2B1B0;
		break;
	case TBM_FORMAT_YUV420:
		ovi->data_type = SPRD_DATA_FORMAT_YUV420_3P;
		ovi->endian.y  = SPRD_DATA_ENDIAN_B3B2B1B0;
		ovi->endian.u  = SPRD_DATA_ENDIAN_B3B2B1B0;
		ovi->endian.v  = SPRD_DATA_ENDIAN_B3B2B1B0;
		break;
	default:
		return TDM_ERROR_INVALID_PARAMETER;
	}
	return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_do_commit(tdm_sprd_output_data *output_data)
{
	tdm_error res = TDM_ERROR_NONE;
	tdm_sprd_layer_data *layer_data;
	overlay_info ovi;
	overlay_display ov_disp = {0,};
	int layer_index = 0;

	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_OPERATION_FAILED);
#if 1
	if (output_data->dpms_value != TDM_OUTPUT_DPMS_ON) {
		output_data->dpms_value = TDM_OUTPUT_DPMS_ON;
		_tdm_sprd_display_output_enable(output_data);
	}
#endif
	LIST_FOR_EACH_ENTRY(layer_data, &output_data->layer_list, link) {
		if (!layer_data->display_buffer_changed && !layer_data->info_changed)
			continue;
		if (layer_data->display_buffer) {
			if (layer_data->capabilities & TDM_LAYER_CAPABILITY_PRIMARY) {
				layer_index = SPRD_LAYER_OSD;
			} else if (layer_data->capabilities & TDM_LAYER_CAPABILITY_OVERLAY) {
				layer_index = SPRD_LAYER_IMG;
			} else {
				TDM_ERR ("layer capability (0x%x) not supported\n", layer_data->capabilities);
				continue;
			}
			tbm_format frmt = layer_data->display_buffer->format;
			if (_tdm_sprd_tbmformat_to_sprdformat(frmt, &ovi) != TDM_ERROR_NONE) {
				ovi.data_type = SPRD_DATA_FORMAT_RGB888;
				ovi.endian.y = SPRD_DATA_ENDIAN_B0B1B2B3;
				ovi.endian.u = SPRD_DATA_ENDIAN_B0B1B2B3;
				ovi.endian.v = SPRD_DATA_ENDIAN_B0B1B2B3;
				TDM_ERR("Unsupported format: %x %c%c%c%c\n", frmt, FOURCC_STR(frmt));
				continue;
			}

			ovi.layer_index = layer_index;
			ovi.size.hsize = layer_data->display_buffer->width;
			ovi.size.vsize = layer_data->display_buffer->height;

			ovi.rect.x = layer_data->info.dst_pos.x;
			ovi.rect.y = layer_data->info.dst_pos.y;

			//restrict width
			int output_width = output_data->mi.xres;
			int width = layer_data->info.src_config.pos.w;
			int x = layer_data->info.dst_pos.x;

			ovi.rect.w = (width + x) <= output_width ? width : output_width - x;
			ovi.rect.h = layer_data->info.src_config.pos.h;

			if (ovi.layer_index == SPRD_LAYER_OSD) {
				ov_disp.osd_handle = layer_data->display_buffer->name[0];
			} else if (ovi.layer_index == SPRD_LAYER_IMG) {
				ov_disp.img_handle = layer_data->display_buffer->name[0];
			}
			if (memcmp(&layer_data->ovi, &ovi, sizeof(overlay_info)) != 0) {
				TDM_DBG("SPRD_FB_SET_OVERLAY(%d) rect:%dx%d+%d+%d size:%dx%d\n",
				        ovi.layer_index,
				        ovi.rect.w, ovi.rect.h, ovi.rect.x, ovi.rect.y, ovi.size.hsize, ovi.size.vsize);
				if (ioctl (layer_data->output_data->fb_fd, SPRD_FB_SET_OVERLAY, &ovi) == -1) {
					TDM_ERR ("SPRD_FB_SET_OVERLAY(%d) error:%s\n", layer_index, strerror (errno));
					continue;
				}
				memcpy(&layer_data->ovi, &ovi, sizeof(overlay_info));
			}
			layer_data->enabled_flag = 1;
			ov_disp.layer_index |= layer_index;
		} else {
			_tdm_sprd_display_layer_disable(layer_data);
		}
	}

	if (ov_disp.layer_index) {
		ov_disp.display_mode = SPRD_DISPLAY_OVERLAY_SYNC;

		TDM_DBG("SPRD_FB_DISPLAY_OVERLAY(%d) osd_handle:%d img_handle:%d\n",
		        ov_disp.layer_index, ov_disp.osd_handle, ov_disp.img_handle);
		if (ioctl(output_data->fb_fd, SPRD_FB_DISPLAY_OVERLAY, &ov_disp) == -1) {
			TDM_ERR( "SPRD_FB_DISPLAY_OVERLAY(%d) error: %s \n", strerror (errno),
			         ov_disp.layer_index);
			res = TDM_ERROR_OPERATION_FAILED;
		} else {
			//save enable layers
//            enable_layers |= ov_disp.layer_index;
		}
	}

	return res;
}

#if 0
static void
_tdm_sprd_display_cb_pp(int fd, unsigned int prop_id, unsigned int *buf_idx,
                        unsigned int tv_sec, unsigned int tv_usec,
                        void *user_data)
{
	tdm_sprd_pp_handler(prop_id, buf_idx, tv_sec, tv_usec, user_data);
}
#endif
#if 0
static int
_tdm_sprd_display_events_handle(int fd, Drm_Event_Context *evctx)
{
#define MAX_BUF_SIZE    1024

	char buffer[MAX_BUF_SIZE];
	unsigned int len, i;
	struct drm_event *e;

	/* The DRM read semantics guarantees that we always get only
	 * complete events. */
	len = read(fd, buffer, sizeof buffer);
	if (len == 0) {
		TDM_WRN("warning: the size of the drm_event is 0.");
		return 0;
	}
	if (len < sizeof * e) {
		TDM_WRN("warning: the size of the drm_event is less than drm_event structure.");
		return -1;
	}
#if 0
	if (len > MAX_BUF_SIZE - sizeof(struct drm_sprd_ipp_event)) {
		TDM_WRN("warning: the size of the drm_event can be over the maximum size.");
		return -1;
	}
#endif
	i = 0;
	while (i < len) {
		e = (struct drm_event *) &buffer[i];
		switch (e->type) {
		case DRM_EVENT_VBLANK: {
			struct drm_event_vblank *vblank;

			if (evctx->vblank_handler == NULL)
				break;

			vblank = (struct drm_event_vblank *)e;
			TDM_DBG("******* VBLANK *******");
			evctx->vblank_handler (fd, vblank->sequence,
			                       vblank->tv_sec, vblank->tv_usec,
			                       (void *)((unsigned long)vblank->user_data));
			TDM_DBG("******* VBLANK *******...");
		}
		break;
#if 0
		case DRM_EXYNOS_IPP_EVENT: {
			struct drm_sprd_ipp_event *ipp;

			if (evctx->pp_handler == NULL)
				break;

			ipp = (struct drm_sprd_ipp_event *)e;
			TDM_DBG("******* PP *******");
			evctx->pp_handler (fd, ipp->prop_id, ipp->buf_id,
			                   ipp->tv_sec, ipp->tv_usec,
			                   (void *)((unsigned long)ipp->user_data));
			TDM_DBG("******* PP *******...");
		}
#endif
		break;
		case DRM_EVENT_FLIP_COMPLETE:
			/* do nothing for flip complete */
			break;
		default:
			break;
		}
		i += e->length;
	}

	return 0;
}
#endif

static tdm_error
_tdm_sprd_display_create_layer_list_LCD(tdm_sprd_output_data *output_data)
{
	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);

	tdm_sprd_data *sprd_data = output_data->sprd_data;
	tdm_sprd_layer_data *layer_data_osd, *layer_data_img;

	layer_data_osd = calloc (1, sizeof(tdm_sprd_layer_data));
	if (!layer_data_osd) {
		TDM_ERR("alloc failed osd");
		return TDM_ERROR_OUT_OF_MEMORY;
	}
	layer_data_img = calloc (1, sizeof(tdm_sprd_layer_data));
	if (!layer_data_img) {
		TDM_ERR("alloc failed img");
		free(layer_data_osd);
		return TDM_ERROR_OUT_OF_MEMORY;
	}

    /* create OSD layer */
	layer_data_osd->sprd_data = sprd_data;
	layer_data_osd->output_data = output_data;

	layer_data_osd->capabilities = TDM_LAYER_CAPABILITY_PRIMARY |
	                               TDM_LAYER_CAPABILITY_GRAPHIC |
	                               TDM_LAYER_CAPABILITY_SCANOUT |
	                               TDM_LAYER_CAPABILITY_RESEVED_MEMORY |
	                               TDM_LAYER_CAPABILITY_NO_CROP;
	layer_data_osd->zpos = 1;

	layer_data_osd->format_count = sizeof(osd_layer_formats) / sizeof(int);
	layer_data_osd->formats = osd_layer_formats;

	TDM_DBG("layer_data_osd(%p) capabilities(%x)", layer_data_osd,
	        layer_data_osd->capabilities);

	LIST_ADDTAIL(&layer_data_osd->link, &output_data->layer_list);

	/* create IMG layer */
	layer_data_img->sprd_data = sprd_data;
	layer_data_img->output_data = output_data;
	layer_data_img->capabilities = TDM_LAYER_CAPABILITY_OVERLAY |
	                               TDM_LAYER_CAPABILITY_GRAPHIC |
	                               TDM_LAYER_CAPABILITY_SCANOUT |
	                               TDM_LAYER_CAPABILITY_NO_CROP;
	layer_data_img->zpos = 0;

	layer_data_img->format_count = sizeof(img_layer_formats) / sizeof(int);
	layer_data_img->formats = img_layer_formats;

	TDM_DBG("layer_data_img(%p) capabilities(%x)", layer_data_img,
	        layer_data_img->capabilities);

	LIST_ADDTAIL(&layer_data_img->link, &output_data->layer_list);


	return TDM_ERROR_NONE;
}

tdm_error
_tdm_sprd_display_output_update(tdm_sprd_output_data *output)
{
	RETURN_VAL_IF_FAIL(output, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(output->fb_fd_name, TDM_ERROR_INVALID_PARAMETER);

	if (!output->fb_fd)
		output->fb_fd = open (output->fb_fd_name, O_RDWR, 0);

	if (output->fb_fd <= 0)
		goto failed;

	if (ioctl (output->fb_fd, FBIOGET_VSCREENINFO, &output->mi) == -1)
		goto failed;

	output->status = TDM_OUTPUT_CONN_STATUS_CONNECTED;

	if (output->mi.activate & FB_ACTIVATE_VBL) {
		output->dpms_value = TDM_OUTPUT_DPMS_ON;
	} else {
		output->dpms_value = TDM_OUTPUT_DPMS_OFF;
	}


	return TDM_ERROR_NONE;


failed:

	if (output->fb_fd > 0) {
		close(output->fb_fd);
		output->fb_fd = 0;
	}
	output->dpms_value = TDM_OUTPUT_DPMS_OFF;
	output->status = TDM_OUTPUT_CONN_STATUS_DISCONNECTED;
	return TDM_ERROR_OPERATION_FAILED;
}

static void
_tdm_sprd_display_cb_destroy_buffer(tbm_surface_h buffer, void *user_data)
{
	tdm_sprd_data *sprd_data;
	tdm_sprd_display_buffer *display_buffer;
	tdm_sprd_layer_data *layer_data = NULL;
	tdm_sprd_output_data *output_data = NULL;
	if (!user_data) {
		TDM_ERR("no user_data");
		return;
	}
	if (!buffer) {
		TDM_ERR("no buffer");
		return;
	}

	sprd_data = (tdm_sprd_data *)user_data;

	display_buffer = _tdm_sprd_display_find_buffer(sprd_data, buffer);
	if (!display_buffer) {
		TDM_ERR("no display_buffer");
		return;
	}
	TDM_DBG("destroy buffer handle:%d", display_buffer->name[0]);

	LIST_FOR_EACH_ENTRY(output_data, &sprd_data->output_list, link) {
		LIST_FOR_EACH_ENTRY(layer_data, &output_data->layer_list, link) {
			if (display_buffer == layer_data->display_buffer) {
				_tdm_sprd_display_layer_disable (layer_data);
				layer_data->display_buffer = NULL;
			}
		}
	}
	LIST_DEL(&display_buffer->link);

	free(display_buffer);
}

tdm_sprd_output_data *
_tdm_sprd_display_create_output_LCD(tdm_sprd_data *sprd_data)
{
	tdm_sprd_output_data *output_data = NULL;
	tdm_error ret = TDM_ERROR_NONE;

	output_data = calloc (1, sizeof(tdm_sprd_output_data));
	if (!output_data) {
		TDM_ERR("alloc failed");
		return NULL;
	}

	output_data->sprd_data = sprd_data;
	output_data->pipe = 0;
	output_data->connector_type = TDM_OUTPUT_TYPE_LVDS;
	output_data->count_modes = 0;
	output_data->fb_fd_name = FB_DEV_LCD;
	LIST_INITHEAD(&output_data->layer_list);

	ret = _tdm_sprd_display_output_update (output_data);
	if (ret != TDM_ERROR_NONE) {
		TDM_ERR("_tdm_sprd_display_output_update failed (%d)", ret);
		free(output_data);
		return NULL;
	}

	if (output_data->status == TDM_OUTPUT_CONN_STATUS_CONNECTED) {
		//LCD has only one mode
		output_data->count_modes = 1;
		output_data->output_modes = calloc (output_data->count_modes,
		                                    sizeof(tdm_output_mode));
		if (!output_data->output_modes) {
			TDM_ERR("alloc failed");
			if (output_data->fb_fd > 0)
				close(output_data->fb_fd);
			free (output_data);
			return NULL;
		}
		_tdm_sprd_display_to_tdm_mode (&output_data->mi, &output_data->output_modes[0]);
	}

	TDM_DBG("output(%p, status:%d type:%d-%d) pipe(%d) dpms(%d)", output_data,
	        output_data->status,
	        output_data->connector_type, output_data->connector_type_id, output_data->pipe,
	        output_data->dpms_value);

	ret = _tdm_sprd_display_create_layer_list_LCD(output_data);
	if (ret != TDM_ERROR_NONE) {
		TDM_ERR("_tdm_sprd_display_create_layer_list_LCD failed (%d)", ret);
		if (output_data->fb_fd > 0)
			close(output_data->fb_fd);
		free(output_data->output_modes);
		free(output_data);
		return NULL;
	}

	return output_data;
}


void
tdm_sprd_display_destroy_output_list(tdm_sprd_data *sprd_data)
{
	tdm_sprd_output_data *o = NULL, *oo = NULL;

	if (LIST_IS_EMPTY(&sprd_data->output_list))
		return;

	LIST_FOR_EACH_ENTRY_SAFE(o, oo, &sprd_data->output_list, link) {
		LIST_DEL(&o->link);
		if (!LIST_IS_EMPTY(&o->layer_list)) {
			tdm_sprd_layer_data *l = NULL, *ll = NULL;
			LIST_FOR_EACH_ENTRY_SAFE(l, ll, &o->layer_list, link) {
				LIST_DEL(&l->link);
				free(l);
			}
		}
		free(o->output_modes);
		free(o);
	}
}

tdm_error
tdm_sprd_display_create_output_list (tdm_sprd_data *sprd_data)
{
	tdm_sprd_output_data *output_data = NULL;

	RETURN_VAL_IF_FAIL(LIST_IS_EMPTY(&sprd_data->output_list),
	                   TDM_ERROR_OPERATION_FAILED);

	//have only LCD output
	output_data = _tdm_sprd_display_create_output_LCD(sprd_data);
	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_OPERATION_FAILED);
	LIST_ADDTAIL(&output_data->link, &sprd_data->output_list);
	sprd_data->output_count = 1;

	TDM_DBG("output count: %d", sprd_data->output_count);

	return TDM_ERROR_NONE;
}

tdm_error
sprd_display_get_capabilitiy(tdm_backend_data *bdata, tdm_caps_display *caps)
{
	RETURN_VAL_IF_FAIL(caps, TDM_ERROR_INVALID_PARAMETER);

	caps->max_layer_count = -1; /* not defined */

	return TDM_ERROR_NONE;
}

tdm_error
sprd_display_get_pp_capability(tdm_backend_data *bdata, tdm_caps_pp *caps)
{
	return tdm_sprd_pp_get_capability(bdata, caps);
}

tdm_output **
sprd_display_get_outputs(tdm_backend_data *bdata, int *count, tdm_error *error)
{
	tdm_sprd_data *sprd_data = bdata;
	tdm_sprd_output_data *output_data = NULL;
	tdm_output **outputs;
	tdm_error ret;
	int i;

	RETURN_VAL_IF_FAIL(sprd_data, NULL);
	RETURN_VAL_IF_FAIL(count, NULL);

	*count = 0;
	LIST_FOR_EACH_ENTRY(output_data, &sprd_data->output_list, link)
	(*count)++;

	if (*count == 0) {
		ret = TDM_ERROR_NONE;
		goto failed_get;
	}

	/* will be freed in frontend */
	outputs = calloc(*count, sizeof(tdm_sprd_output_data *));
	if (!outputs) {
		TDM_ERR("failed: alloc memory");
		*count = 0;
		ret = TDM_ERROR_OUT_OF_MEMORY;
		goto failed_get;
	}

	i = 0;
	LIST_FOR_EACH_ENTRY(output_data, &sprd_data->output_list, link)
	outputs[i++] = output_data;

	if (error)
		*error = TDM_ERROR_NONE;

	return outputs;
failed_get:
	if (error)
		*error = ret;
	return NULL;
}

tdm_error
sprd_display_get_fd(tdm_backend_data *bdata, int *fd)
{
	tdm_sprd_data *sprd_data = bdata;

	RETURN_VAL_IF_FAIL(sprd_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(fd, TDM_ERROR_INVALID_PARAMETER);

	*fd = sprd_data->drm_fd;

	return TDM_ERROR_NONE;
}

tdm_error
sprd_display_handle_events(tdm_backend_data *bdata)
{
	tdm_sprd_data *sprd_data = bdata;
	RETURN_VAL_IF_FAIL(sprd_data, TDM_ERROR_INVALID_PARAMETER);

	if (drmHandleEvent(sprd_data->drm_fd, &sprd_data->evctx) < 0) {
		return TDM_ERROR_OPERATION_FAILED;
	}
	return TDM_ERROR_NONE;
}

tdm_pp *
sprd_display_create_pp(tdm_backend_data *bdata, tdm_error *error)
{
	tdm_sprd_data *sprd_data = bdata;

	RETURN_VAL_IF_FAIL(sprd_data, NULL);

	return tdm_sprd_pp_create(sprd_data, error);
}

tdm_error
sprd_output_get_capability(tdm_output *output, tdm_caps_output *caps)
{
	tdm_sprd_output_data *output_data = output;
	int i;
	tdm_error ret;

	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(caps, TDM_ERROR_INVALID_PARAMETER);

	memset(caps, 0, sizeof(tdm_caps_output));

	snprintf(caps->maker, TDM_NAME_LEN, "unknown");
	snprintf(caps->model, TDM_NAME_LEN, "unknown");
	snprintf(caps->name, TDM_NAME_LEN, "unknown");

	caps->status = output_data->status;
	caps->type = output_data->connector_type;
	caps->type_id = output_data->connector_type_id;

	caps->mode_count = output_data->count_modes;
	caps->modes = calloc(1, sizeof(tdm_output_mode) * caps->mode_count);
	if (!caps->modes) {
		ret = TDM_ERROR_OUT_OF_MEMORY;
		TDM_ERR("alloc failed\n");
		goto failed_get;
	}
	for (i = 0; i < caps->mode_count; i++)
		caps->modes[i] = output_data->output_modes[i];

	caps->mmWidth = output_data->mi.width;
	caps->mmHeight = output_data->mi.height;
	caps->subpixel = 0;

	caps->min_w = 0;
	caps->min_h = 0;
	caps->max_w = output_data->mi.xres_virtual;
	caps->max_h = output_data->mi.yres_virtual;
	caps->preferred_align = -1;

	caps->prop_count = 0;
	caps->props = NULL;

	return TDM_ERROR_NONE;
failed_get:
	if (caps->modes)
		free(caps->modes);
	if (caps->props)
		free(caps->props);
	memset(caps, 0, sizeof(tdm_caps_output));
	return ret;
}

tdm_layer **
sprd_output_get_layers(tdm_output *output,  int *count, tdm_error *error)
{
	tdm_sprd_output_data *output_data = output;
	tdm_sprd_layer_data *layer_data = NULL;
	tdm_layer **layers;
	tdm_error ret;
	int i;

	RETURN_VAL_IF_FAIL(output_data, NULL);
	RETURN_VAL_IF_FAIL(count, NULL);

	*count = 0;
	LIST_FOR_EACH_ENTRY(layer_data, &output_data->layer_list, link)
	(*count)++;

	if (*count == 0) {
		ret = TDM_ERROR_NONE;
		goto failed_get;
	}

	/* will be freed in frontend */
	layers = calloc(*count, sizeof(tdm_sprd_layer_data *));
	if (!layers) {
		TDM_ERR("failed: alloc memory");
		*count = 0;
		ret = TDM_ERROR_OUT_OF_MEMORY;
		goto failed_get;
	}

	i = 0;
	LIST_FOR_EACH_ENTRY(layer_data, &output_data->layer_list, link)
	layers[i++] = layer_data;

	if (error)
		*error = TDM_ERROR_NONE;

	return layers;
failed_get:
	if (error)
		*error = ret;
	return NULL;
}

tdm_error
sprd_output_set_property(tdm_output *output, unsigned int id, tdm_value value)
{
	return TDM_ERROR_NOT_IMPLEMENTED;
}

tdm_error
sprd_output_get_property(tdm_output *output, unsigned int id, tdm_value *value)
{
	return TDM_ERROR_NOT_IMPLEMENTED;
}

tdm_error
sprd_output_wait_vblank(tdm_output *output, int interval, int sync,
                        void *user_data)
{
	tdm_sprd_output_data *output_data = output;
	tdm_sprd_data *sprd_data;
	tdm_sprd_vblank_data *vblank_data;
	uint target_msc;
	tdm_error ret;

	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);

	vblank_data = calloc(1, sizeof(tdm_sprd_vblank_data));
	if (!vblank_data) {
		TDM_ERR("alloc failed");
		return TDM_ERROR_OUT_OF_MEMORY;
	}

	sprd_data = output_data->sprd_data;

	ret = _tdm_sprd_display_get_cur_msc(sprd_data->drm_fd, output_data->pipe,
	                                    &target_msc);
	if (ret != TDM_ERROR_NONE)
		goto failed_vblank;

	target_msc++;

	vblank_data->type = VBLANK_TYPE_WAIT;
	vblank_data->output_data = output_data;
	vblank_data->user_data = user_data;

	ret = _tdm_sprd_display_wait_vblank(sprd_data->drm_fd, output_data->pipe,
	                                    &target_msc, vblank_data);
	if (ret != TDM_ERROR_NONE)
		goto failed_vblank;

	return TDM_ERROR_NONE;
failed_vblank:
	free(vblank_data);
	return ret;
}

tdm_error
sprd_output_set_vblank_handler(tdm_output *output,
                               tdm_output_vblank_handler func)
{
	tdm_sprd_output_data *output_data = output;
	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(func, TDM_ERROR_INVALID_PARAMETER);
	output_data->vblank_func = func;

	return TDM_ERROR_NONE;
}

tdm_error
sprd_output_commit(tdm_output *output, int sync, void *user_data)
{
	tdm_sprd_output_data *output_data = output;
	tdm_sprd_data *sprd_data;
	tdm_error ret = TDM_ERROR_NONE;

	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);

	sprd_data = output_data->sprd_data;

	_tdm_sprd_display_do_commit(output_data);

	tdm_sprd_vblank_data *vblank_data = calloc (1, sizeof(tdm_sprd_vblank_data));
	uint target_msc;

	if (!vblank_data) {
		TDM_ERR("alloc failed");
		return TDM_ERROR_OUT_OF_MEMORY;
	}

	ret = _tdm_sprd_display_get_cur_msc (sprd_data->drm_fd, output_data->pipe,
	                                     &target_msc);
	if (ret != TDM_ERROR_NONE) {
		free (vblank_data);
		return ret;
	}

	target_msc++;

	vblank_data->type = VBLANK_TYPE_COMMIT;
	vblank_data->output_data = output_data;
	vblank_data->user_data = user_data;

	ret = _tdm_sprd_display_wait_vblank (sprd_data->drm_fd, output_data->pipe,
	                                     &target_msc, vblank_data);
	if (ret != TDM_ERROR_NONE) {
		free (vblank_data);
		return ret;
	}

	return TDM_ERROR_NONE;
}

tdm_error
sprd_output_set_commit_handler(tdm_output *output,
                               tdm_output_commit_handler func)
{
	tdm_sprd_output_data *output_data = output;

	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(func, TDM_ERROR_INVALID_PARAMETER);

	output_data->commit_func = func;

	return TDM_ERROR_NONE;
}

tdm_error
sprd_output_set_dpms(tdm_output *output, tdm_output_dpms dpms_value)
{
	tdm_sprd_output_data *output_data = output;
	tdm_error ret;

	if (output_data->dpms_value == dpms_value) {
		return TDM_ERROR_NONE;
	}

	output_data->dpms_value = dpms_value;

	if (dpms_value != TDM_OUTPUT_DPMS_ON) {
		ret = _tdm_sprd_display_output_disable(output_data);
	} else {
		ret = _tdm_sprd_display_output_enable(output_data);
	}
	return ret;
}

tdm_error
sprd_output_get_dpms(tdm_output *output, tdm_output_dpms *dpms_value)
{
	RETURN_VAL_IF_FAIL(output, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(dpms_value, TDM_ERROR_INVALID_PARAMETER);

	tdm_sprd_output_data *output_data = output;

	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);

	*dpms_value = output_data->dpms_value;

	return TDM_ERROR_NONE;
}

tdm_error
sprd_output_set_mode(tdm_output *output, const tdm_output_mode *mode)
{
	tdm_sprd_output_data *output_data = output;

	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(mode, TDM_ERROR_INVALID_PARAMETER);

	output_data->current_mode = mode;
	output_data->mode_changed = 1;

	return TDM_ERROR_NONE;
}

tdm_error
sprd_output_get_mode(tdm_output *output, const tdm_output_mode **mode)
{
	tdm_sprd_output_data *output_data = output;

	RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(mode, TDM_ERROR_INVALID_PARAMETER);

	*mode = output_data->current_mode;

	return TDM_ERROR_NONE;
}

tdm_error
sprd_layer_get_capability(tdm_layer *layer, tdm_caps_layer *caps)
{
	tdm_sprd_layer_data *layer_data = layer;
	int i;
	tdm_error ret;

	RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(caps, TDM_ERROR_INVALID_PARAMETER);

	memset(caps, 0, sizeof(tdm_caps_layer));

	caps->capabilities = layer_data->capabilities;
	caps->zpos = layer_data->zpos;

	caps->format_count = layer_data->format_count;
	caps->formats = calloc(1, sizeof(tbm_format) * caps->format_count);
	if (!caps->formats) {
		ret = TDM_ERROR_OUT_OF_MEMORY;
		TDM_ERR("alloc failed\n");
		goto failed_get;
	}

	for (i = 0; i < caps->format_count; i++) {
		caps->formats[i] = layer_data->formats[i];
	}

	caps->prop_count = 0;
	caps->props = NULL;

	return TDM_ERROR_NONE;
failed_get:
	free(caps->formats);
	free(caps->props);
	memset(caps, 0, sizeof(tdm_caps_layer));
	return ret;
}

tdm_error
sprd_layer_set_property(tdm_layer *layer, unsigned int id, tdm_value value)
{
	return TDM_ERROR_NOT_IMPLEMENTED;
}

tdm_error
sprd_layer_get_property(tdm_layer *layer, unsigned int id, tdm_value *value)
{
	return TDM_ERROR_NOT_IMPLEMENTED;
}

tdm_error
sprd_layer_set_info(tdm_layer *layer, tdm_info_layer *info)
{
	tdm_sprd_layer_data *layer_data = layer;

	RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(info, TDM_ERROR_INVALID_PARAMETER);

	layer_data->info = *info;
	layer_data->info_changed = 1;

	return TDM_ERROR_NONE;
}

tdm_error
sprd_layer_get_info(tdm_layer *layer, tdm_info_layer *info)
{
	tdm_sprd_layer_data *layer_data = layer;

	RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(info, TDM_ERROR_INVALID_PARAMETER);

	*info = layer_data->info;

	return TDM_ERROR_NONE;
}

tdm_error
sprd_layer_set_buffer(tdm_layer *layer, tbm_surface_h surface)
{
	tdm_sprd_layer_data *layer_data = layer;
	tdm_sprd_data *sprd_data;
	tdm_sprd_display_buffer *display_buffer;
	tdm_error err = TDM_ERROR_NONE;
	int i, count;

	RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);
	RETURN_VAL_IF_FAIL(surface, TDM_ERROR_INVALID_PARAMETER);

	sprd_data = layer_data->sprd_data;
	display_buffer = _tdm_sprd_display_find_buffer(sprd_data, surface);
	if (!display_buffer) {
		display_buffer = calloc(1, sizeof(tdm_sprd_display_buffer));
		if (!display_buffer) {
			TDM_ERR("alloc failed");
			return TDM_ERROR_OUT_OF_MEMORY;
		}
		display_buffer->buffer = surface;

		err = tdm_buffer_add_destroy_handler(surface,
		                                     _tdm_sprd_display_cb_destroy_buffer, sprd_data);
		if (err != TDM_ERROR_NONE) {
			TDM_ERR("add destroy handler fail");
			free(display_buffer);
			return TDM_ERROR_OPERATION_FAILED;
		}
		LIST_ADDTAIL(&display_buffer->link, &sprd_data->buffer_list);

		display_buffer->width = tbm_surface_get_width(surface);
		display_buffer->height = tbm_surface_get_height(surface);
		display_buffer->format = tbm_surface_get_format(surface);
		display_buffer->count = tbm_surface_internal_get_num_bos(surface);
		count = tbm_surface_internal_get_num_planes (display_buffer->format);
		TDM_DBG("set buffer layer(%d): %dx%d %c%c%c%c bo_num:%d plane_num:%d",
		        layer_data->capabilities,
		        display_buffer->width, display_buffer->height,
		        FOURCC_STR(display_buffer->format), display_buffer->count, count);

		for (i = 0; i < display_buffer->count; i++) {
			tbm_bo bo = tbm_surface_internal_get_bo (surface, i);
			display_buffer->handles[i] = tbm_bo_get_handle (bo, TBM_DEVICE_DEFAULT).u32;
			display_buffer->name[i] = tbm_bo_export(bo);
			TDM_DBG("    set buffer layer(%d): bo%d(name:%d handle:%d)",
			        layer_data->capabilities,
			        i, display_buffer->name[i], display_buffer->handles[i]);
		}
		for (i = 0; i < count; i++) {
			tbm_surface_internal_get_plane_data (surface, i, &display_buffer->size,
			                                     &display_buffer->offsets[i],
			                                     &display_buffer->pitches[i]);
			TDM_DBG("    set buffer layer(%d): plane%d(size:%d offset:%d pitch:%d)",
			        layer_data->capabilities,
			        i, display_buffer->size, display_buffer->offsets[i],
			        display_buffer->pitches[i]);

		}

		if (IS_RGB(display_buffer->format))
			display_buffer->width = display_buffer->pitches[0] >> 2;
		else
			display_buffer->width = display_buffer->pitches[0];
	}

	layer_data->display_buffer = display_buffer;
	layer_data->display_buffer_changed = 1;

	return TDM_ERROR_NONE;

}

tdm_error
sprd_layer_unset_buffer(tdm_layer *layer)
{
	tdm_sprd_layer_data *layer_data = layer;
	TDM_DBG("Unset buffer");
	RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);
	_tdm_sprd_display_layer_disable(layer);
	layer_data->display_buffer = NULL;

	return TDM_ERROR_NONE;
}

static int
_sprd_drm_user_handler(struct drm_event *event)
{
	RETURN_VAL_IF_FAIL(event, -1);

	TDM_DBG("got event %d\n", event->type);

	if (event->type != DRM_SPRD_IPP_EVENT)
		return -1;

	tdm_sprd_pp_handler((struct drm_sprd_ipp_event *)event);

	return 0;
}

static void
_sprd_drm_vblank_event (int fd, unsigned int sequence, unsigned int tv_sec,
                        unsigned int tv_usec, void *user_data)
{
	tdm_sprd_vblank_data *vblank_data = (tdm_sprd_vblank_data * )user_data;
	tdm_sprd_output_data *output_data;

	if (!vblank_data) {
		TDM_ERR("no vblank data");
		return;
	}

	output_data = vblank_data->output_data;
	if (!output_data) {
		TDM_ERR("no output data");
		return;
	}
	switch (vblank_data->type) {
	case VBLANK_TYPE_WAIT:
		if (output_data->vblank_func)
			output_data->vblank_func(output_data, sequence,
			                         tv_sec, tv_usec,
			                         vblank_data->user_data);
		break;
	case VBLANK_TYPE_COMMIT:

		if (output_data->commit_func)
			output_data->commit_func(output_data, sequence,
			                         tv_sec, tv_usec,
			                         vblank_data->user_data);

		break;
	default:
		break;
	}
	free(vblank_data);
}

static void
_sprd_drm_flip_complete_event (int fd, unsigned int sequence,
                               unsigned int tv_sec, unsigned int tv_usec, void *user_data)
{
	TDM_DBG("FLIP EVENT");
}

tdm_error
tdm_sprd_display_init_event_handling(tdm_sprd_data *sprd_data)
{
	RETURN_VAL_IF_FAIL(sprd_data, TDM_ERROR_INVALID_PARAMETER);

	sprd_data->evctx.version = 2;
	sprd_data->evctx.page_flip_handler = _sprd_drm_flip_complete_event;
	sprd_data->evctx.vblank_handler = _sprd_drm_vblank_event;

	drmAddUserHandler(sprd_data->drm_fd, _sprd_drm_user_handler);

	return TDM_ERROR_NONE;
}

void
tdm_sprd_display_deinit_event_handling(tdm_sprd_data *sprd_data)
{
	RETURN_VOID_IF_FAIL(sprd_data);

	drmRemoveUserHandler(sprd_data->drm_fd, _sprd_drm_user_handler);
}
