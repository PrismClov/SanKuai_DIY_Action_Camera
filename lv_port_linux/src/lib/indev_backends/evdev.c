/**
 *
 * @file evdev.c
 *
 * The lib evdev driver
 *
 * Based on the original file from the repository
 *
 * - Move the driver to a separate file to avoid excessive conditional
 *   compilation
 *   Author: EDGEMTech Ltd, Erik Tagirov (erik.tagirov@edgemtech.ch)
 *
 * Copyright (c) 2025 EDGEMTech Ltd.
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "lvgl/lvgl.h"
#if LV_USE_EVDEV
#include "lvgl/src/core/lv_global.h"
#include "../backends.h"

/*********************
 *      DEFINES
 *********************/

#define EVDEV_SCAN_MAX 32

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void indev_deleted_cb(lv_event_t *e);
static void discovery_cb(lv_indev_t *indev, lv_evdev_type_t type, void *user_data);
static void set_mouse_cursor_icon(lv_indev_t *indev, lv_display_t *display);
static bool is_touchscreen_device(const char *path);
static char *find_touchscreen_device(void);
static lv_indev_t *init_pointer_evdev(lv_display_t *display);

/**********************
 *  STATIC VARIABLES
 **********************/

static char *backend_name = "EVDEV";

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/*
 * Initialize the evdev driver
 *
 * @param backend the backend descriptor
 */
int backend_init_evdev(backend_t *backend)
{
    LV_ASSERT_NULL(backend);
    backend->handle->indev = malloc(sizeof(indev_backend_t));
    LV_ASSERT_NULL(backend->handle->indev);

    backend->handle->indev->init_indev = init_pointer_evdev;

    backend->name = backend_name;
    backend->type = BACKEND_INDEV;
    return 0;
}


/**********************
 *   STATIC FUNCTIONS
 **********************/

/*
 * Remove cursor icon
 *
 * @description When the indev is deleted remove the mouse cursor icon
 * @note called by the LVGL evdev driver
 * @param e the deletion event
 */
static void indev_deleted_cb(lv_event_t *e)
{
    if(LV_GLOBAL_DEFAULT()->deinit_in_progress) return;
    lv_obj_t *cursor_obj = lv_event_get_user_data(e);
    lv_obj_delete(cursor_obj);
}


/*
 * Set cursor icon
 *
 * @description Once the input device is discovered set the mouse cursor icon
 * @note called by the LVGL evdev driver
 * @param indev the input device
 * @param type the type of the input device
 * @param user_data the user data
 */
static void discovery_cb(lv_indev_t *indev, lv_evdev_type_t type, void *user_data)
{
    LV_LOG_USER("new '%s' device discovered", type == LV_EVDEV_TYPE_REL ? "REL" :
                                              type == LV_EVDEV_TYPE_ABS ? "ABS" :
                                              type == LV_EVDEV_TYPE_KEY ? "KEY" :
                                              "unknown");

    lv_display_t *disp = user_data;
    lv_indev_set_display(indev, disp);

    /* 仅鼠标显示光标；触摸屏（ABS）隐藏光标 */
    if(type == LV_EVDEV_TYPE_REL) {
        set_mouse_cursor_icon(indev, disp);
    }
}

/*
 * Set cursor icon
 *
 * @description Enables a pointer (touchscreen/mouse) input device
 * @param display the display on which to create
 * @param indev the input device to set the cursor on
 */
static void set_mouse_cursor_icon(lv_indev_t *indev, lv_display_t *display)
{
    /* Set the cursor icon */
    LV_IMAGE_DECLARE(mouse_cursor_icon);
    lv_obj_t *cursor_obj = lv_image_create(lv_display_get_screen_active(display));
    lv_image_set_src(cursor_obj, &mouse_cursor_icon);
    lv_indev_set_cursor(indev, cursor_obj);

    /* delete the mouse cursor icon if the device is removed */
    lv_indev_add_event_cb(indev, indev_deleted_cb, LV_EVENT_DELETE, cursor_obj);

}

/*
 * Check if an input device is a touchscreen.
 *
 * Detection strategy (in order of reliability):
 *   1. INPUT_PROP_DIRECT explicitly marks a direct-touch device (touchscreen).
 *   2. Single-touch: EV_ABS with both ABS_X and ABS_Y.
 *   3. Multi-touch:  EV_ABS with both ABS_MT_POSITION_X and ABS_MT_POSITION_Y.
 *
 * We also require EV_KEY (most real touchscreens report BTN_TOUCH / key events),
 * which helps avoid mis-detecting accelerometers / joysticks that only have ABS axes.
 */
