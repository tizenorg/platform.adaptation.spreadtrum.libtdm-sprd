#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if HAVE_UDEV
#include <libudev.h>
#endif

#include "tdm_sprd.h"
#include <tdm_helper.h>

#define SPRD_DRM_NAME "sprd"

static tdm_func_display sprd_func_display =
{
    sprd_display_get_capabilitiy,
    sprd_display_get_pp_capability,
    NULL,  //display_get_capture_capability
    sprd_display_get_outputs,
    sprd_display_get_fd,
    sprd_display_handle_events,
    sprd_display_create_pp,
};

static tdm_func_output sprd_func_output =
{
    sprd_output_get_capability,
    sprd_output_get_layers,
    sprd_output_set_property,
    sprd_output_get_property,
    sprd_output_wait_vblank,
    sprd_output_set_vblank_handler,
    sprd_output_commit,
    sprd_output_set_commit_handler,
    sprd_output_set_dpms,
    sprd_output_get_dpms,
    sprd_output_set_mode,
    sprd_output_get_mode,
    NULL,   //output_create_capture
};

static tdm_func_layer sprd_func_layer =
{
    sprd_layer_get_capability,
    sprd_layer_set_property,
    sprd_layer_get_property,
    sprd_layer_set_info,
    sprd_layer_get_info,
    sprd_layer_set_buffer,
    sprd_layer_unset_buffer,
    NULL,    //layer_set_video_pos
    NULL,    //layer_create_capture
};

static tdm_func_pp sprd_func_pp =
{
    sprd_pp_destroy,
    sprd_pp_set_info,
    sprd_pp_attach,
    sprd_pp_commit,
    sprd_pp_set_done_handler,
};

static tdm_sprd_data *sprd_data;

static int
_tdm_sprd_open_drm(void)
{
    int fd = -1;

    fd = drmOpen(SPRD_DRM_NAME, NULL);
    if (fd < 0)
    {
        TDM_ERR("Cannot open '%s' drm", SPRD_DRM_NAME);
    }

#ifdef HAVE_UDEV
    if (fd < 0)
    {
        struct udev *udev;
        struct udev_enumerate *e;
        struct udev_list_entry *entry;
        struct udev_device *device, *drm_device, *device_parent;
        const char *filename;

        TDM_WRN("Cannot open drm device.. search by udev");
        udev = udev_new();
        if (!udev)
        {
            TDM_ERR("fail to initialize udev context\n");
            goto close_l;
        }

        /* Will try to find sys path /sprd-drm/drm/card0 */
        e = udev_enumerate_new(udev);
        udev_enumerate_add_match_subsystem(e, "drm");
        udev_enumerate_add_match_sysname(e, "card[0-9]*");
        udev_enumerate_scan_devices(e);

        drm_device = NULL;
        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e))
        {
            device = udev_device_new_from_syspath(udev_enumerate_get_udev(e),
                                                  udev_list_entry_get_name
                                                  (entry));
            device_parent = udev_device_get_parent(device);
            /* Not need unref device_parent. device_parent and device have same refcnt */
            if (device_parent)
            {
                if (strcmp(udev_device_get_sysname(device_parent), "sprd-drm") == 0)
                {
                    drm_device = device;
                    TDM_DBG("Found drm device: '%s' (%s)\n",
                            udev_device_get_syspath(drm_device),
                            udev_device_get_sysname(device_parent));
                    break;
                }
            }
            udev_device_unref(device);
        }

        if (drm_device == NULL)
        {
            TDM_ERR("fail to find drm device\n");
            udev_enumerate_unref(e);
            udev_unref(udev);
            goto close_l;
        }

        filename = udev_device_get_devnode(drm_device);

        fd = open(filename, O_RDWR | O_CLOEXEC);
        if (fd < 0)
        {
            TDM_ERR("Cannot open drm device(%s)\n", filename);
        }
        udev_device_unref(drm_device);
        udev_enumerate_unref(e);
        udev_unref(udev);
    }
close_l:
#endif

#ifdef SET_DRM_IRQ
    drmCtlInstHandler (fd, 78);
#endif

    return fd;
}

void
tdm_sprd_deinit(tdm_backend_data *bdata)
{
    if (sprd_data != bdata)
        return;

    TDM_INFO("deinit");
    tdm_sprd_display_destroy_output_list(sprd_data);
    tdm_sprd_display_destroy_event_list(sprd_data);

    free(sprd_data);
    sprd_data = NULL;
}

tdm_backend_data*
tdm_sprd_init(tdm_display *dpy, tdm_error *error)
{
    tdm_error ret;

    if (!dpy)
    {
        TDM_ERR("display is null");
        if (error)
            *error = TDM_ERROR_INVALID_PARAMETER;
        return NULL;
    }

    if (sprd_data)
    {
        TDM_ERR("failed_l: init twice");
        if (error)
            *error = TDM_ERROR_BAD_REQUEST;
        return NULL;
    }

    sprd_data = calloc(1, sizeof(tdm_sprd_data));
    if (!sprd_data)
    {
        TDM_ERR("alloc failed_l");
        if (error)
            *error = TDM_ERROR_OUT_OF_MEMORY;
        return NULL;
    }

    LIST_INITHEAD(&sprd_data->output_list);
    LIST_INITHEAD(&sprd_data->buffer_list);
    LIST_INITHEAD(&sprd_data->events_list);

    ret = tdm_backend_register_func_display(dpy, &sprd_func_display);
    if (ret != TDM_ERROR_NONE)
        goto failed_l;

    ret = tdm_backend_register_func_output(dpy, &sprd_func_output);
    if (ret != TDM_ERROR_NONE)
        goto failed_l;

    ret = tdm_backend_register_func_layer(dpy, &sprd_func_layer);
    if (ret != TDM_ERROR_NONE)
        goto failed_l;

    ret = tdm_backend_register_func_pp(dpy, &sprd_func_pp);
    if (ret != TDM_ERROR_NONE)
        goto failed_l;

    sprd_data->dpy = dpy;
    sprd_data->drm_fd = -1;

    if (sprd_data->drm_fd < 0)
        sprd_data->drm_fd = _tdm_sprd_open_drm();

    if (sprd_data->drm_fd < 0)
    {
        ret = TDM_ERROR_OPERATION_FAILED;
        goto failed_l;
    }

#if 0
    if (drmSetClientCap(sprd_data->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0)
        TDM_WRN("Set DRM_CLIENT_CAP_UNIVERSAL_PLANES failed_l");
#endif

    ret = tdm_sprd_display_create_output_list(sprd_data);
    if (ret != TDM_ERROR_NONE)
        goto failed_l;

    ret = tdm_sprd_display_create_event_list(sprd_data);
    if (ret != TDM_ERROR_NONE)
        goto failed_l;
    if (error)
        *error = TDM_ERROR_NONE;

    TDM_INFO("init success!");

    return (tdm_backend_data*)sprd_data;
failed_l:
    if (error)
        *error = ret;

    tdm_sprd_deinit(sprd_data);

    TDM_ERR("init failed!");
    return NULL;
}

tdm_backend_module tdm_backend_module_data =
{
    "sprd",
    "Samsung",
    TDM_BACKEND_ABI_VERSION,
    tdm_sprd_init,
    tdm_sprd_deinit
};

