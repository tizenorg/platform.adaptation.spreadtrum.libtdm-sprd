#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "tdm_sprd.h"
typedef struct _tdm_sprd_pp_buffer
{
    int index;
    tbm_surface_h src;
    tbm_surface_h dst;

    struct list_head link;
} tdm_sprd_pp_buffer;

typedef struct _tdm_sprd_pp_data
{
    tdm_sprd_data *sprd_data;

    unsigned int prop_id;

    tdm_info_pp info;
    int info_changed;

    struct list_head pending_buffer_list;
    struct list_head buffer_list;

    tdm_pp_done_handler done_func;
    void *done_user_data;

    int startd;
    int first_event;

    struct list_head link;
} tdm_sprd_pp_data;

#if 1
static tbm_format pp_formats[] =
{
    TBM_FORMAT_XRGB8888,
    TBM_FORMAT_RGB565,
    TBM_FORMAT_YUYV,
    TBM_FORMAT_UYVY,
    TBM_FORMAT_NV12,
    TBM_FORMAT_NV21,
    TBM_FORMAT_YUV420,
    TBM_FORMAT_YVU420,
    TBM_FORMAT_YUV444,
};
#else
static tbm_format *pp_formats = NULL;
#endif

#if 1
#define NUM_PP_FORMAT   (sizeof(pp_formats) / sizeof(pp_formats[0]))
#else
#define NUM_PP_FORMAT 0
#endif

static int pp_list_init;
static struct list_head pp_list;

