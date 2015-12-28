#ifndef _TDM_SPRD_H_
#define _TDM_SPRD_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <tbm_surface.h>
#include <tbm_surface_internal.h>
#include <tdm_backend.h>
#include <tdm_log.h>
#include <tdm_list.h>
#include "sprd_pp_7727.h"
/* sprd backend functions (display) */
tdm_error    sprd_display_get_capabilitiy(tdm_backend_data *bdata, tdm_caps_display *caps);
tdm_error    sprd_display_get_pp_capability(tdm_backend_data *bdata, tdm_caps_pp *caps);
tdm_output** sprd_display_get_outputs(tdm_backend_data *bdata, int *count, tdm_error *error);
tdm_error    sprd_display_get_fd(tdm_backend_data *bdata, int *fd);
tdm_error    sprd_display_handle_events(tdm_backend_data *bdata);
tdm_pp*      sprd_display_create_pp(tdm_backend_data *bdata, tdm_error *error);
tdm_error    sprd_output_get_capability(tdm_output *output, tdm_caps_output *caps);
tdm_layer**  sprd_output_get_layers(tdm_output *output, int *count, tdm_error *error);
tdm_error    sprd_output_set_property(tdm_output *output, unsigned int id, tdm_value value);
tdm_error    sprd_output_get_property(tdm_output *output, unsigned int id, tdm_value *value);
tdm_error    sprd_output_wait_vblank(tdm_output *output, int interval, int sync, void *user_data);
tdm_error    sprd_output_set_vblank_handler(tdm_output *output, tdm_output_vblank_handler func);
tdm_error    sprd_output_commit(tdm_output *output, int sync, void *user_data);
tdm_error    sprd_output_set_commit_handler(tdm_output *output, tdm_output_commit_handler func);
tdm_error    sprd_output_set_dpms(tdm_output *output, tdm_output_dpms dpms_value);
tdm_error    sprd_output_get_dpms(tdm_output *output, tdm_output_dpms *dpms_value);
tdm_error    sprd_output_set_mode(tdm_output *output, const tdm_output_mode *mode);
tdm_error    sprd_output_get_mode(tdm_output *output, const tdm_output_mode **mode);
tdm_error    sprd_layer_get_capability(tdm_layer *layer, tdm_caps_layer *caps);
tdm_error    sprd_layer_set_property(tdm_layer *layer, unsigned int id, tdm_value value);
tdm_error    sprd_layer_get_property(tdm_layer *layer, unsigned int id, tdm_value *value);
tdm_error    sprd_layer_set_info(tdm_layer *layer, tdm_info_layer *info);
tdm_error    sprd_layer_get_info(tdm_layer *layer, tdm_info_layer *info);
tdm_error    sprd_layer_set_buffer(tdm_layer *layer, tbm_surface_h buffer);
tdm_error    sprd_layer_unset_buffer(tdm_layer *layer);
void         sprd_pp_destroy(tdm_pp *pp);
tdm_error    sprd_pp_set_info(tdm_pp *pp, tdm_info_pp *info);
tdm_error    sprd_pp_attach(tdm_pp *pp, tbm_surface_h src, tbm_surface_h dst);
tdm_error    sprd_pp_commit(tdm_pp *pp);
tdm_error    sprd_pp_set_done_handler(tdm_pp *pp, tdm_pp_done_handler func, void *user_data);


/* sprd module internal macros, structures, functions */
#define NEVER_GET_HERE() TDM_ERR("** NEVER GET HERE **")

#define C(b,m)              (((b) >> (m)) & 0xFF)
#define B(c,s)              ((((unsigned int)(c)) & 0xff) << (s))
#define FOURCC(a,b,c,d)     (B(d,24) | B(c,16) | B(b,8) | B(a,0))
#define FOURCC_STR(id)      C(id,0), C(id,8), C(id,16), C(id,24)

#define IS_RGB(format)      (format == TBM_FORMAT_XRGB8888 || format == TBM_FORMAT_ARGB8888 || \
                             format == TBM_FORMAT_XBGR8888 || format == TBM_FORMAT_ABGR8888)

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define SWAP(a, b)  ({int t; t = a; a = b; b = t;})
#define ROUNDUP(x)  (ceil (floor ((float)(height) / 4)))

#define ALIGN_TO_16B(x)    ((((x) + (1 <<  4) - 1) >>  4) <<  4)
#define ALIGN_TO_32B(x)    ((((x) + (1 <<  5) - 1) >>  5) <<  5)
#define ALIGN_TO_128B(x)   ((((x) + (1 <<  7) - 1) >>  7) <<  7)
#define ALIGN_TO_2KB(x)    ((((x) + (1 << 11) - 1) >> 11) << 11)
#define ALIGN_TO_8KB(x)    ((((x) + (1 << 13) - 1) >> 13) << 13)
#define ALIGN_TO_64KB(x)   ((((x) + (1 << 16) - 1) >> 16) << 16)

#define RETURN_VAL_IF_FAIL(cond, val) {\
    if (!(cond)) {\
        TDM_ERR("'%s' failed", #cond);\
        return val;\
    }\
}

#define RETURN_VOID_IF_FAIL(cond) RETURN_VAL_IF_FAIL(cond,)

typedef struct _tdm_sprd_data
{
    struct list_head events_list;
    tdm_display *dpy;
    int drm_fd;
    int fb_fd;
    void * sprd_drm_dev;

    /* If true, it means that the device has many planes for one crtc. If false,
     * planes are dedicated to specific crtc.
     */
    int has_zpos_info;

    /* If has_zpos_info is false and is_immutable_zpos is true, it means that
     * planes are dedicated to specific crtc.
     */
    int is_immutable_zpos;

    drmModeResPtr mode_res;
    drmModePlaneResPtr plane_res;
    struct list_head output_list;
    struct list_head buffer_list;
} tdm_sprd_data;

uint32_t     tdm_sprd_format_to_drm_format(tbm_format format);
tbm_format   tdm_sprd_format_to_tbm_format(uint32_t format);

tdm_error    tdm_sprd_display_create_output_list(tdm_sprd_data *sprd_data);
void         tdm_sprd_display_destroy_output_list(tdm_sprd_data *sprd_data);
tdm_error    tdm_sprd_display_create_layer_list(tdm_sprd_data *sprd_data);
tdm_error    tdm_sprd_display_set_property(tdm_sprd_data *sprd_data,
                                             unsigned int obj_id, unsigned int obj_type,
                                             const char *name, unsigned int value);
tdm_error    tdm_sprd_display_get_property(tdm_sprd_data *sprd_data,
                                             unsigned int obj_id, unsigned int obj_type,
                                             const char *name, unsigned int *value, int *is_immutable);

tdm_error    tdm_sprd_pp_get_capability(tdm_sprd_data *sprd_data, tdm_caps_pp *caps);
tdm_pp*      tdm_sprd_pp_create(tdm_sprd_data *sprd_data, tdm_error *error);
void         tdm_sprd_pp_handler(int fd, tdm_sprd_data *sprd_data_p, void* hw_event_data);
tdm_error   tdm_sprd_display_create_event_list(tdm_sprd_data *sprd_data);
void        tdm_sprd_display_destroy_event_list(tdm_sprd_data *sprd_data);
#endif /* _TDM_SPRD_H_ */
