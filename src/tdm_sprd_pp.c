#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#define PP_MAX_STEP 2
#include "tdm_sprd.h"
typedef struct _tdm_sprd_pp_buffer
{
    int index;
    tbm_surface_h src;
    tbm_surface_h dst;
    struct list_head link;
} tdm_sprd_pp_buffer;

enum
{
    IPP_STOP = 0,
    IPP_RUN = 1,
    IPP_PAUSE = 2
};
typedef struct _tdm_sprd_pp_data
{
    tdm_sprd_data *sprd_data;

    unsigned int prop_id;

    tdm_info_pp info;
    int info_changed;
    tdm_info_pp tasks_array[PP_MAX_STEP];
    tdm_sprd_pp_buffer task_buffers[PP_MAX_STEP];
    unsigned int tasks_array_size;
    unsigned int current_step;
    tbm_surface_h temp_buffer[PP_MAX_STEP];
    struct list_head pending_buffer_list;
    struct list_head buffer_list;
    struct list_head temp_buffer_list;

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
static unsigned int
_tdm_sprd_pp_set(tdm_sprd_pp_data *pp_data, tdm_info_pp *info, unsigned int prop_id)
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
        return 0;
    }

    TDM_DBG("success. prop_id(%u) ", property.prop_id);
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

    CLEAR(buf);
    buf.prop_id = prop_id;
    buf.ops_id = SPRD_DRM_OPS_SRC;
    buf.buf_type = type;
    buf.buf_id = 0;
    buf.user_data = (__u64)(uintptr_t)pp_data;
    bo_num = tbm_surface_internal_get_num_bos(src);
    for (i = 0; i < SPRD_DRM_PLANAR_MAX && i < bo_num; i++)
    {
        tbm_bo bo = tbm_surface_internal_get_bo(src, i);
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
    buf.prop_id = prop_id;
    buf.ops_id = SPRD_DRM_OPS_DST;
    buf.buf_type = type;
    buf.buf_id = 0;
    buf.user_data = (__u64)(uintptr_t)pp_data;
    bo_num = tbm_surface_internal_get_num_bos(dst);
    for (i = 0; i < SPRD_DRM_PLANAR_MAX && i < bo_num; i++)
    {
        tbm_bo bo = tbm_surface_internal_get_bo(dst, i);
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
_tdm_sprd_pp_cmd(tdm_sprd_pp_data *pp_data, unsigned int prop_id, enum drm_sprd_ipp_ctrl cmd)
{
    tdm_sprd_data *sprd_data = pp_data->sprd_data;
    struct drm_sprd_ipp_cmd_ctrl ctrl;
    int ret = 0;
    ctrl.prop_id = prop_id;
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
tdm_sprd_pp_handler(struct drm_sprd_ipp_event *hw_ipp_p)
{
    RETURN_VOID_IF_FAIL(hw_ipp_p);
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

    TDM_DBG("pp_data(%p) index(%d, %d) prop_id(%u)", pp_data, hw_ipp_p->buf_id[0], hw_ipp_p->buf_id[1], hw_ipp_p->prop_id);
    if (hw_ipp_p->prop_id != pp_data->prop_id)
    {
        TDM_WRN("Wrong PP event");
        return;
    }
    if (!pp_data->first_event)
    {
        TDM_DBG("pp(%p) got a first event. ", pp_data);
        pp_data->first_event = 1;
    }
    LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->buffer_list, link)
    {
        if (pp_data->task_buffers[pp_data->current_step].dst == b->dst)
        {
            dequeued_buffer = b;
            LIST_DEL(&dequeued_buffer->link);
            TDM_DBG("dequeued: %d", dequeued_buffer->index);
            pp_data->task_buffers[pp_data->current_step].dst = NULL;
            pp_data->task_buffers[pp_data->current_step].src = NULL;
            if (pp_data->done_func)
                pp_data->done_func(pp_data,
                                   dequeued_buffer->src,
                                   dequeued_buffer->dst,
                                   pp_data->done_user_data);
            free(dequeued_buffer);
            return;
        }
    }
    pp_data->task_buffers[pp_data->current_step].dst = NULL;
    pp_data->task_buffers[pp_data->current_step].src = NULL;
    pp_data->current_step++;
    if (pp_data->current_step >= pp_data->tasks_array_size)
    {
        TDM_ERR("Wrong pp queue");
        return;
    }
    if (pp_data->startd == IPP_RUN)
    {
        _tdm_sprd_pp_cmd(pp_data, pp_data->prop_id, IPP_CTRL_PAUSE);
        pp_data->startd = IPP_PAUSE;
    }
    if ((pp_data->prop_id = _tdm_sprd_pp_set(pp_data, &pp_data->tasks_array[pp_data->current_step],
                                             pp_data->prop_id)) <= 0)
    {
        TDM_ERR("Can't setup next pp step");
        return;
    }
    _tdm_sprd_pp_queue(pp_data, pp_data->prop_id, pp_data->task_buffers[pp_data->current_step].src,
                       pp_data->task_buffers[pp_data->current_step].dst, IPP_BUF_ENQUEUE);
    if (pp_data->startd == IPP_STOP)
    {
        pp_data->startd = IPP_RUN;
        _tdm_sprd_pp_cmd(pp_data, pp_data->prop_id, IPP_CTRL_PLAY);
    }
    else if(pp_data->startd == IPP_PAUSE)
    {
        pp_data->startd = IPP_RUN;
        _tdm_sprd_pp_cmd(pp_data, pp_data->prop_id, IPP_CTRL_RESUME);
    }
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
    int i;
    if (!pp_data)
        return;
    LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->pending_buffer_list, link)
    {
        LIST_DEL(&b->link);
        free(b);
    }
    if (pp_data->prop_id && pp_data->task_buffers[pp_data->current_step].src && pp_data->task_buffers[pp_data->current_step].dst)
    {
        _tdm_sprd_pp_queue(pp_data, pp_data->prop_id, pp_data->task_buffers[pp_data->current_step].src,
                           pp_data->task_buffers[pp_data->current_step].dst, IPP_BUF_DEQUEUE);
    }
    LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->buffer_list, link)
    {
        LIST_DEL(&b->link);
        free(b);
    }
    for (i = 0; i < PP_MAX_STEP; i++)
        if (pp_data->temp_buffer[i])
        {
            tbm_surface_destroy(pp_data->temp_buffer[i]);
            pp_data->temp_buffer[i] = NULL;
        }
    if (pp_data->prop_id)
        _tdm_sprd_pp_cmd(pp_data, pp_data->prop_id, IPP_CTRL_STOP);
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

static tdm_error
_sprd_pp_get_scale_leap(unsigned int src, unsigned int dst,
                        unsigned int *leap_array, unsigned int *size)
{
    unsigned int i = 0;
    unsigned int ratio, next_value = src;
    TDM_DBG(" PP scale src %u", src);
    for (i = 0; i < PP_MAX_STEP; i++)
    {
        ratio = PP_RATIO(next_value, dst);
        if ((ratio >= PP_UP_MAX_RATIO) && (ratio <= PP_DOWN_MIN_RATIO))
            break;
        if (ratio < PP_UP_MAX_RATIO)
        {
            next_value = PP_RATIO(next_value, PP_UP_MAX_RATIO);
        }
        if (ratio > PP_DOWN_MIN_RATIO)
        {
            next_value =  PP_RATIO(next_value, PP_DOWN_MIN_RATIO);
        }
        if (leap_array)
            leap_array[i] = next_value;
        TDM_DBG("[%u] => %u", i, next_value);
    }
    if (i == PP_MAX_STEP)
    {
        TDM_ERR("Can't scale. Reaching maximum iteration count %d", PP_MAX_STEP);
        return TDM_ERROR_OPERATION_FAILED;
    }
    TDM_DBG("[%u] dst => %u", i, dst);
    if (leap_array)
        leap_array[i] = dst;
    if (size)
        *size = i+1;
    return TDM_ERROR_NONE;
}

static tdm_error
_sprd_pp_make_roadmap(tdm_sprd_pp_data* pp_data)
{
    unsigned int height_leap[PP_MAX_STEP], width_leap[PP_MAX_STEP],
            height_leap_size = 0, width_leap_size = 0, i, max_size;
    if (_sprd_pp_get_scale_leap(pp_data->info.src_config.pos.h, pp_data->info.dst_config.pos.h,
                                height_leap, &height_leap_size) != TDM_ERROR_NONE)
    {
        TDM_ERR("height %u -> %u ratio out of range", pp_data->info.src_config.pos.h, pp_data->info.dst_config.pos.h);
        return TDM_ERROR_OPERATION_FAILED;
    }
    if (_sprd_pp_get_scale_leap(pp_data->info.src_config.pos.w, pp_data->info.dst_config.pos.w,
                                width_leap, &width_leap_size) != TDM_ERROR_NONE)
    {
        TDM_ERR("width %u -> %u ratio out of range", pp_data->info.src_config.pos.w, pp_data->info.dst_config.pos.w);
        return TDM_ERROR_OPERATION_FAILED;
    }
    pp_data->tasks_array[0] = pp_data->info;
    pp_data->tasks_array[0].dst_config.pos.h = height_leap[0];
    pp_data->tasks_array[0].dst_config.pos.w = width_leap[0];
    max_size = (width_leap_size > height_leap_size ? width_leap_size : height_leap_size);
    for (i = 1; i < max_size; i++)
    {
        if (pp_data->temp_buffer[i-1])
        {
            tbm_surface_destroy(pp_data->temp_buffer[i-1]);
        }
        pp_data->temp_buffer[i-1] = tbm_surface_internal_create_with_flags( width_leap[(((i-1) < width_leap_size) ? (i-1) : (width_leap_size-1))],
                                                       height_leap[(((i-1) < height_leap_size) ? (i-1): (height_leap_size-1))],
                                                       pp_data->info.dst_config.format, TBM_BO_SCANOUT);
        if (pp_data->temp_buffer[i-1] == NULL)
        {
            TDM_ERR("Can't alloc buffer");
            return TDM_ERROR_OUT_OF_MEMORY;
        }
        tbm_surface_info_s surface_info;
        tbm_surface_map(pp_data->temp_buffer[i-1], TBM_SURF_OPTION_READ, &surface_info);
        tbm_surface_unmap(pp_data->temp_buffer[i-1]);
        if (tbm_surface_internal_get_num_planes(pp_data->info.dst_config.format) > 1)
        {
            pp_data->tasks_array[i-1].dst_config.size.h = surface_info.planes[0].stride;
        }
        else
        {
            TDM_DBG("BPP = %u", (tbm_surface_internal_get_bpp(pp_data->info.dst_config.format)));
            pp_data->tasks_array[i-1].dst_config.size.h =
                    (surface_info.planes[0].stride << 3)/tbm_surface_internal_get_bpp(pp_data->info.dst_config.format);
        }
        pp_data->tasks_array[i-1].dst_config.size.v = tbm_surface_get_height(pp_data->temp_buffer[i-1]);
        pp_data->tasks_array[i-1].dst_config.pos.w = width_leap[(((i-1) < width_leap_size) ? (i-1) : (width_leap_size-1))];
        pp_data->tasks_array[i-1].dst_config.pos.h = height_leap[(((i-1) < height_leap_size) ? (i-1): (height_leap_size-1))];
        pp_data->tasks_array[i].transform = TDM_TRANSFORM_NORMAL;
        pp_data->tasks_array[i].src_config.format = pp_data->tasks_array[i-1].dst_config.format;
        pp_data->tasks_array[i].dst_config.format = pp_data->tasks_array[i-1].dst_config.format;
        pp_data->tasks_array[i].src_config = pp_data->tasks_array[i-1].dst_config;
        pp_data->tasks_array[i].dst_config = pp_data->tasks_array[i-1].dst_config;
        pp_data->tasks_array[i].src_config.pos.x = 0;
        pp_data->tasks_array[i].src_config.pos.y = 0;
        pp_data->tasks_array[i].dst_config.pos.x = 0;
        pp_data->tasks_array[i].dst_config.pos.y = 0;
        pp_data->tasks_array[i].sync = pp_data->tasks_array[i-1].sync;
        pp_data->tasks_array[i].flags = pp_data->tasks_array[i-1].flags;
    }
    pp_data->tasks_array[max_size-1].dst_config = pp_data->info.dst_config;
    pp_data->tasks_array_size = max_size;
    pp_data->current_step = 0;
    return TDM_ERROR_NONE;
}

static tdm_error
_sprd_pp_reconfigure_buffers(tdm_sprd_pp_data *pp_data)
{
    int i;
    tdm_sprd_pp_buffer *main_buffer = NULL, *b = NULL, *bb = NULL;
    LIST_FOR_EACH_ENTRY_SAFE(b, bb, &pp_data->pending_buffer_list, link)
    {
        main_buffer = b;
        LIST_DEL(&b->link);
        LIST_ADDTAIL(&b->link, &pp_data->buffer_list);
    }
    if (main_buffer == NULL)
    {
        TDM_DBG("Nothing to do");
        return TDM_ERROR_NONE;
    }
    pp_data->task_buffers[0].src = main_buffer->src;
    pp_data->task_buffers[0].dst = main_buffer->dst;
    for (i = 1; i < pp_data->tasks_array_size; i++)
    {
        pp_data->task_buffers[i-1].dst = pp_data->temp_buffer[i-1];
        pp_data->task_buffers[i].src = pp_data->temp_buffer[i-1];
        pp_data->task_buffers[i].dst = main_buffer->dst;
    }
    return TDM_ERROR_NONE;
}

tdm_error
sprd_pp_commit(tdm_pp *pp)
{
#if 1
    tdm_sprd_pp_data *pp_data = pp;
    RETURN_VAL_IF_FAIL(pp_data, TDM_ERROR_INVALID_PARAMETER);
    if (pp_data->info_changed)
    {
        if (_sprd_pp_make_roadmap(pp_data) != TDM_ERROR_NONE)
        {
            return TDM_ERROR_OPERATION_FAILED;
        }
    }
    if (pp_data->info_changed || pp_data->tasks_array_size > 1)
    {
        if (pp_data->startd == IPP_RUN)
        {
            _tdm_sprd_pp_cmd(pp_data, pp_data->prop_id, IPP_CTRL_PAUSE);
            pp_data->startd = IPP_PAUSE;
        }
        if ((pp_data->prop_id = _tdm_sprd_pp_set(pp_data, &pp_data->tasks_array[0], pp_data->prop_id)) <= 0)
            return TDM_ERROR_OPERATION_FAILED;
        pp_data->info_changed = 0;
    }
    if (_sprd_pp_reconfigure_buffers(pp_data) != TDM_ERROR_NONE)
    {
        return TDM_ERROR_OPERATION_FAILED;
    }
    if (_tdm_sprd_pp_queue(pp_data, pp_data->prop_id, pp_data->task_buffers[0].src,
                           pp_data->task_buffers[0].dst, IPP_BUF_ENQUEUE) != TDM_ERROR_NONE)
    {
        return TDM_ERROR_OPERATION_FAILED;
    }
    pp_data->current_step = 0;
    if (pp_data->startd == IPP_STOP)
    {
        pp_data->startd = IPP_RUN;
        _tdm_sprd_pp_cmd(pp_data, pp_data->prop_id, IPP_CTRL_PLAY);
    }
    else if(pp_data->startd == IPP_PAUSE)
    {
        pp_data->startd = IPP_RUN;
        _tdm_sprd_pp_cmd(pp_data, pp_data->prop_id, IPP_CTRL_RESUME);
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