static int
_get_index(tdm_sprd_pp_data *pp_data)
{
    tdm_sprd_pp_buffer *buffer = NULL;
    int ret = 0;

    while (1)
    {
        int found = 0;
        LIST_FOR_EACH_ENTRY(buffer, &pp_data->pending_buffer_list, link)
        {
            if (ret == buffer->index)
            {
                found = 1;
                break;
            }
        }
        if (!found)
            LIST_FOR_EACH_ENTRY(buffer, &pp_data->buffer_list, link)
            {
                if (ret == buffer->index)
                {
                    found = 1;
                    break;
                }
            }
        if (!found)
            break;
        ret++;
    }

    return ret;
}
#if 1
static tdm_error
_tdm_sprd_pp_set(tdm_sprd_pp_data *pp_data)
{
    tdm_sprd_data *sprd_data = pp_data->sprd_data;
    tdm_info_pp *info = &pp_data->info;
    struct drm_sprd_ipp_property property;
    int ret = 0;

    CLEAR(property);
    property.config[0].ops_id = SPRD_DRM_OPS_SRC;
    property.config[0].fmt = tdm_sprd_format_to_drm_format(info->src_config.format);
    memcpy(&property.config[0].sz, &info->src_config.size, sizeof(tdm_size));
    memcpy(&property.config[0].pos, &info->src_config.pos, sizeof(tdm_pos));
    property.config[1].ops_id = SPRD_DRM_OPS_DST;
    property.config[1].degree = info->transform / 4;
    property.config[1].flip = (info->transform > 3) ? SPRD_DRM_FLIP_HORIZONTAL : 0;
    property.config[1].fmt = tdm_sprd_format_to_drm_format(info->dst_config.format);
    memcpy(&property.config[1].sz, &info->dst_config.size, sizeof(tdm_size));
    memcpy(&property.config[1].pos, &info->dst_config.pos, sizeof(tdm_pos));
    property.cmd = IPP_CMD_M2M;
    property.prop_id = pp_data->prop_id;

    TDM_DBG("src : flip(%x) deg(%d) fmt(%c%c%c%c) sz(%dx%d) pos(%d,%d %dx%d)  ",
            property.config[0].flip, property.config[0].degree, FOURCC_STR(property.config[0].fmt),
            property.config[0].sz.hsize, property.config[0].sz.vsize,
            property.config[0].pos.x, property.config[0].pos.y, property.config[0].pos.w, property.config[0].pos.h);
    TDM_DBG("dst : flip(%x) deg(%d) fmt(%c%c%c%c) sz(%dx%d) pos(%d,%d %dx%d)  ",
            property.config[1].flip, property.config[1].degree, FOURCC_STR(property.config[1].fmt),
            property.config[1].sz.hsize, property.config[1].sz.vsize,
            property.config[1].pos.x, property.config[1].pos.y, property.config[1].pos.w, property.config[1].pos.h);

    ret = ioctl(sprd_data->drm_fd, DRM_IOCTL_SPRD_IPP_SET_PROPERTY, &property);
    if (ret)
    {
        TDM_ERR("failed: %m");
        return TDM_ERROR_OPERATION_FAILED;
    }

    TDM_DBG("success. prop_id(%d) ", property.prop_id);
    pp_data->prop_id = property.prop_id;
    return TDM_ERROR_NONE;
}
#endif
#if 1
static tdm_error
_tdm_sprd_pp_queue(tdm_sprd_pp_data *pp_data, tdm_sprd_pp_buffer *buffer, enum drm_sprd_ipp_buf_type type)
{
    tdm_sprd_data *sprd_data = pp_data->sprd_data;
    struct drm_sprd_ipp_queue_buf buf;
    int i, bo_num, ret = 0;

    CLEAR(buf);
    buf.prop_id = pp_data->prop_id;
    buf.ops_id = SPRD_DRM_OPS_SRC;
    buf.buf_type = type;
    buf.buf_id = buffer->index;
    buf.user_data = (__u64)(uintptr_t)pp_data;
    bo_num = tbm_surface_internal_get_num_bos(buffer->src);
    for (i = 0; i < SPRD_DRM_PLANAR_MAX && i < bo_num; i++)
    {
        tbm_bo bo = tbm_surface_internal_get_bo(buffer->src, i);
        buf.handle[i] = (__u32)tbm_bo_get_handle(bo, TBM_DEVICE_DEFAULT).u32;
    }

    TDM_DBG("prop_id(%d) ops_id(%d) ctrl(%d) id(%d) handles(%x %x %x). ",
            buf.prop_id, buf.ops_id, buf.buf_type, buf.buf_id,
            buf.handle[0], buf.handle[1], buf.handle[2]);

    ret = ioctl(sprd_data->drm_fd, DRM_IOCTL_SPRD_IPP_QUEUE_BUF, &buf);
    if (ret)
    {
        TDM_ERR("src failed. prop_id(%d) op(%d) buf(%d) id(%d). %m",
                buf.prop_id, buf.ops_id, buf.buf_type, buf.buf_id);
        return TDM_ERROR_OPERATION_FAILED;
    }

    CLEAR(buf);
    buf.prop_id = pp_data->prop_id;
    buf.ops_id = SPRD_DRM_OPS_DST;
    buf.buf_type = type;
    buf.buf_id = buffer->index;
    buf.user_data = (__u64)(uintptr_t)pp_data;
    bo_num = tbm_surface_internal_get_num_bos(buffer->dst);
    for (i = 0; i < SPRD_DRM_PLANAR_MAX && i < bo_num; i++)
    {
        tbm_bo bo = tbm_surface_internal_get_bo(buffer->dst, i);
        buf.handle[i] = (__u32)tbm_bo_get_handle(bo, TBM_DEVICE_DEFAULT).u32;
    }

    TDM_DBG("prop_id(%d) ops_id(%d) ctrl(%d) id(%d) handles(%x %x %x). ",
            buf.prop_id, buf.ops_id, buf.buf_type, buf.buf_id,
            buf.handle[0], buf.handle[1], buf.handle[2]);

    ret = ioctl(sprd_data->drm_fd, DRM_IOCTL_SPRD_IPP_QUEUE_BUF, &buf);
    if (ret)
    {
        TDM_ERR("dst failed. prop_id(%d) op(%d) buf(%d) id(%d). %m",
                buf.prop_id, buf.ops_id, buf.buf_type, buf.buf_id);
        return TDM_ERROR_OPERATION_FAILED;
    }

    TDM_DBG("success. prop_id(%d)", buf.prop_id);

    return TDM_ERROR_NONE;
}
#endif
#if 1 
static tdm_error
_tdm_sprd_pp_cmd(tdm_sprd_pp_data *pp_data, enum drm_sprd_ipp_ctrl cmd)
{
    tdm_sprd_data *sprd_data = pp_data->sprd_data;
    struct drm_sprd_ipp_cmd_ctrl ctrl;
    int ret = 0;

    ctrl.prop_id = pp_data->prop_id;
    ctrl.ctrl = cmd;

    TDM_DBG("prop_id(%d) ctrl(%d). ", ctrl.prop_id, ctrl.ctrl);

    ret = ioctl(sprd_data->drm_fd, DRM_IOCTL_SPRD_IPP_CMD_CTRL, &ctrl);
    if (ret)
    {
        TDM_ERR("failed. prop_id(%d) ctrl(%d). %m", ctrl.prop_id, ctrl.ctrl);
        return TDM_ERROR_OPERATION_FAILED;
    }

    TDM_DBG("success. prop_id(%d) ", ctrl.prop_id);

    return TDM_ERROR_NONE;
}
#endif
void
tdm_sprd_pp_handler(int fd, tdm_sprd_data *sprd_data_p, void* hw_event_data_p)
{
    RETURN_VOID_IF_FAIL(sprd_data_p);
    RETURN_VOID_IF_FAIL(hw_event_data_p);
    struct drm_sprd_ipp_event *hw_ipp_p = (struct drm_sprd_ipp_event *) hw_event_data_p;
    tdm_sprd_pp_data *found = NULL, *pp_data = (tdm_sprd_pp_data *)(unsigned long) hw_ipp_p->user_data;
    tdm_sprd_pp_buffer *b = NULL, *bb = NULL, *dequeued_buffer = NULL;

    if (!pp_data || !hw_ipp_p->buf_id)
    {
        TDM_ERR("invalid params");
        return;
    }

    LIST_FOR_EACH_ENTRY(found, &pp_list, link)
    {
        if (found == pp_data)
            break;
    }
    if (!found)
        return;

    TDM_DBG("pp_data(%p) index(%d, %d)", pp_data, hw_ipp_p->buf_id[0], hw_ipp_p->buf_id[1]);

    LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->buffer_list, link)
    {
        if (hw_ipp_p->buf_id[0] == b->index)
        {
            dequeued_buffer = b;
            LIST_DEL(&dequeued_buffer->link);
            TDM_DBG("dequeued: %d", dequeued_buffer->index);
            break;
        }
    }

    if (!dequeued_buffer)
    {
        TDM_ERR("not found buffer index: %d", hw_ipp_p->buf_id[0]);
        return;
    }

    if (!pp_data->first_event)
    {
        TDM_DBG("pp(%p) got a first event. ", pp_data);
        pp_data->first_event = 1;
    }

    if (pp_data->done_func)
        pp_data->done_func(pp_data,
                           dequeued_buffer->src,
                           dequeued_buffer->dst,
                           pp_data->done_user_data);
    free(dequeued_buffer);
}

