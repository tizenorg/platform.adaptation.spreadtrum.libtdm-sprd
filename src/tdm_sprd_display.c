#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <drm_fourcc.h>
#include <tdm_helper.h>
#include "tdm_sprd.h"

#define MIN_WIDTH   32
#define LAYER_COUNT_PER_OUTPUT   2


typedef struct _tdm_sprd_output_data tdm_sprd_output_data;
typedef struct _tdm_sprd_layer_data tdm_sprd_layer_data;
typedef struct _tdm_sprd_vblank_data tdm_sprd_vblank_data;

typedef enum
{
    VBLANK_TYPE_WAIT,
    VBLANK_TYPE_COMMIT,
} vblank_type;

typedef struct _tdm_sprd_display_buffer
{
    struct list_head link;

    unsigned int fb_id;
    tdm_buffer *buffer;
    int width;
} tdm_sprd_display_buffer;

struct _tdm_sprd_vblank_data
{
    vblank_type type;
    tdm_sprd_output_data *output_data;
    void *user_data;
};

struct _tdm_sprd_output_data
{
    struct list_head link;

    /* data which are fixed at initializing */
    tdm_sprd_data *sprd_data;
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t crtc_id;
    uint32_t pipe;
    uint32_t dpms_prop_id;
    int count_modes;
    drmModeModeInfoPtr drm_modes;
    tdm_output_mode *output_modes;
    tdm_output_type connector_type;
    unsigned int connector_type_id;
    struct list_head layer_list;
    tdm_sprd_layer_data *primary_layer;

    /* not fixed data below */
    tdm_output_vblank_handler vblank_func;
    tdm_output_commit_handler commit_func;

    tdm_output_conn_status status;

    int mode_changed;
    const tdm_output_mode *current_mode;

    int waiting_vblank_event;
};

struct _tdm_sprd_layer_data
{
    struct list_head link;