static bool is_touchscreen_device(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        LV_LOG_WARN("Cannot open %s for probing: %s", path, strerror(errno));
        return false;
    }

    unsigned long evbit[EV_MAX / (sizeof(unsigned long) * 8) + 1] = {0};
    unsigned long absbit[ABS_MAX / (sizeof(unsigned long) * 8) + 1] = {0};
    unsigned long propbit[INPUT_PROP_MAX / (sizeof(unsigned long) * 8) + 1] = {0};
    bool has_abs = false;
    bool has_key = false;
    bool has_direct = false;
    bool has_abs_xy = false;
    bool has_abs_mt_xy = false;

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0) {
        has_abs = evbit[EV_ABS / (sizeof(unsigned long) * 8)] & (1UL << (EV_ABS % (sizeof(unsigned long) * 8)));
        has_key = evbit[EV_KEY / (sizeof(unsigned long) * 8)] & (1UL << (EV_KEY % (sizeof(unsigned long) * 8)));
    }

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) >= 0) {
        bool has_x = absbit[ABS_X / (sizeof(unsigned long) * 8)] & (1UL << (ABS_X % (sizeof(unsigned long) * 8)));
        bool has_y = absbit[ABS_Y / (sizeof(unsigned long) * 8)] & (1UL << (ABS_Y % (sizeof(unsigned long) * 8)));
        has_abs_xy = has_x && has_y;

        bool has_mt_x = absbit[ABS_MT_POSITION_X / (sizeof(unsigned long) * 8)] & (1UL << (ABS_MT_POSITION_X % (sizeof(unsigned long) * 8)));
        bool has_mt_y = absbit[ABS_MT_POSITION_Y / (sizeof(unsigned long) * 8)] & (1UL << (ABS_MT_POSITION_Y % (sizeof(unsigned long) * 8)));
        has_abs_mt_xy = has_mt_x && has_mt_y;
    }

    if (ioctl(fd, EVIOCGPROP(sizeof(propbit)), propbit) >= 0) {
        has_direct = propbit[INPUT_PROP_DIRECT / (sizeof(unsigned long) * 8)] & (1UL << (INPUT_PROP_DIRECT % (sizeof(unsigned long) * 8)));
    }

    close(fd);

    bool is_touch = has_direct || has_abs_xy || has_abs_mt_xy;

    /* Require at least EV_ABS; EV_KEY further reduces false positives with non-touch ABS devices. */
    if (!has_abs) {
        is_touch = false;
    }

    if (is_touch) {
        LV_LOG_USER("Probe %s: DIRECT=%d ABS_XY=%d MT_XY=%d KEY=%d -> selected as touchscreen",
                    path, has_direct, has_abs_xy, has_abs_mt_xy, has_key);
    } else {
        LV_LOG_TRACE("Probe %s: DIRECT=%d ABS_XY=%d MT_XY=%d KEY=%d -> not a touchscreen",
                     path, has_direct, has_abs_xy, has_abs_mt_xy, has_key);
    }
    (void)has_key;  /* 仅用于日志，日志关闭时避免未使用警告 */

    return is_touch;
}

/*
 * Auto-detect touchscreen device by scanning /dev/input/event*
 *
 * @return allocated path string, caller must free(), or NULL if not found
 */
static char *find_touchscreen_device(void)
{
    /* Try /dev/input/event* in numeric order */
    for (int i = 0; i < EVDEV_SCAN_MAX; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (is_touchscreen_device(path)) {
            LV_LOG_USER("Found touchscreen device: %s", path);
            return strdup(path);
        }
    }

    /* Fallback: scan directory for any event* node (handles non-sequential naming) */
    DIR *dir = opendir("/dev/input");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "event", 5) != 0) continue;
            char path[320];
            snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
            if (is_touchscreen_device(path)) {
                LV_LOG_USER("Found touchscreen device: %s", path);
                closedir(dir);
                return strdup(path);
            }
        }
        closedir(dir);
    }

    return NULL;
}