tdm_error
tdm_sprd_pp_get_capability(tdm_sprd_data *sprd_data, tdm_caps_pp *caps)
{
    int i;

    if (!caps)
    {
        TDM_ERR("invalid params");
        return TDM_ERROR_INVALID_PARAMETER;
    }

    caps->capabilities = TDM_PP_CAPABILITY_ASYNC;

    caps->format_count = NUM_PP_FORMAT;
    caps->formats = NULL;
    if (NUM_PP_FORMAT)
    {
        /* will be freed in frontend */
        caps->formats = calloc(1, sizeof pp_formats);
        if (!caps->formats)
        {
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
    caps->preferred_align = 16;

    return TDM_ERROR_NONE;
}

tdm_pp*
tdm_sprd_pp_create(tdm_sprd_data *sprd_data, tdm_error *error)
{
    tdm_sprd_pp_data *pp_data = calloc(1, sizeof(tdm_sprd_pp_data));
    if (!pp_data)
    {
        TDM_ERR("alloc failed");
        if (error)
            *error = TDM_ERROR_OUT_OF_MEMORY;
        return NULL;
    }

    pp_data->sprd_data = sprd_data;

    LIST_INITHEAD(&pp_data->pending_buffer_list);
    LIST_INITHEAD(&pp_data->buffer_list);

    if (!pp_list_init)
    {
        pp_list_init = 1;
        LIST_INITHEAD(&pp_list);
    }
    LIST_ADDTAIL(&pp_data->link, &pp_list);

    return pp_data;
}

void
sprd_pp_destroy(tdm_pp *pp)
{
    tdm_sprd_pp_data *pp_data = pp;
    tdm_sprd_pp_buffer *b = NULL, *bb = NULL;

    if (!pp_data)
        return;
    LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->pending_buffer_list, link)
    {
        LIST_DEL(&b->link);
        free(b);
    }

    LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->buffer_list, link)
    {
        LIST_DEL(&b->link);
#if 1
        _tdm_sprd_pp_queue(pp_data, b, IPP_BUF_DEQUEUE);
#endif
        free(b);
    }
#if 1
    if (pp_data->prop_id)
        _tdm_sprd_pp_cmd(pp_data, IPP_CTRL_STOP);
#endif
    LIST_DEL(&pp_data->link);

    free(pp_data);
}

