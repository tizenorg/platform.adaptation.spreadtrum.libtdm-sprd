#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <tdm_helper.h>
#include <linux/fb.h>
#include <video/sprdfb.h>
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

typedef enum
{
    VBLANK_TYPE_WAIT,
    VBLANK_TYPE_COMMIT,
} vblank_type_t;

typedef struct
{
    struct list_head link;
    uint32_t hw_event_type;
    void (*hw_event_func)(int fd, tdm_sprd_data *sprd_data_p, void* hw_event_data);
} hw_event_callback_t;

struct _tdm_sprd_vblank_data_s
{
    vblank_type_t type;
    tdm_sprd_output_data *output_data;
    void *user_data;
};

typedef struct _tdm_sprd_display_buffer
{
    struct list_head link;

    unsigned int fb_id;
    tdm_buffer *buffer;
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

struct _tdm_sprd_output_data
{
    struct list_head link;

    /* data which are fixed at initializing */
    tdm_sprd_data *sprd_data;
    uint32_t pipe;
    int count_modes;
    tdm_output_mode *output_modes;
    tdm_output_type connector_type;
    unsigned int connector_type_id;
    struct list_head layer_list;
    tdm_sprd_layer_data *primary_layer;

    tdm_output_vblank_handler vblank_func;
    tdm_output_commit_handler commit_func;
    tdm_output_conn_status status;

    int mode_changed;
    tdm_output_mode *current_mode;

    int waiting_vblank_event;

    char * fb_fd_name;
    int fb_fd;

    struct fb_var_screeninfo mi;

    tdm_output_dpms dpms_value;

};

struct _tdm_sprd_layer_data
{
    struct list_head link;

    /* data which are fixed at initializing */
    tdm_sprd_data *sprd_data;
    tdm_sprd_output_data *output_data;
    tdm_layer_capability capabilities;

    //list of sprd formats
    int format_count;
    int *formats;

    /* not fixed data below */
    tdm_info_layer info;
    int info_changed;

    tdm_sprd_display_buffer *display_buffer;
    int display_buffer_changed;
};

typedef struct _Drm_Event_Context
{
    void (*vblank_handler)(int fd, unsigned int sequence, unsigned int tv_sec,
                           unsigned int tv_usec, void *user_data);
    void (*pp_handler)(int fd, unsigned int  prop_id, unsigned int *buf_idx,
                       unsigned int  tv_sec, unsigned int  tv_usec, void *user_data);
} Drm_Event_Context;

//TODO:: support other formats
int img_layer_formats[] = {
        SPRD_DATA_FORMAT_RGB888
};

//TODO:: support other formats
int osd_layer_formats[] = {
        SPRD_DATA_FORMAT_RGB888
};

#if 0
static tdm_error
check_hw_restriction(unsigned int crtc_w, unsigned int buf_w,
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

    if (buf_w < MIN_WIDTH || buf_w % 2)
    {
        TDM_ERR("buf_w(%d) not 2's multiple or less than %d", buf_w, MIN_WIDTH);
        return TDM_ERROR_BAD_REQUEST;
    }

    if (src_x > dst_x || ((dst_x - src_x) + buf_w) > crtc_w)
        virtual_screen = 1;
    else
        virtual_screen = 0;

    start = (dst_x < 0) ? 0 : dst_x;
    end = ((dst_x + dst_w) > crtc_w) ? crtc_w : (dst_x + dst_w);

    /* check window minimun width */
    if ((end - start) < MIN_WIDTH)
    {
        TDM_ERR("visible_w(%d) less than %d", end-start, MIN_WIDTH);
        return TDM_ERROR_BAD_REQUEST;
    }

    if (!virtual_screen)
    {
        /* Pagewidth of window (= 8 byte align / bytes-per-pixel ) */
        if ((end - start) % 2)
            end--;
    }
    else
    {
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

    if (src_x != *new_src_x || src_w != *new_src_w || dst_x != *new_dst_x || dst_w != *new_dst_w)
        TDM_DBG("=> buf_w(%d) src(%d,%d) dst(%d,%d), virt(%d) start(%d) end(%d)",
                buf_w, *new_src_x, *new_src_w, *new_dst_x, *new_dst_w, virtual_screen, start, end);

    return TDM_ERROR_NONE;
}
#endif


static tdm_sprd_display_buffer*
_tdm_sprd_display_find_buffer(tdm_sprd_data *sprd_data, tdm_buffer *buffer)
{
    tdm_sprd_display_buffer *display_buffer;

    LIST_FOR_EACH_ENTRY(display_buffer, &sprd_data->buffer_list, link)
    {
        if (display_buffer->buffer == buffer)
            return display_buffer;
    }

    return NULL;
}


static inline uint32_t _get_refresh(struct fb_var_screeninfo * timing)
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

    hfreq = pixclock/htotal;
    return hfreq/vtotal;
}

/*
 * Convert fb_var_screeninfo to tdm_output_mode
 */
static inline void _tdm_sprd_display_to_tdm_mode(struct fb_var_screeninfo * timing, tdm_output_mode  *mode)
{

    if (!timing->pixclock)
        return;

    mode->refresh = _get_refresh(timing);
    mode->width = timing->xres;
    mode->height = timing->yres;
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

    if (timing->vmode & FB_VMODE_INTERLACED)
        mode->flags |= DRM_MODE_FLAG_INTERLACE;

    if (timing->vmode & FB_VMODE_DOUBLE)
        mode->flags |= DRM_MODE_FLAG_DBLSCAN;

    int interlaced = !!(mode->flags & DRM_MODE_FLAG_INTERLACE);
    snprintf(mode->name, DRM_DISPLAY_MODE_LEN, "%dx%d%s", mode->width, mode->height,
             interlaced ? "i" : "");
}

static int
_localdrmWaitVBlank(int fd, drmVBlank *vbl)
{
    struct timespec timeout, cur;
    int ret;

    ret = clock_gettime(CLOCK_MONOTONIC, &timeout);
    if (ret < 0)
    {
    	TDM_ERR("clock_gettime failed: %s", strerror(errno));
	goto out;
    }
    timeout.tv_sec++;

    do
    {
       ret = ioctl(fd, DRM_IOCTL_WAIT_VBLANK, vbl);
       vbl->request.type &= ~DRM_VBLANK_RELATIVE;
       if (ret && errno == EINTR)
       {
	       clock_gettime(CLOCK_MONOTONIC, &cur);
	       /* Timeout after 1s */
	       if (cur.tv_sec > timeout.tv_sec + 1 ||
		   (cur.tv_sec == timeout.tv_sec && cur.tv_nsec >=
		    timeout.tv_nsec))
           {
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
    if (_localdrmWaitVBlank(fd, &vbl))
    {
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

    vbl.request.type =  DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
    if (pipe > 0)
        vbl.request.type |= DRM_VBLANK_SECONDARY;

    vbl.request.sequence = 1;
    vbl.request.signal = (unsigned long)(uintptr_t)data;

    if (_localdrmWaitVBlank(fd, &vbl))
    {
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
    TDM_ERR ("FB_BLANK_POWERDOWN\n");
    if (ioctl (output_data->fb_fd, FBIOBLANK, FB_BLANK_POWERDOWN) < 0)
    {
        TDM_ERR ("FB_BLANK_POWERDOWN is failed: %s\n", strerror (errno));
        return TDM_ERROR_OPERATION_FAILED;
    }
    return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_output_enable(tdm_sprd_output_data *output_data)
{
    TDM_ERR ("FB_BLANK_UNBLANK\n");
    if (ioctl (output_data->fb_fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0)
    {
        TDM_ERR ("FB_BLANK_UNBLANK is failed: %s\n", strerror (errno));
        return TDM_ERROR_OPERATION_FAILED;
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
    int layer_index;

    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_OPERATION_FAILED);

    if (output_data->dpms_value == TDM_OUTPUT_DPMS_OFF)
    {
        output_data->dpms_value = TDM_OUTPUT_DPMS_ON;
        _tdm_sprd_display_output_enable(output_data);
    }

    LIST_FOR_EACH_ENTRY(layer_data, &output_data->layer_list, link)
    {
        if(!layer_data->display_buffer_changed && !layer_data->info_changed)
            continue;

        if (layer_data->capabilities & TDM_LAYER_CAPABILITY_PRIMARY)
        {
            layer_index = SPRD_LAYER_OSD;
        }
        else if (layer_data->capabilities & TDM_LAYER_CAPABILITY_OVERLAY)
        {
            layer_index = SPRD_LAYER_IMG;
        }
        else
        {
            TDM_ERR ("layer capability (0x%x) not supported\n", layer_data->capabilities);
            continue;
        }

        if(layer_data->display_buffer)
        {
            ovi.layer_index = layer_index;
            ov_disp.layer_index |= layer_index;

            //TODO:: support different formats
            ovi.data_type = SPRD_DATA_FORMAT_RGB888;
            ovi.endian.y = SPRD_DATA_ENDIAN_B0B1B2B3;
            ovi.endian.u = SPRD_DATA_ENDIAN_B0B1B2B3;
            ovi.endian.v = SPRD_DATA_ENDIAN_B0B1B2B3;
            ovi.rb_switch = 0;

            ovi.size.hsize = layer_data->display_buffer->pitches[0] / 4;
            ovi.size.vsize = layer_data->display_buffer->height;

            ovi.rect.x = layer_data->info.dst_pos.x;
            ovi.rect.y = layer_data->info.dst_pos.y;
            ovi.rect.w = layer_data->info.src_config.pos.w;
            ovi.rect.h = layer_data->info.src_config.pos.h;

            if (ovi.layer_index == SPRD_LAYER_OSD)
            {
                ov_disp.osd_handle = layer_data->display_buffer->name[0];
            }
            else if (ovi.layer_index == SPRD_LAYER_IMG)
            {
                ov_disp.img_handle = layer_data->display_buffer->name[0];
            }

            TDM_ERR("SPRD_FB_SET_OVERLAY(%d) rect:%dx%d+%d+%d size:%dx%d\n", ovi.layer_index,
                    ovi.rect.w, ovi.rect.h, ovi.rect.x, ovi.rect.y, ovi.size.hsize, ovi.size.vsize);
            if (ioctl (layer_data->output_data->fb_fd, SPRD_FB_SET_OVERLAY, &ovi) == -1)
            {
                TDM_ERR ("SPRD_FB_SET_OVERLAY(%d) error:%s\n", layer_index, strerror (errno));
            }
        }
        else
        {
            if (ioctl(layer_data->output_data->fb_fd, SPRD_FB_UNSET_OVERLAY, &layer_index) == -1)
            {
                TDM_ERR ("SPRD_FB_UNSET_OVERLAY(%d) error:%s\n", layer_index, strerror (errno));
            }
        }
    }


    if (ov_disp.layer_index)
    {
        ov_disp.display_mode = SPRD_DISPLAY_OVERLAY_ASYNC;

        TDM_ERR("SPRD_FB_DISPLAY_OVERLAY(%d) osd_handle:%d img_handle:%d\n", ov_disp.layer_index, ov_disp.osd_handle, ov_disp.img_handle);
        if (ioctl(output_data->fb_fd, SPRD_FB_DISPLAY_OVERLAY, &ov_disp) == -1) {
            TDM_ERR( "SPRD_FB_DISPLAY_OVERLAY(%d) error: %s \n", strerror (errno), ov_disp.layer_index);
            res = TDM_ERROR_OPERATION_FAILED;
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
    if (len == 0)
    {
        TDM_WRN("warning: the size of the drm_event is 0.");
        return 0;
    }
    if (len < sizeof *e)
    {
        TDM_WRN("warning: the size of the drm_event is less than drm_event structure.");
        return -1;
    }
#if 0
    if (len > MAX_BUF_SIZE - sizeof(struct drm_sprd_ipp_event))
    {
        TDM_WRN("warning: the size of the drm_event can be over the maximum size.");
        return -1;
    }
#endif
    i = 0;
    while (i < len)
    {
        e = (struct drm_event *) &buffer[i];
        switch (e->type)
        {
            case DRM_EVENT_VBLANK:
                {
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
            case DRM_EXYNOS_IPP_EVENT:
                {
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
    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_OPERATION_FAILED);

    tdm_sprd_data *sprd_data = output_data->sprd_data;
    tdm_sprd_layer_data *layer_data;

    //create OSD layer
    layer_data = calloc (1, sizeof(tdm_sprd_layer_data));
    if (!layer_data)
    {
        TDM_ERR("alloc failed");
    }

    layer_data->sprd_data = sprd_data;
    layer_data->output_data = output_data;

    layer_data->capabilities = TDM_LAYER_CAPABILITY_PRIMARY | TDM_LAYER_CAPABILITY_GRAPHIC;
    output_data->primary_layer = layer_data;

    layer_data->format_count = sizeof(osd_layer_formats)/sizeof(int);
    layer_data->formats = osd_layer_formats;

    TDM_DBG("layer_data(%p) capabilities(%x)", layer_data, layer_data->capabilities);

    LIST_ADDTAIL(&layer_data->link, &output_data->layer_list);


    //create IMG layer
    layer_data = calloc (1, sizeof(tdm_sprd_layer_data));
    if (!layer_data)
    {
        TDM_ERR("alloc failed");
    }
    layer_data->sprd_data = sprd_data;
    layer_data->output_data = output_data;
    layer_data->capabilities = TDM_LAYER_CAPABILITY_OVERLAY | TDM_LAYER_CAPABILITY_GRAPHIC;
    output_data->primary_layer = layer_data;

    layer_data->format_count = sizeof(img_layer_formats)/sizeof(int);
    layer_data->formats = img_layer_formats;

    TDM_DBG("layer_data(%p) capabilities(%x)", layer_data, layer_data->capabilities);

    LIST_ADDTAIL(&layer_data->link, &output_data->layer_list);


    return TDM_ERROR_NONE;
}

tdm_error
_tdm_sprd_display_output_update(tdm_sprd_output_data * output)
{
    RETURN_VAL_IF_FAIL(output, TDM_ERROR_OPERATION_FAILED);
    RETURN_VAL_IF_FAIL(output->fb_fd_name, TDM_ERROR_OPERATION_FAILED);

    if (!output->fb_fd)
        output->fb_fd = open (output->fb_fd_name, O_RDWR, 0);

    if (output->fb_fd <= 0)
        goto failed;

   if (ioctl (output->fb_fd, FBIOGET_VSCREENINFO, &output->mi) == -1)
        goto failed;

    output->status = TDM_OUTPUT_CONN_STATUS_CONNECTED;

    if (output->mi.activate & FB_ACTIVATE_VBL)
    {
        output->dpms_value = TDM_OUTPUT_DPMS_ON;
    }
    else
    {
        output->dpms_value = TDM_OUTPUT_DPMS_OFF;
    }


    return TDM_ERROR_NONE;


failed:

    if (output->fb_fd > 0)
    {
        output->fb_fd = 0;
        close(output->fb_fd);
    }
    output->dpms_value = TDM_OUTPUT_DPMS_OFF;
    output->status = TDM_OUTPUT_CONN_STATUS_DISCONNECTED;
    return TDM_ERROR_OPERATION_FAILED;
}

static void
_tdm_sprd_display_cb_destroy_buffer(tdm_buffer *buffer, void *user_data)
{
    tdm_sprd_data *sprd_data;
    tdm_sprd_display_buffer *display_buffer;

    if (!user_data)
    {
        TDM_ERR("no user_data");
        return;
    }
    if (!buffer)
    {
        TDM_ERR("no buffer");
        return;
    }

    sprd_data = (tdm_sprd_data *)user_data;

    display_buffer = _tdm_sprd_display_find_buffer(sprd_data, buffer);
    if (!display_buffer)
    {
        TDM_ERR("no display_buffer");
        return;
    }
    LIST_DEL(&display_buffer->link);

    free(display_buffer);
}

tdm_sprd_output_data *
_tdm_sprd_display_create_output_LCD(tdm_sprd_data *sprd_data)
{
    tdm_sprd_output_data *output_data;

    output_data = calloc (1, sizeof(tdm_sprd_output_data));
    if (!output_data)
    {
        TDM_ERR("alloc failed");
        return NULL;
    }

    output_data->sprd_data = sprd_data;
    output_data->pipe = 1;
    output_data->connector_type = TDM_OUTPUT_TYPE_LVDS;
    output_data->count_modes = 0;
    output_data->fb_fd_name = FB_DEV_LCD;
    LIST_INITHEAD(&output_data->layer_list);

    _tdm_sprd_display_output_update (output_data);

    if (output_data->status == TDM_OUTPUT_CONN_STATUS_CONNECTED)
    {
        //LCD has only one mode
        output_data->count_modes = 1;
        output_data->output_modes = calloc (output_data->count_modes, sizeof(tdm_output_mode));
        if (!output_data->output_modes)
        {
            TDM_ERR("alloc failed");
            free (output_data);
            goto failed_create;
        }
        _tdm_sprd_display_to_tdm_mode (&output_data->mi, &output_data->output_modes[0]);
    }

    TDM_DBG("output(%p, status:%d type:%d-%d) pipe(%d) dpms(%d)", output_data, output_data->status,
            output_data->connector_type, output_data->connector_type_id, output_data->pipe, output_data->dpms_value);

    _tdm_sprd_display_create_layer_list_LCD(output_data);


    return output_data;
failed_create:

    if (output_data->output_modes)
        free(output_data->output_modes);

    return NULL;
}


void
tdm_sprd_display_destroy_output_list(tdm_sprd_data *sprd_data)
{
    tdm_sprd_output_data *o = NULL, *oo = NULL;

    if (LIST_IS_EMPTY(&sprd_data->output_list))
        return;

    LIST_FOR_EACH_ENTRY_SAFE(o, oo, &sprd_data->output_list, link)
    {
        LIST_DEL(&o->link);
        if (!LIST_IS_EMPTY(&o->layer_list))
        {
            tdm_sprd_layer_data *l = NULL, *ll = NULL;
            LIST_FOR_EACH_ENTRY_SAFE(l, ll, &o->layer_list, link)
            {
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

    RETURN_VAL_IF_FAIL(LIST_IS_EMPTY(&sprd_data->output_list), TDM_ERROR_OPERATION_FAILED);

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

tdm_output**
sprd_display_get_outputs(tdm_backend_data *bdata, int *count, tdm_error *error)
{
    tdm_sprd_data *sprd_data = bdata;
    tdm_sprd_output_data *output_data;
    tdm_output **outputs;
    tdm_error ret;
    int i;

    RETURN_VAL_IF_FAIL(sprd_data, NULL);
    RETURN_VAL_IF_FAIL(count, NULL);

    *count = 0;
    LIST_FOR_EACH_ENTRY(output_data, &sprd_data->output_list, link)
        (*count)++;

    if (*count == 0)
    {
        ret = TDM_ERROR_NONE;
        goto failed_get;
    }

    /* will be freed in frontend */
    outputs = calloc(*count, sizeof(tdm_sprd_output_data*));
    if (!outputs)
    {
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
    #define MAX_BUF_SIZE    1024

    char buffer[MAX_BUF_SIZE];
    unsigned int len, i;
    hw_event_t *kernel_event_p;
    hw_event_callback_t *event_list_cur = NULL, *event_list_next = NULL;
    /* The DRM read semantics guarantees that we always get only
     * complete events. */
    len = read(sprd_data->drm_fd, buffer, sizeof buffer);
    if (len == 0)
    {
        TDM_WRN("warning: the size of the drm_event is 0.");
        return TDM_ERROR_NONE;
    }
    if (len < sizeof *kernel_event_p)
    {
        TDM_WRN("warning: the size of the drm_event is less than drm_event structure.");
        return TDM_ERROR_OPERATION_FAILED;
    }
    if (len > MAX_BUF_SIZE)
    {
        TDM_WRN("warning: the size of the drm_event can be over the maximum size.");
        return TDM_ERROR_OPERATION_FAILED;
    }
    i = 0;
    while (i < len)
    {
        kernel_event_p = (hw_event_t *) &buffer[i];
        LIST_FOR_EACH_ENTRY_SAFE(event_list_cur, event_list_next, &sprd_data->events_list, link)
        {
            if (event_list_cur->hw_event_type == kernel_event_p->type)
            {
                if (event_list_cur->hw_event_func)
                    event_list_cur->hw_event_func(sprd_data->drm_fd, sprd_data, &buffer[i]);
                break;
            }
        }
        i += kernel_event_p->length;
    }

    return TDM_ERROR_NONE;
}

tdm_pp*
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

    caps->status = output_data->status;
    caps->type = output_data->connector_type;
    caps->type_id = output_data->connector_type_id;

    caps->mode_count = output_data->count_modes;
    caps->modes = calloc(1, sizeof(tdm_output_mode) * caps->mode_count);
    if (!caps->modes)
    {
        ret = TDM_ERROR_OUT_OF_MEMORY;
        TDM_ERR("alloc failed\n");
        goto failed_get;
    }
    for (i = 0; i < caps->mode_count; i++)
        caps->modes[i] = output_data->output_modes[i];

    caps->mmWidth = output_data->mi.width;
    caps->mmHeight = output_data->mi.height;
    caps->subpixel = 0;

    caps->min_w = -1;
    caps->min_h = -1;
    caps->max_w = -1;
    caps->max_h = -1;
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

tdm_layer**
sprd_output_get_layers(tdm_output *output,  int *count, tdm_error *error)
{
    tdm_sprd_output_data *output_data = output;
    tdm_sprd_layer_data *layer_data;
    tdm_layer **layers;
    tdm_error ret;
    int i;

    RETURN_VAL_IF_FAIL(output_data, NULL);
    RETURN_VAL_IF_FAIL(count, NULL);

    *count = 0;
    LIST_FOR_EACH_ENTRY(layer_data, &output_data->layer_list, link)
        (*count)++;

    if (*count == 0)
    {
        ret = TDM_ERROR_NONE;
        goto failed_get;
    }

    /* will be freed in frontend */
    layers = calloc(*count, sizeof(tdm_sprd_layer_data*));
    if (!layers)
    {
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
sprd_output_wait_vblank(tdm_output *output, int interval, int sync, void *user_data)
{
    tdm_sprd_output_data *output_data = output;
    tdm_sprd_data *sprd_data;
    tdm_sprd_vblank_data *vblank_data;
    uint target_msc;
    tdm_error ret;

    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);

    vblank_data = calloc(1, sizeof(tdm_sprd_vblank_data));
    if (!vblank_data)
    {
        TDM_ERR("alloc failed");
        return TDM_ERROR_OUT_OF_MEMORY;
    }

    sprd_data = output_data->sprd_data;

    ret = _tdm_sprd_display_get_cur_msc(sprd_data->drm_fd, output_data->pipe, &target_msc);
    if (ret != TDM_ERROR_NONE)
        goto failed_vblank;

    target_msc++;

    vblank_data->type = VBLANK_TYPE_WAIT;
    vblank_data->output_data = output_data;
    vblank_data->user_data = user_data;

    ret = _tdm_sprd_display_wait_vblank(sprd_data->drm_fd, output_data->pipe, &target_msc, vblank_data);
    if (ret != TDM_ERROR_NONE)
        goto failed_vblank;

    return TDM_ERROR_NONE;
failed_vblank:
    free(vblank_data);
    return ret;
}

tdm_error
sprd_output_set_vblank_handler(tdm_output *output, tdm_output_vblank_handler func)
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
    tdm_error ret;
    uint target_msc;

    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);

    //TODO: check flag sync

    sprd_data = output_data->sprd_data;

    tdm_sprd_vblank_data *vblank_data = calloc (1, sizeof(tdm_sprd_vblank_data));

    if (!vblank_data)
    {
        TDM_ERR("alloc failed");
        return TDM_ERROR_OUT_OF_MEMORY;
    }

    vblank_data->type = VBLANK_TYPE_COMMIT;
    vblank_data->output_data = output_data;
    vblank_data->user_data = user_data;

    target_msc++;
    ret = _tdm_sprd_display_wait_vblank (sprd_data->drm_fd, 0, &target_msc, vblank_data);
    if (ret != TDM_ERROR_NONE)
    {
        free (vblank_data);
        return ret;
    }
    
    return ret;
}

tdm_error
sprd_output_set_commit_handler(tdm_output *output, tdm_output_commit_handler func)
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

    if (output_data->dpms_value == dpms_value)
    {
        return TDM_ERROR_NONE;
    }

    output_data->dpms_value = dpms_value;

    if (dpms_value == TDM_OUTPUT_DPMS_OFF)
    {
        ret = _tdm_sprd_display_output_disable(output_data);
    }
    else
    {
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
sprd_output_set_mode(tdm_output *output, tdm_output_mode *mode)
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
    caps->zpos = -1;  /* if VIDEO layer, zpos is -1 */

    caps->format_count = layer_data->format_count;
    caps->formats = calloc(1, sizeof(tbm_format) * caps->format_count);
    if (!caps->formats)
    {
        ret = TDM_ERROR_OUT_OF_MEMORY;
        TDM_ERR("alloc failed\n");
        goto failed_get;
    }

    for (i = 0; i < caps->format_count; i++)
    {
        caps->formats[i] = tdm_sprd_fb_format_to_tbm_format(layer_data->formats[i]);
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
sprd_layer_set_buffer(tdm_layer *layer, tdm_buffer *buffer)
{
    tdm_sprd_layer_data *layer_data = layer;
    tdm_sprd_data *sprd_data;
    tdm_sprd_display_buffer *display_buffer;
    tdm_error err = TDM_ERROR_NONE;
    tbm_surface_h surface;
    int i, count;

    RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(buffer, TDM_ERROR_INVALID_PARAMETER);

    sprd_data = layer_data->sprd_data;
    surface = tdm_buffer_get_surface(buffer);
    display_buffer = _tdm_sprd_display_find_buffer(sprd_data, buffer);
    if (!display_buffer)
    {
        display_buffer = calloc(1, sizeof(tdm_sprd_display_buffer));
        if (!display_buffer)
        {
            TDM_ERR("alloc failed");
            return TDM_ERROR_OUT_OF_MEMORY;
        }
        display_buffer->buffer = buffer;

        err = tdm_buffer_add_destroy_handler(buffer, _tdm_sprd_display_cb_destroy_buffer, sprd_data);
        if (err != TDM_ERROR_NONE)
        {
            TDM_ERR("add destroy handler fail");
            return TDM_ERROR_OPERATION_FAILED;
        }
        LIST_ADDTAIL(&display_buffer->link, &sprd_data->buffer_list);

        display_buffer->width = tbm_surface_get_width(surface);
        display_buffer->height = tbm_surface_get_height(surface);
        display_buffer->format = tbm_surface_get_format(surface);
        display_buffer->count = tbm_surface_internal_get_num_bos(surface);
        for (i = 0; i < display_buffer->count; i++)
        {
            tbm_bo bo = tbm_surface_internal_get_bo (surface, i);
            display_buffer->handles[i] = tbm_bo_get_handle (bo, TBM_DEVICE_DEFAULT).u32;
            display_buffer->name[i] = tbm_bo_export(bo);
        }
        count = tbm_surface_internal_get_num_planes (display_buffer->format);
        for (i = 0; i < count; i++)
            tbm_surface_internal_get_plane_data (surface, i, &display_buffer->size, &display_buffer->offsets[i],
                                                 &display_buffer->pitches[i]);

        if (IS_RGB(display_buffer->format))
            display_buffer->width = display_buffer->pitches[0] >> 2;
        else
            display_buffer->width = display_buffer->pitches[0];
    }

    TDM_DBG("sprd_data->drm_fd : %d, display_buffer->fb_id:%u", sprd_data->drm_fd, display_buffer->fb_id);
    layer_data->display_buffer = display_buffer;
    layer_data->display_buffer_changed = 1;

    return TDM_ERROR_NONE;

}

tdm_error
sprd_layer_unset_buffer(tdm_layer *layer)
{
    tdm_sprd_layer_data *layer_data = layer;

    RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);

    layer_data->display_buffer = NULL;
    layer_data->display_buffer_changed = 1;

    return TDM_ERROR_NONE;
}

static void
_sprd_drm_vblank_event (int fd, tdm_sprd_data *sprd_data_p, void* hw_event_data)
{
    RETURN_VAL_IF_FAIL(hw_event_data,);
    RETURN_VAL_IF_FAIL(sprd_data_p,);
    struct drm_event_vblank *hw_vblank = (struct drm_event_vblank *) hw_event_data;
    tdm_sprd_vblank_data *vblank_data = (tdm_sprd_vblank_data* )((unsigned long) hw_vblank->user_data);
    tdm_sprd_output_data *output_data;

    if (!vblank_data)
    {
        TDM_ERR("no vblank data");
        return;
    }

    output_data = vblank_data->output_data;
    if (!output_data)
    {
        TDM_ERR("no output data");
        return;
    }
    switch(vblank_data->type)
    {
    case VBLANK_TYPE_WAIT:
        if (output_data->vblank_func)
            output_data->vblank_func(output_data, hw_vblank->sequence,
                                     hw_vblank->tv_sec, hw_vblank->tv_usec,
                                     vblank_data->user_data);
        break;
    case VBLANK_TYPE_COMMIT:

        _tdm_sprd_display_do_commit(output_data);

        if (output_data->commit_func)
            output_data->commit_func(output_data, hw_vblank->sequence,
                                     hw_vblank->tv_sec, hw_vblank->tv_usec,
                                     vblank_data->user_data);
        break;
    default:
        return;
    }
}
static void
_sprd_drm_flip_complete_event (int fd, tdm_sprd_data *sprd_data_p, void* hw_event_data)
{
    TDM_DBG("FLIP EVENT");
}

tdm_error
tdm_sprd_display_create_event_list(tdm_sprd_data *sprd_data)
{
    RETURN_VAL_IF_FAIL(sprd_data, TDM_ERROR_INVALID_PARAMETER);
    tdm_error ret_err = TDM_ERROR_NONE;
    hw_event_callback_t *vblank_event_p = NULL;
    hw_event_callback_t *ipp_event_p = NULL;
    hw_event_callback_t *flip_complete_event_p = NULL;
#ifdef HAVE_FB_VBLANK
/** @TODO FB vblank */
#else
    if ((vblank_event_p = malloc(sizeof(hw_event_callback_t))) == NULL)
    {
        TDM_ERR("alloc fail");
        ret_err = TDM_ERROR_OUT_OF_MEMORY;
        goto bad_l;
    }
    vblank_event_p->hw_event_type = DRM_EVENT_VBLANK;
    vblank_event_p->hw_event_func = _sprd_drm_vblank_event;
    LIST_ADD(&vblank_event_p->link, &sprd_data->events_list);
    if ((flip_complete_event_p = malloc(sizeof(hw_event_callback_t))) == NULL)
    {
        TDM_ERR("alloc fail");
        ret_err = TDM_ERROR_OUT_OF_MEMORY;
        goto bad_l;
    }
    flip_complete_event_p->hw_event_type = DRM_EVENT_FLIP_COMPLETE;
    flip_complete_event_p->hw_event_func = _sprd_drm_flip_complete_event;
    LIST_ADD(&flip_complete_event_p->link, &sprd_data->events_list);
#endif
    if ((ipp_event_p = malloc(sizeof(hw_event_callback_t))) == NULL)
    {
        TDM_ERR("alloc_fail");
        ret_err = TDM_ERROR_OUT_OF_MEMORY;
        goto bad_l;
    }
    ipp_event_p->hw_event_type = DRM_SPRD_IPP_EVENT;
    ipp_event_p->hw_event_func = tdm_sprd_pp_handler;
    LIST_ADD(&ipp_event_p->link, &sprd_data->events_list);
    if (ret_err != TDM_ERROR_NONE)
        goto bad_l;
    return TDM_ERROR_NONE;
bad_l:
    tdm_sprd_display_destroy_event_list(sprd_data);
    return ret_err;
}

void
tdm_sprd_display_destroy_event_list(tdm_sprd_data *sprd_data)
{
    RETURN_VAL_IF_FAIL(sprd_data,);
}