    /* data which are fixed at initializing */
    tdm_sprd_data *sprd_data;
    tdm_sprd_output_data *output_data;
    uint32_t plane_id;
    tdm_layer_capability capabilities;
    int zpos;

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

static drmModeModeInfoPtr
_tdm_sprd_display_get_mode(tdm_sprd_output_data *output_data)
{
    int i;

    if (!output_data->current_mode)
    {
        TDM_ERR("no output_data->current_mode");
        return NULL;
    }

    for (i = 0; i < output_data->count_modes; i++)
    {
        drmModeModeInfoPtr drm_mode = &output_data->drm_modes[i];
        if ((drm_mode->hdisplay == output_data->current_mode->width) &&
            (drm_mode->vdisplay == output_data->current_mode->height) &&
            (drm_mode->vrefresh == output_data->current_mode->refresh) &&
            (drm_mode->flags == output_data->current_mode->flags) &&
            (drm_mode->type == output_data->current_mode->type) &&
            !(strncmp(drm_mode->name, output_data->current_mode->name, TDM_NAME_LEN)))
            return drm_mode;
    }

    return NULL;
}

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

static void
_tdm_sprd_display_to_tdm_mode(drmModeModeInfoPtr drm_mode, tdm_output_mode *tdm_mode)
{
    tdm_mode->width = drm_mode->hdisplay;
    tdm_mode->height = drm_mode->vdisplay;
    tdm_mode->refresh = drm_mode->vrefresh;
    tdm_mode->flags = drm_mode->flags;
    tdm_mode->type = drm_mode->type;
    snprintf(tdm_mode->name, TDM_NAME_LEN, "%s", drm_mode->name);
}

static tdm_error
_tdm_sprd_display_get_cur_msc (int fd, int pipe, uint *msc)
{
    drmVBlank vbl;

    vbl.request.type = DRM_VBLANK_RELATIVE;
    if (pipe > 0)
        vbl.request.type |= DRM_VBLANK_SECONDARY;

    vbl.request.sequence = 0;
    if (drmWaitVBlank(fd, &vbl))
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

    vbl.request.type =  DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT;
    if (pipe > 0)
        vbl.request.type |= DRM_VBLANK_SECONDARY;

    vbl.request.sequence = *target_msc;
    vbl.request.signal = (unsigned long)(uintptr_t)data;

    if (drmWaitVBlank(fd, &vbl))
    {
        *target_msc = 0;
        TDM_ERR("wait vblank failed: %m");
        return TDM_ERROR_OPERATION_FAILED;
    }

    *target_msc = vbl.reply.sequence;

    return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_commit_primary_layer(tdm_sprd_layer_data *layer_data)
{
    tdm_sprd_data *sprd_data = layer_data->sprd_data;
    tdm_sprd_output_data *output_data = layer_data->output_data;

    if (output_data->mode_changed && layer_data->display_buffer_changed)
    {
        drmModeModeInfoPtr mode;

        if (!layer_data->display_buffer)
        {
            TDM_ERR("primary layer should have a buffer for modestting");
            return TDM_ERROR_BAD_REQUEST;
        }

        output_data->mode_changed = 0;
        layer_data->display_buffer_changed = 0;
        layer_data->info_changed = 0;

        mode = _tdm_sprd_display_get_mode(output_data);
        if (!mode)
        {
            TDM_ERR("couldn't find proper mode");
            return TDM_ERROR_BAD_REQUEST;
        }

        if (drmModeSetCrtc(sprd_data->drm_fd, output_data->crtc_id,
                           layer_data->display_buffer->fb_id, 0, 0,
                           &output_data->connector_id, 1, mode))
        {
            TDM_ERR("set crtc failed: %m");
            return TDM_ERROR_OPERATION_FAILED;
        }

        return TDM_ERROR_NONE;
    }
    else if (layer_data->display_buffer_changed)
    {
        layer_data->display_buffer_changed = 0;

        if (!layer_data->display_buffer)
        {
            if (drmModeSetCrtc(sprd_data->drm_fd, output_data->crtc_id,
                               0, 0, 0, NULL, 0, NULL))
            {
                TDM_ERR("unset crtc failed: %m");
                return TDM_ERROR_OPERATION_FAILED;
            }
        }
        else
        {
            if (drmModePageFlip(sprd_data->drm_fd, output_data->crtc_id,
                                layer_data->display_buffer->fb_id, DRM_MODE_PAGE_FLIP_EVENT, layer_data->display_buffer))
            {
                TDM_ERR("pageflip failed: %m");
                return TDM_ERROR_OPERATION_FAILED;
            }
        }
    }

    return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_commit_layer(tdm_sprd_layer_data *layer_data)
{
    tdm_sprd_data *sprd_data = layer_data->sprd_data;
    tdm_sprd_output_data *output_data = layer_data->output_data;
    unsigned int new_src_x, new_src_w;
    unsigned int new_dst_x, new_dst_w;
    uint32_t fx, fy, fw, fh;
    int crtc_w;

    if (!layer_data->display_buffer_changed && !layer_data->info_changed)
        return TDM_ERROR_NONE;

    if (output_data->current_mode)
        crtc_w = output_data->current_mode->width;
    else
    {
        drmModeCrtcPtr crtc = drmModeGetCrtc(sprd_data->drm_fd, output_data->crtc_id);
        if (!crtc)
        {
            TDM_ERR("getting crtc failed");
            return TDM_ERROR_OPERATION_FAILED;
        }
        crtc_w = crtc->width;
        if (crtc_w == 0)
        {
            TDM_ERR("getting crtc width failed");
            return TDM_ERROR_OPERATION_FAILED;
        }
    }

    layer_data->display_buffer_changed = 0;
    layer_data->info_changed = 0;

    if (!layer_data->display_buffer)
    {
        if (drmModeSetPlane(sprd_data->drm_fd, layer_data->plane_id,
                            output_data->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
            TDM_ERR("unset plane(%d) filed: %m", layer_data->plane_id);

        return TDM_ERROR_NONE;
    }

    /* check hw restriction*/
    if (check_hw_restriction(crtc_w, layer_data->display_buffer->width,
                             layer_data->info.src_config.pos.x,
                             layer_data->info.src_config.pos.w,
                             layer_data->info.dst_pos.x,
                             layer_data->info.dst_pos.w,
                             &new_src_x, &new_src_w, &new_dst_x, &new_dst_w) != TDM_ERROR_NONE)
    {
        TDM_WRN("not going to set plane(%d)", layer_data->plane_id);
        return TDM_ERROR_NONE;
    }

    if (layer_data->info.src_config.pos.x != new_src_x)
        TDM_DBG("src_x changed: %d => %d", layer_data->info.src_config.pos.x, new_src_x);
    if (layer_data->info.src_config.pos.w != new_src_w)
        TDM_DBG("src_w changed: %d => %d", layer_data->info.src_config.pos.w, new_src_w);
    if (layer_data->info.dst_pos.x != new_dst_x)
        TDM_DBG("dst_x changed: %d => %d", layer_data->info.dst_pos.x, new_dst_x);
    if (layer_data->info.dst_pos.w != new_dst_w)
        TDM_DBG("dst_w changed: %d => %d", layer_data->info.dst_pos.w, new_dst_w);

    /* Source values are 16.16 fixed point */
    fx = ((unsigned int)new_src_x) << 16;
    fy = ((unsigned int)layer_data->info.src_config.pos.y) << 16;
    fw = ((unsigned int)new_src_w) << 16;
    fh = ((unsigned int)layer_data->info.src_config.pos.h) << 16;

    if (drmModeSetPlane(sprd_data->drm_fd, layer_data->plane_id,
                        output_data->crtc_id, layer_data->display_buffer->fb_id, 0,
                        new_dst_x, layer_data->info.dst_pos.y,
                        new_dst_w, layer_data->info.dst_pos.h,
                        fx, fy, fw, fh) < 0)
    {
        TDM_ERR("set plane(%d) failed: %m", layer_data->plane_id);
        return TDM_ERROR_OPERATION_FAILED;
    }

    return TDM_ERROR_NONE;
}

static void
_tdm_sprd_display_cb_vblank(int fd, unsigned int sequence,
                              unsigned int tv_sec, unsigned int tv_usec,
                              void *user_data)
{
    tdm_sprd_vblank_data *vblank_data = user_data;
    tdm_sprd_output_data *output_data;

    if (!vblank_data)
    {
        TDM_ERR("no vblank data");
        return;
    }

    output_data = vblank_data->output_data;

    switch(vblank_data->type)
    {
    case VBLANK_TYPE_WAIT:
        if (output_data->vblank_func)
            output_data->vblank_func(output_data, sequence, tv_sec, tv_usec, vblank_data->user_data);
        break;
    case VBLANK_TYPE_COMMIT:
        if (output_data->commit_func)
            output_data->commit_func(output_data, sequence, tv_sec, tv_usec, vblank_data->user_data);
        break;
    default:
        return;
    }
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
_tdm_sprd_display_create_layer_list_type(tdm_sprd_data *sprd_data)
{
    tdm_error ret;
    int i;

    for (i = 0; i < sprd_data->plane_res->count_planes; i++)
    {
        tdm_sprd_output_data *output_data;
        tdm_sprd_layer_data *layer_data;
        drmModePlanePtr plane;
        unsigned int type = 0;

        plane = drmModeGetPlane(sprd_data->drm_fd, sprd_data->plane_res->planes[i]);
        if (!plane)
        {
            TDM_ERR("no plane");
            continue;
        }

        ret = tdm_sprd_display_get_property(sprd_data, sprd_data->plane_res->planes[i],
                                              DRM_MODE_OBJECT_PLANE, "type", &type, NULL);
        if (ret != TDM_ERROR_NONE)
        {
            TDM_ERR("plane(%d) doesn't have 'type' info", sprd_data->plane_res->planes[i]);
            drmModeFreePlane(plane);
            continue;
        }

        layer_data = calloc(1, sizeof(tdm_sprd_layer_data));
        if (!layer_data)
        {
            TDM_ERR("alloc failed");
            drmModeFreePlane(plane);
            continue;
        }

        LIST_FOR_EACH_ENTRY(output_data, &sprd_data->output_list, link)
        {
            if (plane->possible_crtcs & (1 << output_data->pipe))
                break;
        }

        if (!output_data)
        {
            TDM_ERR("plane(%d) couldn't found proper output", plane->plane_id);
            drmModeFreePlane(plane);
            free(layer_data);
            continue;
        }

        layer_data->sprd_data = sprd_data;
        layer_data->output_data = output_data;
        layer_data->plane_id = sprd_data->plane_res->planes[i];

        if (type == DRM_PLANE_TYPE_CURSOR)
        {
            layer_data->capabilities = TDM_LAYER_CAPABILITY_CURSOR | TDM_LAYER_CAPABILITY_GRAPHIC;
            layer_data->zpos = 2;
        }
        else if (type == DRM_PLANE_TYPE_OVERLAY)
        {
            layer_data->capabilities = TDM_LAYER_CAPABILITY_OVERLAY | TDM_LAYER_CAPABILITY_GRAPHIC;
            layer_data->zpos = 1;
        }
        else if (type == DRM_PLANE_TYPE_PRIMARY)
        {
            layer_data->capabilities = TDM_LAYER_CAPABILITY_PRIMARY | TDM_LAYER_CAPABILITY_GRAPHIC;
            layer_data->zpos = 0;
            output_data->primary_layer = layer_data;
        }
        else
        {
            drmModeFreePlane(plane);
            free(layer_data);
            continue;
        }

        TDM_DBG("layer_data(%p) plane_id(%d) crtc_id(%d) zpos(%d) capabilities(%x)",
                layer_data, layer_data->plane_id, layer_data->output_data->crtc_id,
                layer_data->zpos, layer_data->capabilities);

        LIST_ADDTAIL(&layer_data->link, &output_data->layer_list);

        drmModeFreePlane(plane);
    }

    return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_create_layer_list_immutable_zpos(tdm_sprd_data *sprd_data)
{
    tdm_error ret;
    int i;

    for (i = 0; i < sprd_data->plane_res->count_planes; i++)
    {
        tdm_sprd_output_data *output_data;
        tdm_sprd_layer_data *layer_data;
        drmModePlanePtr plane;
        unsigned int type = 0, zpos = 0;

        plane = drmModeGetPlane(sprd_data->drm_fd, sprd_data->plane_res->planes[i]);
        if (!plane)
        {
            TDM_ERR("no plane");
            continue;
        }

        ret = tdm_sprd_display_get_property(sprd_data, sprd_data->plane_res->planes[i],
                                              DRM_MODE_OBJECT_PLANE, "type", &type, NULL);
        if (ret != TDM_ERROR_NONE)
        {
            TDM_ERR("plane(%d) doesn't have 'type' info", sprd_data->plane_res->planes[i]);
            drmModeFreePlane(plane);
            continue;
        }

        ret = tdm_sprd_display_get_property(sprd_data, sprd_data->plane_res->planes[i],
                                              DRM_MODE_OBJECT_PLANE, "zpos", &zpos, NULL);
        if (ret != TDM_ERROR_NONE)
        {
            TDM_ERR("plane(%d) doesn't have 'zpos' info", sprd_data->plane_res->planes[i]);
            drmModeFreePlane(plane);
            continue;
        }

        layer_data = calloc(1, sizeof(tdm_sprd_layer_data));
        if (!layer_data)
        {
            TDM_ERR("alloc failed");
            drmModeFreePlane(plane);
            continue;
        }

        LIST_FOR_EACH_ENTRY(output_data, &sprd_data->output_list, link)
        {
            if (plane->possible_crtcs & (1 << output_data->pipe))
                break;
        }

        if (!output_data)
        {
            TDM_ERR("plane(%d) couldn't found proper output", plane->plane_id);
            drmModeFreePlane(plane);
            free(layer_data);
            continue;
        }

        layer_data->sprd_data = sprd_data;
        layer_data->output_data = output_data;
        layer_data->plane_id = sprd_data->plane_res->planes[i];
        layer_data->zpos = zpos;

        if (type == DRM_PLANE_TYPE_CURSOR)
            layer_data->capabilities = TDM_LAYER_CAPABILITY_CURSOR | TDM_LAYER_CAPABILITY_GRAPHIC;
        else if (type == DRM_PLANE_TYPE_OVERLAY)
            layer_data->capabilities = TDM_LAYER_CAPABILITY_OVERLAY | TDM_LAYER_CAPABILITY_GRAPHIC;
        else if (type == DRM_PLANE_TYPE_PRIMARY)
        {
            layer_data->capabilities = TDM_LAYER_CAPABILITY_PRIMARY | TDM_LAYER_CAPABILITY_GRAPHIC;
            output_data->primary_layer = layer_data;
        }
        else
        {
            drmModeFreePlane(plane);
            free(layer_data);
            continue;
        }

        TDM_DBG("layer_data(%p) plane_id(%d) crtc_id(%d) zpos(%d) capabilities(%x)",
                layer_data, layer_data->plane_id, layer_data->output_data->crtc_id,
                layer_data->zpos, layer_data->capabilities);

        LIST_ADDTAIL(&layer_data->link, &output_data->layer_list);

        drmModeFreePlane(plane);
    }

    return TDM_ERROR_NONE;
}

static tdm_error
_tdm_sprd_display_create_layer_list_not_fixed(tdm_sprd_data *sprd_data)
{
    int i, find_pipe = -1;

    if (sprd_data->mode_res->count_connectors * LAYER_COUNT_PER_OUTPUT > sprd_data->plane_res->count_planes)
    {
        TDM_ERR("not enough layers");
        return TDM_ERROR_OPERATION_FAILED;
    }

    for (i = 0; i < sprd_data->plane_res->count_planes; i++)
    {
        tdm_sprd_output_data *output_data;
        tdm_sprd_layer_data *layer_data;
        drmModePlanePtr plane;

        plane = drmModeGetPlane(sprd_data->drm_fd, sprd_data->plane_res->planes[i]);
        if (!plane)
        {
            TDM_ERR("no plane");
            continue;
        }

        layer_data = calloc(1, sizeof(tdm_sprd_layer_data));
        if (!layer_data)
        {
            TDM_ERR("alloc failed");
            drmModeFreePlane(plane);
            continue;
        }

        /* TODO
         * Currently, kernel doesn give us the correct device infomation.
         * Primary connector type is invalid. plane's count is not correct.
         * So we need to fix all of them with kernel.
         * Temporarily we dedicate only 2 plane to each output.
         * First plane is primary layer. Second plane's zpos is 1.
         */
        if (i % LAYER_COUNT_PER_OUTPUT == 0)
            find_pipe++;

        LIST_FOR_EACH_ENTRY(output_data, &sprd_data->output_list, link)
        {
            if (output_data->pipe == find_pipe)
                break;
        }

        if (i == sprd_data->mode_res->count_connectors * LAYER_COUNT_PER_OUTPUT)
        {
            TDM_DBG("no more plane(%d) need for outputs", plane->plane_id);
            drmModeFreePlane(plane);
            free(layer_data);
            return TDM_ERROR_NONE;
        }

        layer_data->sprd_data = sprd_data;
        layer_data->output_data = output_data;
        layer_data->plane_id = sprd_data->plane_res->planes[i];

        layer_data->zpos = i % 2;
        if (layer_data->zpos == 0)
        {
            layer_data->capabilities = TDM_LAYER_CAPABILITY_PRIMARY | TDM_LAYER_CAPABILITY_GRAPHIC;
            output_data->primary_layer = layer_data;
        }
        else
        {
            tdm_error ret;

            layer_data->capabilities = TDM_LAYER_CAPABILITY_OVERLAY | TDM_LAYER_CAPABILITY_GRAPHIC;

            ret = tdm_sprd_display_set_property(sprd_data, layer_data->plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "zpos", layer_data->zpos);
            if (ret != TDM_ERROR_NONE)
            {
                drmModeFreePlane(plane);
                free(layer_data);
                return TDM_ERROR_OPERATION_FAILED;
            }
        }

        TDM_DBG("layer_data(%p) plane_id(%d) crtc_id(%d) zpos(%d) capabilities(%x)",
                layer_data, layer_data->plane_id, layer_data->output_data->crtc_id,
                layer_data->zpos, layer_data->capabilities);

        LIST_ADDTAIL(&layer_data->link, &output_data->layer_list);

        drmModeFreePlane(plane);
    }

    return TDM_ERROR_NONE;
}

static void
_tdm_sprd_display_cb_destroy_buffer(tdm_buffer *buffer, void *user_data)
{
    tdm_sprd_data *sprd_data;
    tdm_sprd_display_buffer *display_buffer;
    int ret;

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

    if (display_buffer->fb_id > 0)
    {
        ret = drmModeRmFB(sprd_data->drm_fd, display_buffer->fb_id);
        if (ret < 0)
        {
            TDM_ERR("rm fb failed");
            return;
        }
        TDM_DBG("drmModeRmFB success!!! fb_id:%d", display_buffer->fb_id);
    }
    else
        TDM_DBG("drmModeRmFB not called fb_id:%d", display_buffer->fb_id);

    free(display_buffer);
}

tdm_error
tdm_sprd_display_create_layer_list(tdm_sprd_data *sprd_data)
{
    tdm_sprd_output_data *output_data;
    tdm_error ret;

    if (!sprd_data->has_zpos_info)
        ret = _tdm_sprd_display_create_layer_list_type(sprd_data);
    else if (sprd_data->is_immutable_zpos)
        ret = _tdm_sprd_display_create_layer_list_immutable_zpos(sprd_data);
    else
        ret = _tdm_sprd_display_create_layer_list_not_fixed(sprd_data);

    if (ret != TDM_ERROR_NONE)
        return ret;

    LIST_FOR_EACH_ENTRY(output_data, &sprd_data->output_list, link)
    {
        if (!output_data->primary_layer)
        {
            TDM_ERR("output(%d) no primary layer", output_data->pipe);
            return TDM_ERROR_OPERATION_FAILED;
        }
    }

    return TDM_ERROR_NONE;
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
        free(o->drm_modes);
        free(o->output_modes);
        free(o);
    }
}

tdm_error
tdm_sprd_display_create_output_list(tdm_sprd_data *sprd_data)
{
    tdm_sprd_output_data *output_data;
    int i;
    tdm_error ret;
    int allocated = 0;

    RETURN_VAL_IF_FAIL(LIST_IS_EMPTY(&sprd_data->output_list), TDM_ERROR_OPERATION_FAILED);

    for (i = 0; i < sprd_data->mode_res->count_connectors; i++)
    {
        drmModeConnectorPtr connector;
        drmModeEncoderPtr encoder;
        int crtc_id = 0, c, j;

        connector = drmModeGetConnector(sprd_data->drm_fd, sprd_data->mode_res->connectors[i]);
        if (!connector)
        {
            TDM_ERR("no connector");
            ret = TDM_ERROR_OPERATION_FAILED;
            goto failed_create;
        }

        if (connector->count_encoders != 1)
        {
            TDM_ERR("too many encoders: %d", connector->count_encoders);
            drmModeFreeConnector(connector);
            ret = TDM_ERROR_OPERATION_FAILED;
            goto failed_create;
        }

        encoder = drmModeGetEncoder(sprd_data->drm_fd, connector->encoders[0]);
        if (!encoder)
        {
            TDM_ERR("no encoder");
            drmModeFreeConnector(connector);
            ret = TDM_ERROR_OPERATION_FAILED;
            goto failed_create;
        }

        for (c = 0; c < sprd_data->mode_res->count_crtcs; c++)
        {
            if (allocated & (1 << c))
                continue;

            if ((encoder->possible_crtcs & (1 << c)) == 0)
                continue;

            crtc_id = sprd_data->mode_res->crtcs[c];
            allocated |= (1 << c);
            break;
        }

        if (crtc_id == 0)
        {
            TDM_ERR("no possible crtc");
            drmModeFreeConnector(connector);
            ret = TDM_ERROR_OPERATION_FAILED;
            goto failed_create;
        }

        output_data = calloc(1, sizeof(tdm_sprd_output_data));
        if (!output_data)
        {
            TDM_ERR("alloc failed");
            drmModeFreeConnector(connector);
            drmModeFreeEncoder(encoder);
            ret = TDM_ERROR_OUT_OF_MEMORY;
            goto failed_create;
        }

        LIST_INITHEAD(&output_data->layer_list);

        output_data->sprd_data = sprd_data;
        output_data->connector_id = sprd_data->mode_res->connectors[i];
        output_data->encoder_id = encoder->encoder_id;
        output_data->crtc_id = crtc_id;
        output_data->pipe = c;
        output_data->connector_type = connector->connector_type;
        output_data->connector_type_id = connector->connector_type_id;

        if (connector->connection == DRM_MODE_CONNECTED)
            output_data->status = TDM_OUTPUT_CONN_STATUS_CONNECTED;
        else
            output_data->status = TDM_OUTPUT_CONN_STATUS_DISCONNECTED;

        for (j = 0; j < connector->count_props; j++)
        {
            drmModePropertyPtr prop = drmModeGetProperty(sprd_data->drm_fd, connector->props[i]);
            if (!prop)
                continue;
            if (!strcmp(prop->name, "DPMS"))
            {
                output_data->dpms_prop_id = connector->props[i];
                drmModeFreeProperty(prop);
                break;
            }
            drmModeFreeProperty(prop);
        }

        output_data->count_modes = connector->count_modes;
        output_data->drm_modes = calloc(connector->count_modes, sizeof(drmModeModeInfo));
        if (!output_data->drm_modes)
        {
            TDM_ERR("alloc failed");
            free(output_data);
            drmModeFreeConnector(connector);
            drmModeFreeEncoder(encoder);
            ret = TDM_ERROR_OUT_OF_MEMORY;
            goto failed_create;
        }
        output_data->output_modes = calloc(connector->count_modes, sizeof(tdm_output_mode));
        if (!output_data->output_modes)
        {
            TDM_ERR("alloc failed");
            free(output_data);
            free(output_data->drm_modes);
            drmModeFreeConnector(connector);
            drmModeFreeEncoder(encoder);
            ret = TDM_ERROR_OUT_OF_MEMORY;
            goto failed_create;
        }
        for (j = 0; j < connector->count_modes; j++)
        {
            output_data->drm_modes[j] = connector->modes[j];
            _tdm_sprd_display_to_tdm_mode(&output_data->drm_modes[j], &output_data->output_modes[j]);
        }

        LIST_ADDTAIL(&output_data->link, &sprd_data->output_list);

        TDM_DBG("output_data(%p) connector_id(%d:%d:%d-%d) encoder_id(%d) crtc_id(%d) pipe(%d) dpms_id(%d)",
                output_data, output_data->connector_id, output_data->status, output_data->connector_type,
                output_data->connector_type_id, output_data->encoder_id, output_data->crtc_id,
                output_data->pipe, output_data->dpms_prop_id);

        drmModeFreeEncoder(encoder);
        drmModeFreeConnector(connector);
    }

    TDM_DBG("output count: %d", sprd_data->mode_res->count_connectors);

    return TDM_ERROR_NONE;
failed_create:
    tdm_sprd_display_destroy_output_list(sprd_data);
    return ret;
}

tdm_error
tdm_sprd_display_set_property(tdm_sprd_data *sprd_data,
                                unsigned int obj_id, unsigned int obj_type,
                                const char *name, unsigned int value)
{
    drmModeObjectPropertiesPtr props = NULL;
    unsigned int i;

    props = drmModeObjectGetProperties(sprd_data->drm_fd, obj_id, obj_type);
    if (!props)
    {
        TDM_ERR("drmModeObjectGetProperties failed: %m");
        return TDM_ERROR_OPERATION_FAILED;
    }
    for (i = 0; i < props->count_props; i++)
    {
        drmModePropertyPtr prop = drmModeGetProperty(sprd_data->drm_fd, props->props[i]);
        int ret;
        if (!prop)
        {
            TDM_ERR("drmModeGetProperty failed: %m");
            drmModeFreeObjectProperties(props);
            return TDM_ERROR_OPERATION_FAILED;
        }
        if (!strcmp(prop->name, name))
        {
            ret = drmModeObjectSetProperty(sprd_data->drm_fd, obj_id, obj_type, prop->prop_id, value);
            if (ret < 0)
            {
                TDM_ERR("drmModeObjectSetProperty failed: %m");
                drmModeFreeProperty(prop);
                drmModeFreeObjectProperties(props);
                return TDM_ERROR_OPERATION_FAILED;
            }
            drmModeFreeProperty(prop);
            drmModeFreeObjectProperties(props);
            return TDM_ERROR_NONE;
        }
        drmModeFreeProperty(prop);
    }

    TDM_ERR("not found '%s' property", name);

    drmModeFreeObjectProperties(props);
    /* TODO
    * kernel info error
    * it must be changed to 'return TDM_ERROR_OPERATION_FAILED' after kernel fix.
    */
    return TDM_ERROR_NONE;
}

tdm_error
tdm_sprd_display_get_property(tdm_sprd_data *sprd_data,
                                unsigned int obj_id, unsigned int obj_type,
                                const char *name, unsigned int *value, int *is_immutable)
{
    drmModeObjectPropertiesPtr props = NULL;
    int i;

    props = drmModeObjectGetProperties(sprd_data->drm_fd, obj_id, obj_type);
    if (!props)
        return TDM_ERROR_OPERATION_FAILED;

    for (i = 0; i < props->count_props; i++)
    {
        drmModePropertyPtr prop = drmModeGetProperty(sprd_data->drm_fd, props->props[i]);

        if (!prop)
            continue;

        if (!strcmp(prop->name, name))
        {
            if (is_immutable)
                *is_immutable = prop->flags & DRM_MODE_PROP_IMMUTABLE;
            if (value)
                *value = (unsigned int)props->prop_values[i];
            drmModeFreeProperty(prop);
            drmModeFreeObjectProperties(props);
            return TDM_ERROR_NONE;
        }

        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    TDM_DBG("coundn't find '%s' property", name);
    return TDM_ERROR_OPERATION_FAILED;
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
//    Drm_Event_Context ctx;
    drmEventContext evctx;
    RETURN_VAL_IF_FAIL(sprd_data, TDM_ERROR_INVALID_PARAMETER);
    memset(&evctx, 0, sizeof(drmEventContext));

    evctx.vblank_handler = _tdm_sprd_display_cb_vblank;
  //  ctx.pp_handler = _tdm_sprd_display_cb_pp;
    drmHandleEvent(sprd_data->drm_fd, &evctx);
    //_tdm_sprd_display_events_handle(sprd_data->drm_fd, &ctx);

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
    tdm_sprd_data *sprd_data;
    drmModeConnectorPtr connector = NULL;
    drmModeCrtcPtr crtc = NULL;
    drmModeObjectPropertiesPtr props = NULL;
    int i;
    tdm_error ret;

    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(caps, TDM_ERROR_INVALID_PARAMETER);

    memset(caps, 0, sizeof(tdm_caps_output));

    sprd_data = output_data->sprd_data;

    caps->status = output_data->status;
    caps->type = output_data->connector_type;
    caps->type_id = output_data->connector_type_id;

    connector = drmModeGetConnector(sprd_data->drm_fd, output_data->connector_id);
    RETURN_VAL_IF_FAIL(connector, TDM_ERROR_OPERATION_FAILED);

    caps->mode_count = connector->count_modes;
    caps->modes = calloc(1, sizeof(tdm_output_mode) * caps->mode_count);
    if (!caps->modes)
    {
        ret = TDM_ERROR_OUT_OF_MEMORY;
        TDM_ERR("alloc failed\n");
        goto failed_get;
    }
    for (i = 0; i < caps->mode_count; i++)
        caps->modes[i] = output_data->output_modes[i];

    caps->mmWidth = connector->mmWidth;
    caps->mmHeight = connector->mmHeight;
    caps->subpixel = connector->subpixel;

    caps->min_w = sprd_data->mode_res->min_width;
    caps->min_h = sprd_data->mode_res->min_height;
    caps->max_w = sprd_data->mode_res->max_width;
    caps->max_h = sprd_data->mode_res->max_height;
    caps->preferred_align = -1;

    crtc = drmModeGetCrtc(sprd_data->drm_fd, output_data->crtc_id);
    if (!crtc)
    {
        ret = TDM_ERROR_OPERATION_FAILED;
        TDM_ERR("get crtc failed: %m\n");
        goto failed_get;
    }

    props = drmModeObjectGetProperties(sprd_data->drm_fd, output_data->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!props)
    {
        ret = TDM_ERROR_OPERATION_FAILED;
        TDM_ERR("get crtc properties failed: %m\n");
        goto failed_get;
    }

    caps->prop_count = props->count_props;
    caps->props = calloc(1, sizeof(tdm_prop) * caps->prop_count);
    if (!caps->props)
    {
        ret = TDM_ERROR_OUT_OF_MEMORY;
        TDM_ERR("alloc failed\n");
        goto failed_get;
    }

    for (i = 0; i < caps->prop_count; i++)
    {
        drmModePropertyPtr prop = drmModeGetProperty(sprd_data->drm_fd, props->props[i]);
        if (!prop)
            continue;
        snprintf(caps->props[i].name, TDM_NAME_LEN, "%s", prop->name);
        caps->props[i].id = props->props[i];
        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    drmModeFreeCrtc(crtc);
    drmModeFreeConnector(connector);

    return TDM_ERROR_NONE;
failed_get:
    drmModeFreeCrtc(crtc);
    drmModeFreeObjectProperties(props);
    drmModeFreeConnector(connector);
    free(caps->modes);
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
    tdm_sprd_output_data *output_data = output;
    tdm_sprd_data *sprd_data;
    int ret;

    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(output_data->crtc_id > 0, TDM_ERROR_INVALID_PARAMETER);

    sprd_data = output_data->sprd_data;
    ret = drmModeObjectSetProperty(sprd_data->drm_fd,
                                   output_data->crtc_id, DRM_MODE_OBJECT_CRTC,
                                   id, value.u32);
    if (ret < 0)
    {
        TDM_ERR("set property failed: %m");
        return TDM_ERROR_OPERATION_FAILED;
    }

    return TDM_ERROR_NONE;
}

tdm_error
sprd_output_get_property(tdm_output *output, unsigned int id, tdm_value *value)
{
    tdm_sprd_output_data *output_data = output;
    tdm_sprd_data *sprd_data;
    drmModeObjectPropertiesPtr props;
    int i;

    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(output_data->crtc_id > 0, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(value, TDM_ERROR_INVALID_PARAMETER);

    sprd_data = output_data->sprd_data;
    props = drmModeObjectGetProperties(sprd_data->drm_fd, output_data->crtc_id, DRM_MODE_OBJECT_CRTC);
    if (props == NULL)
    {
        TDM_ERR("get property failed: %m");
        return TDM_ERROR_OPERATION_FAILED;
    }

    for (i = 0; i < props->count_props; i++)
        if (props->props[i] == id)
        {
            (*value).u32 = (uint)props->prop_values[i];
            break;
        }

    drmModeFreeObjectProperties(props);

    return TDM_ERROR_NONE;
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
    tdm_sprd_layer_data *layer_data;
    tdm_error ret;

    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);

    sprd_data = output_data->sprd_data;

    LIST_FOR_EACH_ENTRY(layer_data, &output_data->layer_list, link)
    {
        if (layer_data == output_data->primary_layer)
        {
            ret = _tdm_sprd_display_commit_primary_layer(layer_data);
            if (ret != TDM_ERROR_NONE)
                return ret;
        }
        else
        {
            ret = _tdm_sprd_display_commit_layer(layer_data);
            if (ret != TDM_ERROR_NONE)
                return ret;
        }
    }

    if (tdm_helper_drm_fd == -1)
    {
        tdm_sprd_vblank_data *vblank_data = calloc(1, sizeof(tdm_sprd_vblank_data));
        uint target_msc;

        if (!vblank_data)
        {
            TDM_ERR("alloc failed");
            return TDM_ERROR_OUT_OF_MEMORY;
        }

        ret = _tdm_sprd_display_get_cur_msc(sprd_data->drm_fd, output_data->pipe, &target_msc);
        if (ret != TDM_ERROR_NONE)
        {
            free(vblank_data);
            return ret;
        }

        target_msc++;

        vblank_data->type = VBLANK_TYPE_COMMIT;
        vblank_data->output_data = output_data;
        vblank_data->user_data = user_data;

        ret = _tdm_sprd_display_wait_vblank(sprd_data->drm_fd, output_data->pipe, &target_msc, vblank_data);
        if (ret != TDM_ERROR_NONE)
        {
            free(vblank_data);
            return ret;
        }
    }

    return TDM_ERROR_NONE;
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
    tdm_sprd_data *sprd_data;
    int ret;

    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);

    sprd_data = output_data->sprd_data;
    ret = drmModeObjectSetProperty(sprd_data->drm_fd,
                                   output_data->connector_id, DRM_MODE_OBJECT_CONNECTOR,
                                   output_data->dpms_prop_id, dpms_value);
    if (ret < 0)
    {
        TDM_ERR("set dpms failed: %m");
        return TDM_ERROR_OPERATION_FAILED;
    }

    return TDM_ERROR_NONE;
}

tdm_error
sprd_output_get_dpms(tdm_output *output, tdm_output_dpms *dpms_value)
{
    tdm_sprd_output_data *output_data = output;
    tdm_sprd_data *sprd_data;
    drmModeObjectPropertiesPtr props;
    int i;

    RETURN_VAL_IF_FAIL(output_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(dpms_value, TDM_ERROR_INVALID_PARAMETER);

    sprd_data = output_data->sprd_data;
    props = drmModeObjectGetProperties(sprd_data->drm_fd, output_data->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (props == NULL)
    {
        TDM_ERR("get property failed: %m");
        return TDM_ERROR_OPERATION_FAILED;
    }

    for (i = 0; i < props->count_props; i++)
        if (props->props[i] == output_data->dpms_prop_id)
        {
            *dpms_value = (uint)props->prop_values[i];
            break;
        }

    drmModeFreeObjectProperties(props);

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
    tdm_sprd_data *sprd_data;
    drmModePlanePtr plane = NULL;
    drmModeObjectPropertiesPtr props = NULL;
    int i;
    tdm_error ret;

    RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(caps, TDM_ERROR_INVALID_PARAMETER);

    memset(caps, 0, sizeof(tdm_caps_layer));

    sprd_data = layer_data->sprd_data;
    plane = drmModeGetPlane(sprd_data->drm_fd, layer_data->plane_id);
    if (!plane)
    {
        TDM_ERR("get plane failed: %m");
        ret = TDM_ERROR_OPERATION_FAILED;
        goto failed_get;
    }

    caps->capabilities = layer_data->capabilities;
    caps->zpos = layer_data->zpos;  /* if VIDEO layer, zpos is -1 */

    caps->format_count = plane->count_formats;
    caps->formats = calloc(1, sizeof(tbm_format) * caps->format_count);
    if (!caps->formats)
    {
        ret = TDM_ERROR_OUT_OF_MEMORY;
        TDM_ERR("alloc failed\n");
        goto failed_get;
    }

    for (i = 0; i < caps->format_count; i++)
    {
        /* TODO: kernel reports wrong formats */
        if (plane->formats[i] != DRM_FORMAT_XRGB8888 && plane->formats[i] != DRM_FORMAT_ARGB8888)
           continue;
        caps->formats[i] = tdm_sprd_format_to_tbm_format(plane->formats[i]);
    }

    props = drmModeObjectGetProperties(sprd_data->drm_fd, layer_data->plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props)
    {
        ret = TDM_ERROR_OPERATION_FAILED;
        TDM_ERR("get plane properties failed: %m\n");
        goto failed_get;
    }

    caps->props = calloc(1, sizeof(tdm_prop) * props->count_props);
    if (!caps->props)
    {
        ret = TDM_ERROR_OUT_OF_MEMORY;
        TDM_ERR("alloc failed\n");
        goto failed_get;
    }

    caps->prop_count = 0;
    for (i = 0; i < props->count_props; i++)
    {
        drmModePropertyPtr prop = drmModeGetProperty(sprd_data->drm_fd, props->props[i]);
        if (!prop)
            continue;
        if (!strncmp(prop->name, "type", TDM_NAME_LEN))
            continue;
        if (!strncmp(prop->name, "zpos", TDM_NAME_LEN))
            continue;
        snprintf(caps->props[i].name, TDM_NAME_LEN, "%s", prop->name);
        caps->props[i].id = props->props[i];
        caps->prop_count++;
        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    drmModeFreePlane(plane);

    return TDM_ERROR_NONE;
failed_get:
    drmModeFreeObjectProperties(props);
    drmModeFreePlane(plane);
    free(caps->formats);
    free(caps->props);
    memset(caps, 0, sizeof(tdm_caps_layer));
    return ret;
}

tdm_error
sprd_layer_set_property(tdm_layer *layer, unsigned int id, tdm_value value)
{
    tdm_sprd_layer_data *layer_data = layer;
    tdm_sprd_data *sprd_data;
    int ret;

    RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(layer_data->plane_id > 0, TDM_ERROR_INVALID_PARAMETER);

    sprd_data = layer_data->sprd_data;
    ret = drmModeObjectSetProperty(sprd_data->drm_fd,
                                   layer_data->plane_id, DRM_MODE_OBJECT_PLANE,
                                   id, value.u32);
    if (ret < 0)
    {
        TDM_ERR("set property failed: %m");
        return TDM_ERROR_OPERATION_FAILED;
    }

    return TDM_ERROR_NONE;
}

tdm_error
sprd_layer_get_property(tdm_layer *layer, unsigned int id, tdm_value *value)
{
    tdm_sprd_layer_data *layer_data = layer;
    tdm_sprd_data *sprd_data;
    drmModeObjectPropertiesPtr props;
    int i;

    RETURN_VAL_IF_FAIL(layer_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(layer_data->plane_id > 0, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(value, TDM_ERROR_INVALID_PARAMETER);

    sprd_data = layer_data->sprd_data;
    props = drmModeObjectGetProperties(sprd_data->drm_fd, layer_data->plane_id,
                                       DRM_MODE_OBJECT_PLANE);
    if (props == NULL)
    {
        TDM_ERR("get property failed: %m");
        return TDM_ERROR_OPERATION_FAILED;
    }

    for (i = 0; i < props->count_props; i++)
        if (props->props[i] == id)
        {
            (*value).u32 = (uint)props->prop_values[i];
            break;
        }

    drmModeFreeObjectProperties(props);

    return TDM_ERROR_NONE;
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
    int ret, i, count;

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
    }

    if (display_buffer->fb_id == 0)
    {
        unsigned int width;
        unsigned int height;
        unsigned int format;
        unsigned int handles[4] = {0,};
        unsigned int pitches[4] = {0,};
        unsigned int offsets[4] = {0,};
        unsigned int size;

        width = tbm_surface_get_width(surface);
        height = tbm_surface_get_height(surface);
        format = tbm_surface_get_format(surface);
        count = tbm_surface_internal_get_num_bos(surface);
        for (i = 0; i < count; i++)
        {
            tbm_bo bo = tbm_surface_internal_get_bo(surface, i);
            handles[i] = tbm_bo_get_handle(bo, TBM_DEVICE_DEFAULT).u32;
        }
        count = tbm_surface_internal_get_num_planes(format);
        for (i = 0; i < count; i++)
            tbm_surface_internal_get_plane_data(surface, i, &size, &offsets[i], &pitches[i]);

        ret = drmModeAddFB2(sprd_data->drm_fd, width, height, format,
                            handles, pitches, offsets, &display_buffer->fb_id, 0);
        if (ret < 0)
        {
            TDM_ERR("add fb failed: %m");
            return TDM_ERROR_OPERATION_FAILED;
        }

        if (IS_RGB(format))
            display_buffer->width = pitches[0] >> 2;
        else
            display_buffer->width = pitches[0];
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