tdm_error
sprd_pp_set_info(tdm_pp *pp, tdm_info_pp *info)
{
    tdm_sprd_pp_data *pp_data = pp;

    RETURN_VAL_IF_FAIL(pp_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(info, TDM_ERROR_INVALID_PARAMETER);

    if (info->sync)
    {
        TDM_ERR("not support sync mode currently");
        return TDM_ERROR_INVALID_PARAMETER;
    }

    pp_data->info = *info;
    pp_data->info_changed = 1;

    return TDM_ERROR_NONE;
}

tdm_error
sprd_pp_attach(tdm_pp *pp, tbm_surface_h src, tbm_surface_h dst)
{
    tdm_sprd_pp_data *pp_data = pp;
    tdm_sprd_pp_buffer *buffer;

    RETURN_VAL_IF_FAIL(pp_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(src, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(dst, TDM_ERROR_INVALID_PARAMETER);

    buffer = calloc(1, sizeof(tdm_sprd_pp_buffer));
    if (!buffer)
    {
        TDM_ERR("alloc failed");
        return TDM_ERROR_NONE;
    }

    LIST_ADDTAIL(&buffer->link, &pp_data->pending_buffer_list);
    buffer->index = _get_index(pp_data);
    buffer->src = src;
    buffer->dst = dst;

    return TDM_ERROR_NONE;
}

tdm_error
sprd_pp_commit(tdm_pp *pp)
{
#if 1
    tdm_sprd_pp_data *pp_data = pp;
    tdm_sprd_pp_buffer *b = NULL, *bb = NULL;
    tdm_error ret = TDM_ERROR_NONE;
    RETURN_VAL_IF_FAIL(pp_data, TDM_ERROR_INVALID_PARAMETER);
    if (pp_data->info_changed)
    {
        if (pp_data->startd)
            _tdm_sprd_pp_cmd(pp_data, IPP_CTRL_PAUSE);

        ret = _tdm_sprd_pp_set(pp_data);
        if (ret < 0)
            return TDM_ERROR_OPERATION_FAILED;
    }

    LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->pending_buffer_list, link)
    {
        LIST_DEL(&b->link);
        _tdm_sprd_pp_queue(pp_data, b, IPP_BUF_ENQUEUE);
        TDM_DBG("queued: %d", b->index);
        LIST_ADDTAIL(&b->link, &pp_data->buffer_list);
    }

    if (pp_data->info_changed)
    {
        pp_data->info_changed = 0;

        if (!pp_data->startd)
        {
            pp_data->startd = 1;
            _tdm_sprd_pp_cmd(pp_data, IPP_CTRL_PLAY);
        }
        else
            _tdm_sprd_pp_cmd(pp_data, IPP_CTRL_RESUME);
    }
#endif
    return TDM_ERROR_NONE;
}

tdm_error
sprd_pp_set_done_handler(tdm_pp *pp, tdm_pp_done_handler func, void *user_data)
{
    tdm_sprd_pp_data *pp_data = pp;

    RETURN_VAL_IF_FAIL(pp_data, TDM_ERROR_INVALID_PARAMETER);
    RETURN_VAL_IF_FAIL(func, TDM_ERROR_INVALID_PARAMETER);

    pp_data->done_func = func;
    pp_data->done_user_data = user_data;

    return TDM_ERROR_NONE;
}