/*
 * Detect if touchscreen axes need swapping.
 *
 * This checks whether the touchscreen hardware axes (ABS_X/ABS_Y) are
 * oriented differently from the display's PHYSICAL orientation.
 *
 * Important: This is a HARDWARE-LEVEL check based on physical resolution
 * (lv_display_get_original_horizontal_resolution), NOT the logical
 * resolution.  Software rotation (lv_display_set_rotation) is handled
 * separately by LVGL's indev layer and does NOT require evdev swap_axes.
 *
 * Swap is needed when:
 *   - physical display is portrait (phys_w < phys_h)
 *   - but touchscreen reports ABS_X range > ABS_Y range (landscape coords)
 *   or vice versa
 *
 * Environment variable override:
 *   LV_LINUX_EVDEV_SWAP_AXES=1  force enable swap
 *   LV_LINUX_EVDEV_SWAP_AXES=0  force disable swap
 */
static bool detect_needs_axis_swap(const char *path, lv_display_t *display)
{
    /* Environment variable override takes precedence */
    const char *env = getenv("LV_LINUX_EVDEV_SWAP_AXES");
    if (env != NULL) {
        bool force = (env[0] == '1' || env[0] == 'y' || env[0] == 'Y');
        LV_LOG_USER("Axis swap forced by env: %s -> %d", env, force);
        return force;
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;

    struct input_absinfo absinfo_x, absinfo_y;
    bool has_x = (ioctl(fd, EVIOCGABS(ABS_X), &absinfo_x) == 0);
    bool has_y = (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo_y) == 0);
    close(fd);

    if (!has_x || !has_y) return false;

    int32_t range_x = absinfo_x.maximum - absinfo_x.minimum;
    int32_t range_y = absinfo_y.maximum - absinfo_y.minimum;

    /* Use PHYSICAL resolution, not logical (rotation-aware) resolution.
     * lv_display_set_rotation() handles coordinate rotation in indev layer,
     * so evdev should always report coordinates aligned to physical display. */
    int32_t phys_w = lv_display_get_original_horizontal_resolution(display);
    int32_t phys_h = lv_display_get_original_vertical_resolution(display);

    bool phys_landscape = (phys_w > phys_h);
    bool touch_landscape = (range_x > range_y);

    bool need_swap = (phys_landscape != touch_landscape) && (range_x != range_y);

    LV_LOG_USER("Axis swap detect: touch range X=%d Y=%d, physical %dx%d, "
                "phys_landscape=%d touch_landscape=%d, swap=%d",
                range_x, range_y, phys_w, phys_h,
                phys_landscape, touch_landscape, need_swap);

    return need_swap;
}

/*
 * Initialize a mouse pointer device
 *
 * Enables a pointer (touchscreen/mouse) input device.
 * Priority:
 *   1. LV_LINUX_EVDEV_POINTER_DEVICE environment variable if set and valid
 *   2. Auto-detect first touchscreen device (ABS_X + ABS_Y)
 *   3. LVGL evdev automatic discovery
 *
 * @param display the LVGL display
 *
 * @return input device
 */
static lv_indev_t *init_pointer_evdev(lv_display_t *display)
{
    const char *input_device = getenv("LV_LINUX_EVDEV_POINTER_DEVICE");
    lv_indev_t *indev = NULL;

    if (input_device != NULL) {
        LV_LOG_USER("Using explicit input device: %s", input_device);
        indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, input_device);
        if (indev != NULL) {
            lv_indev_set_display(indev, display);
            /* Auto-detect axis swap for explicit device too */
            if (detect_needs_axis_swap(input_device, display)) {
                lv_evdev_set_swap_axes(indev, true);
                LV_LOG_USER("Axis swap enabled for %s", input_device);
            }
            return indev;
        }
        LV_LOG_WARN("Failed to open explicit device %s, trying auto-detect", input_device);
    }

    char *auto_dev = find_touchscreen_device();
    if (auto_dev != NULL) {
        indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, auto_dev);
        if (indev != NULL) {
            lv_indev_set_display(indev, display);
            /* Auto-detect axis swap: many touchscreens report portrait
             * coordinates even on landscape-mounted displays */
            if (detect_needs_axis_swap(auto_dev, display)) {
                lv_evdev_set_swap_axes(indev, true);
                LV_LOG_USER("Axis swap enabled for %s", auto_dev);
            }
        }
        free(auto_dev);
        if (indev != NULL) return indev;
    }

    LV_LOG_USER("Using evdev automatic discovery.");
    lv_evdev_discovery_start(discovery_cb, display);
    return NULL;
}
#endif /*#if LV_USE_EVDEV*/
