/**
 * @file lv_linux_drm.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_linux_drm.h"
#if LV_USE_LINUX_DRM && !LV_LINUX_DRM_USE_EGL

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "../../../stdlib/lv_sprintf.h"
#include "../../../draw/lv_draw_buf.h"

#if LV_USE_LINUX_DRM_GBM_BUFFERS

    #include <gbm.h>
    #include <linux/dma-buf.h>
    #include <sys/ioctl.h>

#endif

/*********************
 *      DEFINES
 *********************/
#if LV_COLOR_DEPTH == 32
    #define DRM_FOURCC DRM_FORMAT_XRGB8888
#elif LV_COLOR_DEPTH == 16
    #define DRM_FOURCC DRM_FORMAT_RGB565
#else
    #error LV_COLOR_DEPTH not supported
#endif

#define BUFFER_CNT 2

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    uint32_t handle;
    uint32_t pitch;
    uint32_t offset;
    unsigned long int size;
    uint8_t * map;
    uint32_t fb_handle;
} drm_buffer_t;

typedef struct {
    int fd;
    uint32_t conn_id, enc_id, crtc_id, plane_id, crtc_idx;
    uint32_t width, height;
    uint32_t mmWidth, mmHeight;
    uint32_t fourcc;
    drmModeModeInfo mode;
    uint32_t blob_id;
    drmModeCrtc * saved_crtc;
    drmModeAtomicReq * req;
    drmEventContext drm_event_ctx;
    drmModePlane * plane;
    drmModeCrtc * crtc;
    drmModeConnector * conn;
    uint32_t count_plane_props;
    uint32_t count_crtc_props;
    uint32_t count_conn_props;
    drmModePropertyPtr plane_props[128];
    drmModePropertyPtr crtc_props[128];
    drmModePropertyPtr conn_props[128];
    drm_buffer_t drm_bufs[BUFFER_CNT];
    drm_buffer_t * act_buf;
    /* FULL mode render buffers: LVGL renders to these (logical resolution),
     * then drm_flush copies/rotates to the dumb buffer (physical resolution).
     * This is required because the SW draw unit doesn't support matrix rotation,
     * so we cannot use DIRECT mode with lv_display_set_rotation(). */
    uint8_t * render_bufs[BUFFER_CNT];
    uint32_t render_buf_size;
    uint32_t dumb_buf_idx;  /* current dumb buffer index for display (alternates) */
#if LV_USE_LINUX_DRM_GBM_BUFFERS
    struct gbm_device * gbm_device;
#endif
} drm_dev_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static uint32_t get_plane_property_id(drm_dev_t * drm_dev, const char * name);
static uint32_t get_crtc_property_id(drm_dev_t * drm_dev, const char * name);
static uint32_t get_conn_property_id(drm_dev_t * drm_dev, const char * name);
static void page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec,
                              void * user_data);
static int drm_get_plane_props(drm_dev_t * drm_dev);
static int drm_get_crtc_props(drm_dev_t * drm_dev);
static int drm_get_conn_props(drm_dev_t * drm_dev);
static int drm_add_plane_property(drm_dev_t * drm_dev, const char * name, uint64_t value);
static int drm_add_crtc_property(drm_dev_t * drm_dev, const char * name, uint64_t value);
static int drm_add_conn_property(drm_dev_t * drm_dev, const char * name, uint64_t value);
static int find_plane(drm_dev_t * drm_dev, unsigned int fourcc, uint32_t * plane_id, uint32_t crtc_id,
                      uint32_t crtc_idx);
static int drm_find_connector(drm_dev_t * drm_dev, int64_t connector_id);
static int drm_open(const char * path);
static int drm_setup(drm_dev_t * drm_dev, const char * device_path, int64_t connector_id, unsigned int fourcc);

static uint32_t tick_get_cb(void);

#if !LV_USE_LINUX_DRM_GBM_BUFFERS
    static int drm_allocate_dumb(drm_dev_t * drm_dev, drm_buffer_t * buf);
#elif LV_USE_LINUX_DRM_GBM_BUFFERS
    static int create_gbm_buffer(drm_dev_t * drm_dev, drm_buffer_t * buf);
#endif

static int drm_setup_buffers(drm_dev_t * drm_dev);
static int drm_dmabuf_set_plane(drm_dev_t * drm_dev, drm_buffer_t * buf);
static void drm_flush_wait(lv_display_t * drm_dev);
static void drm_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map);
static void drm_rotate_copy_to_dumb(drm_dev_t * drm_dev, lv_display_rotation_t rot,
                                    const uint8_t * src, uint32_t src_stride,
                                    int32_t w, int32_t h,
                                    drm_buffer_t * dst);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/
#ifndef DIV_ROUND_UP
    #define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_display_t * lv_linux_drm_create(void)
{
    lv_display_t * disp;

    lv_tick_set_cb(tick_get_cb);

    drm_dev_t * drm_dev = lv_malloc_zeroed(sizeof(drm_dev_t));
    LV_ASSERT_MALLOC(drm_dev);
    if(drm_dev == NULL) return NULL;

    drm_dev->fd = -1;

    disp = lv_display_create(800, 480);
    if(disp == NULL) {
        lv_free(drm_dev);
        return NULL;
    }
    lv_display_set_driver_data(disp, drm_dev);
    lv_display_set_flush_wait_cb(disp, drm_flush_wait);
    lv_display_set_flush_cb(disp, drm_flush);

    return disp;
}

/* drm_dmabuf_set_active_buf was used in DIRECT mode to match the LVGL draw
 * buffer to a DRM dumb buffer. In FULL mode (current), drm_flush explicitly
 * selects the dumb buffer via dumb_buf_idx, so this callback is no longer
 * needed. The GBM DMA_BUF_SYNC path would need to be re-added if GBM
 * buffers are enabled with FULL mode in the future. */

void lv_linux_drm_set_file(lv_display_t * disp, const char * file, int64_t connector_id)
{
    int ret;

    drm_dev_t * drm_dev = lv_display_get_driver_data(disp);

    ret = drm_setup(drm_dev, file, connector_id, DRM_FOURCC);
    if(ret) {
        return;
    }

    int32_t hor_res = drm_dev->width;
    int32_t ver_res = drm_dev->height;

    ret = drm_setup_buffers(drm_dev);
    if(ret) {
        LV_LOG_ERROR("DRM buffer allocation failed");
        close(drm_dev->fd);
        drm_dev->fd = -1;
        return;
    }

    LV_LOG_INFO("DRM subsystem and buffer mapped successfully");

    int32_t width = drm_dev->mmWidth;

    /* Allocate render buffers for FULL mode.
     * LVGL renders to these buffers (at logical resolution), then drm_flush
     * copies/rotates the content to the DRM dumb buffer (physical resolution).
     *
     * Why FULL mode instead of DIRECT:
     * - DIRECT mode requires the draw buffer to BE the display buffer (dumb buffer).
     *   With lv_display_set_rotation(), the logical resolution differs from the
     *   physical dumb buffer dimensions, causing stride/layout mismatch.
     * - Matrix rotation (LV_DRAW_TRANSFORM_USE_MATRIX) is NOT supported by the
     *   SW draw unit — it only sets layer->matrix but never applies it during
     *   software blending, leading to out-of-bounds access and crashes.
     * - FULL mode decouples rendering from display: LVGL renders the full logical
     *   frame to a render buffer, and drm_flush performs the rotation copy to
     *   the physical dumb buffer. This works correctly with the SW draw unit.
     *
     * Buffer size: max(hor_res, ver_res) * max(hor_res, ver_res) * bpp/8 covers
     * both portrait and landscape orientations (same pixel count, different stride).
     */
    uint32_t max_dim = LV_MAX(hor_res, ver_res);
    uint32_t px_size = LV_COLOR_DEPTH / 8;
    /* Use max_dim^2 * px_size to guarantee enough space for any orientation */
    drm_dev->render_buf_size = max_dim * max_dim * px_size;

    int i;
    for(i = 0; i < BUFFER_CNT; i++) {
        /* Allocate with alignment padding */
        drm_dev->render_bufs[i] = lv_malloc(drm_dev->render_buf_size + LV_DRAW_BUF_ALIGN);
        LV_ASSERT_MALLOC(drm_dev->render_bufs[i]);
        if(drm_dev->render_bufs[i] == NULL) {
            LV_LOG_ERROR("Failed to allocate render buffer %d", i);
            close(drm_dev->fd);
            drm_dev->fd = -1;
            return;
        }
        lv_memzero(drm_dev->render_bufs[i], drm_dev->render_buf_size + LV_DRAW_BUF_ALIGN);
    }

    /* Align render buffer pointers */
    void * rb[BUFFER_CNT];
    lv_color_format_t cf = lv_display_get_color_format(disp);
    for(i = 0; i < BUFFER_CNT; i++) {
        rb[i] = lv_draw_buf_align(drm_dev->render_bufs[i], cf);
    }

    drm_dev->dumb_buf_idx = 0;

    /* Resolution must be set first because if the screen is smaller than the size passed
     * to lv_display_create then the buffers aren't big enough for LV_DISPLAY_RENDER_MODE_FULL.
     */
    lv_display_set_resolution(disp, hor_res, ver_res);
    lv_display_set_buffers(disp, rb[0], rb[1], drm_dev->render_buf_size,
                           LV_DISPLAY_RENDER_MODE_FULL);

    if(width) {
        lv_display_set_dpi(disp, DIV_ROUND_UP(hor_res * 25400, width * 1000));
    }

    LV_LOG_INFO("Resolution is set to %" LV_PRId32 "x%" LV_PRId32 " at %" LV_PRId32 "dpi (FULL mode)",
                hor_res, ver_res, lv_display_get_dpi(disp));
}

void lv_linux_drm_set_mode_cb(lv_display_t * disp, lv_linux_drm_select_mode_cb_t callback)
{
    LV_UNUSED(disp);
    LV_UNUSED(callback);
    LV_LOG_WARN("DRM without EGL support doesn't currently support setting a mode selection callback");
}

bool lv_linux_drm_is_ready(lv_display_t * disp)
{
    if(disp == NULL) return false;
    drm_dev_t * drm_dev = lv_display_get_driver_data(disp);
    if(drm_dev == NULL) return false;
    return drm_dev->fd >= 0;
}
/**********************
 *   STATIC FUNCTIONS
 **********************/

static uint32_t get_plane_property_id(drm_dev_t * drm_dev, const char * name)
{
    uint32_t i;

    LV_LOG_TRACE("Find plane property: %s", name);

    for(i = 0; i < drm_dev->count_plane_props; ++i)
        if(!lv_strcmp(drm_dev->plane_props[i]->name, name))
            return drm_dev->plane_props[i]->prop_id;

    LV_LOG_TRACE("Unknown plane property: %s", name);

    return 0;
}

static uint32_t get_crtc_property_id(drm_dev_t * drm_dev, const char * name)
{
    uint32_t i;

    LV_LOG_TRACE("Find crtc property: %s", name);

    for(i = 0; i < drm_dev->count_crtc_props; ++i)
        if(!lv_strcmp(drm_dev->crtc_props[i]->name, name))
            return drm_dev->crtc_props[i]->prop_id;

    LV_LOG_TRACE("Unknown crtc property: %s", name);

    return 0;
}

static uint32_t get_conn_property_id(drm_dev_t * drm_dev, const char * name)
{
    uint32_t i;

    LV_LOG_TRACE("Find conn property: %s", name);

    for(i = 0; i < drm_dev->count_conn_props; ++i)
        if(!lv_strcmp(drm_dev->conn_props[i]->name, name))
            return drm_dev->conn_props[i]->prop_id;

    LV_LOG_TRACE("Unknown conn property: %s", name);

    return 0;
}

static void page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec,
                              void * user_data)
{
    LV_UNUSED(fd);
    LV_UNUSED(sequence);
    LV_UNUSED(tv_sec);
    LV_UNUSED(tv_usec);
    LV_LOG_TRACE("flip");
    drm_dev_t * drm_dev = user_data;
    if(drm_dev->req) {
        drmModeAtomicFree(drm_dev->req);
        drm_dev->req = NULL;
    }
}

static int drm_get_plane_props(drm_dev_t * drm_dev)
{
    uint32_t i;

    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drm_dev->fd, drm_dev->plane_id,
                                                                  DRM_MODE_OBJECT_PLANE);
    if(!props) {
        LV_LOG_ERROR("drmModeObjectGetProperties failed");
        return -1;
    }
    LV_LOG_TRACE("Found %u plane props", props->count_props);
    drm_dev->count_plane_props = props->count_props;
    for(i = 0; i < props->count_props; i++) {
        drm_dev->plane_props[i] = drmModeGetProperty(drm_dev->fd, props->props[i]);
        LV_LOG_TRACE("Added plane prop %u:%s", drm_dev->plane_props[i]->prop_id, drm_dev->plane_props[i]->name);
    }
    drmModeFreeObjectProperties(props);

    return 0;
}

static int drm_get_crtc_props(drm_dev_t * drm_dev)
{
    uint32_t i;

    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drm_dev->fd, drm_dev->crtc_id,
                                                                  DRM_MODE_OBJECT_CRTC);
    if(!props) {
        LV_LOG_ERROR("drmModeObjectGetProperties failed");
        return -1;
    }
    LV_LOG_TRACE("Found %u crtc props", props->count_props);
    drm_dev->count_crtc_props = props->count_props;
    for(i = 0; i < props->count_props; i++) {
        drm_dev->crtc_props[i] = drmModeGetProperty(drm_dev->fd, props->props[i]);
        LV_LOG_TRACE("Added crtc prop %u:%s", drm_dev->crtc_props[i]->prop_id, drm_dev->crtc_props[i]->name);
    }
    drmModeFreeObjectProperties(props);

    return 0;
}

static int drm_get_conn_props(drm_dev_t * drm_dev)
{
    uint32_t i;

    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drm_dev->fd, drm_dev->conn_id,
                                                                  DRM_MODE_OBJECT_CONNECTOR);
    if(!props) {
        LV_LOG_ERROR("drmModeObjectGetProperties failed");
        return -1;
    }
    LV_LOG_TRACE("Found %u connector props", props->count_props);
    drm_dev->count_conn_props = props->count_props;
    for(i = 0; i < props->count_props; i++) {
        drm_dev->conn_props[i] = drmModeGetProperty(drm_dev->fd, props->props[i]);
        LV_LOG_TRACE("Added connector prop %u:%s", drm_dev->conn_props[i]->prop_id, drm_dev->conn_props[i]->name);
    }
    drmModeFreeObjectProperties(props);

    return 0;
}

static int drm_add_plane_property(drm_dev_t * drm_dev, const char * name, uint64_t value)
{
    int ret;
    uint32_t prop_id = get_plane_property_id(drm_dev, name);

    if(!prop_id) {
        LV_LOG_ERROR("Couldn't find plane prop %s", name);
        return -1;
    }

    ret = drmModeAtomicAddProperty(drm_dev->req, drm_dev->plane_id, get_plane_property_id(drm_dev, name), value);
    if(ret < 0) {
        LV_LOG_ERROR("drmModeAtomicAddProperty (%s:%" PRIu64 ") failed: %d", name, value, ret);
        return ret;
    }

    return 0;
}

static int drm_add_crtc_property(drm_dev_t * drm_dev, const char * name, uint64_t value)
{
    int ret;
    uint32_t prop_id = get_crtc_property_id(drm_dev, name);

    if(!prop_id) {
        LV_LOG_ERROR("Couldn't find crtc prop %s", name);
        return -1;
    }

    ret = drmModeAtomicAddProperty(drm_dev->req, drm_dev->crtc_id, get_crtc_property_id(drm_dev, name), value);
    if(ret < 0) {
        LV_LOG_ERROR("drmModeAtomicAddProperty (%s:%" PRIu64 ") failed: %d", name, value, ret);
        return ret;
    }

    return 0;
}

static int drm_add_conn_property(drm_dev_t * drm_dev, const char * name, uint64_t value)
{
    int ret;
    uint32_t prop_id = get_conn_property_id(drm_dev, name);

    if(!prop_id) {
        LV_LOG_ERROR("Couldn't find conn prop %s", name);
        return -1;
    }

    ret = drmModeAtomicAddProperty(drm_dev->req, drm_dev->conn_id, get_conn_property_id(drm_dev, name), value);
    if(ret < 0) {
        LV_LOG_ERROR("drmModeAtomicAddProperty (%s:%" PRIu64 ") failed: %d", name, value, ret);
        return ret;
    }

    return 0;
}

static int drm_dmabuf_set_plane(drm_dev_t * drm_dev, drm_buffer_t * buf)
{
    int ret;
    static int first = 1;
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;

#if LV_USE_LINUX_DRM_GBM_BUFFERS

    struct dma_buf_sync sync_req;

    sync_req.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    if(ioctl(buf->handle, DMA_BUF_IOCTL_SYNC, &sync_req) != 0) {
        LV_LOG_ERROR("Failed to end DMA-BUF R/W SYNC");
    }

#endif

    drm_dev->req = drmModeAtomicAlloc();

    /* On first Atomic commit, do a modeset */
    if(first) {
        drm_add_conn_property(drm_dev, "CRTC_ID", drm_dev->crtc_id);

        drm_add_crtc_property(drm_dev, "MODE_ID", drm_dev->blob_id);
        drm_add_crtc_property(drm_dev, "ACTIVE", 1);

        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

        first = 0;
    }

    drm_add_plane_property(drm_dev, "FB_ID", buf->fb_handle);
    drm_add_plane_property(drm_dev, "CRTC_ID", drm_dev->crtc_id);
    drm_add_plane_property(drm_dev, "SRC_X", 0);
    drm_add_plane_property(drm_dev, "SRC_Y", 0);
    drm_add_plane_property(drm_dev, "SRC_W", drm_dev->width << 16);
    drm_add_plane_property(drm_dev, "SRC_H", drm_dev->height << 16);
    drm_add_plane_property(drm_dev, "CRTC_X", 0);
    drm_add_plane_property(drm_dev, "CRTC_Y", 0);
    drm_add_plane_property(drm_dev, "CRTC_W", drm_dev->width);
    drm_add_plane_property(drm_dev, "CRTC_H", drm_dev->height);

    ret = drmModeAtomicCommit(drm_dev->fd, drm_dev->req, flags, drm_dev);
    if(ret) {
        LV_LOG_ERROR("drmModeAtomicCommit failed: %s (%d)", strerror(errno), errno);
        drmModeAtomicFree(drm_dev->req);
        drm_dev->req = NULL;  /* 必须置 NULL，否则 drm_flush_wait 中 while(drm_dev->req) 死等 */
        return ret;
    }

    return 0;
}

static int find_plane(drm_dev_t * drm_dev, unsigned int fourcc, uint32_t * plane_id, uint32_t crtc_id,
                      uint32_t crtc_idx)
{
    LV_UNUSED(crtc_id);
    drmModePlaneResPtr planes;
    drmModePlanePtr plane;
    unsigned int i;
    unsigned int j;
    int ret = 0;
    unsigned int format = fourcc;

    planes = drmModeGetPlaneResources(drm_dev->fd);
    if(!planes) {
        LV_LOG_ERROR("drmModeGetPlaneResources failed");
        return -1;
    }

    LV_LOG_TRACE("drm: found planes %u", planes->count_planes);

    for(i = 0; i < planes->count_planes; ++i) {
        plane = drmModeGetPlane(drm_dev->fd, planes->planes[i]);
        if(!plane) {
            LV_LOG_ERROR("drmModeGetPlane failed: %s", strerror(errno));
            break;
        }

        if(!(plane->possible_crtcs & (1 << crtc_idx))) {
            drmModeFreePlane(plane);
            continue;
        }

        for(j = 0; j < plane->count_formats; ++j) {
            if(plane->formats[j] == format)
                break;
        }

        if(j == plane->count_formats) {
            drmModeFreePlane(plane);
            continue;
        }

        *plane_id = plane->plane_id;
        drmModeFreePlane(plane);

        LV_LOG_TRACE("found plane %d", *plane_id);

        break;
    }

    if(i == planes->count_planes)
        ret = -1;

    drmModeFreePlaneResources(planes);

    return ret;
}

static int drm_find_connector(drm_dev_t * drm_dev, int64_t connector_id)
{
    drmModeConnector * conn = NULL;
    drmModeEncoder * enc = NULL;
    drmModeRes * res;
    int i;

    if((res = drmModeGetResources(drm_dev->fd)) == NULL) {
        LV_LOG_ERROR("drmModeGetResources() failed");
        return -1;
    }

    if(res->count_crtcs <= 0) {
        LV_LOG_ERROR("no Crtcs");
        goto free_res;
    }

    /* find all available connectors */
    for(i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(drm_dev->fd, res->connectors[i]);
        if(!conn)
            continue;

        if(connector_id >= 0 && conn->connector_id != connector_id) {
            drmModeFreeConnector(conn);
            continue;
        }

        if(conn->connection == DRM_MODE_CONNECTED) {
            LV_LOG_TRACE("drm: connector %d: connected", conn->connector_id);
        }
        else if(conn->connection == DRM_MODE_DISCONNECTED) {
            LV_LOG_TRACE("drm: connector %d: disconnected", conn->connector_id);
        }
        else if(conn->connection == DRM_MODE_UNKNOWNCONNECTION) {
            LV_LOG_TRACE("drm: connector %d: unknownconnection", conn->connector_id);
        }
        else {
            LV_LOG_TRACE("drm: connector %d: unknown", conn->connector_id);
        }

        if(conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
            break;

        drmModeFreeConnector(conn);
        conn = NULL;
    };

    if(!conn) {
        LV_LOG_ERROR("suitable connector not found");
        goto free_res;
    }

    drm_dev->conn_id = conn->connector_id;
    LV_LOG_TRACE("conn_id: %d", drm_dev->conn_id);
    drm_dev->mmWidth = conn->mmWidth;
    drm_dev->mmHeight = conn->mmHeight;

    lv_memcpy(&drm_dev->mode, &conn->modes[0], sizeof(drmModeModeInfo));

    if(drmModeCreatePropertyBlob(drm_dev->fd, &drm_dev->mode, sizeof(drm_dev->mode),
                                 &drm_dev->blob_id)) {
        LV_LOG_ERROR("error creating mode blob");
        goto free_res;
    }

    drm_dev->width = conn->modes[0].hdisplay;
    drm_dev->height = conn->modes[0].vdisplay;

    for(i = 0 ; i < res->count_encoders; i++) {
        enc = drmModeGetEncoder(drm_dev->fd, res->encoders[i]);
        if(!enc)
            continue;

        LV_LOG_TRACE("enc%d enc_id %d conn enc_id %d", i, enc->encoder_id, conn->encoder_id);

        if(enc->encoder_id == conn->encoder_id)
            break;

        drmModeFreeEncoder(enc);
        enc = NULL;
    }

    if(enc) {
        drm_dev->enc_id = enc->encoder_id;
        LV_LOG_TRACE("enc_id: %d", drm_dev->enc_id);
        drm_dev->crtc_id = enc->crtc_id;
        LV_LOG_TRACE("crtc_id: %d", drm_dev->crtc_id);
        drmModeFreeEncoder(enc);
    }
    else {
        /* Encoder hasn't been associated yet, look it up */
        for(i = 0; i < conn->count_encoders; i++) {
            int crtc, crtc_id = -1;

            enc = drmModeGetEncoder(drm_dev->fd, conn->encoders[i]);
            if(!enc)
                continue;

            for(crtc = 0 ; crtc < res->count_crtcs; crtc++) {
                uint32_t crtc_mask = 1 << crtc;

                crtc_id = res->crtcs[crtc];

                LV_LOG_TRACE("enc_id %d crtc%d id %d mask %x possible %x", enc->encoder_id, crtc, crtc_id, crtc_mask,
                             enc->possible_crtcs);

                if(enc->possible_crtcs & crtc_mask)
                    break;
            }

            if(crtc_id > 0) {
                drm_dev->enc_id = enc->encoder_id;
                LV_LOG_TRACE("enc_id: %d", drm_dev->enc_id);
                drm_dev->crtc_id = crtc_id;
                LV_LOG_TRACE("crtc_id: %d", drm_dev->crtc_id);
                break;
            }

            drmModeFreeEncoder(enc);
            enc = NULL;
        }

        if(!enc) {
            LV_LOG_ERROR("suitable encoder not found");
            goto free_res;
        }

        drmModeFreeEncoder(enc);
    }

    drm_dev->crtc_idx = UINT32_MAX;

    for(i = 0; i < res->count_crtcs; ++i) {
        if(drm_dev->crtc_id == res->crtcs[i]) {
            drm_dev->crtc_idx = i;
            break;
        }
    }

    if(drm_dev->crtc_idx == UINT32_MAX) {
        LV_LOG_ERROR("drm: CRTC not found");
        goto free_res;
    }

    LV_LOG_TRACE("crtc_idx: %d", drm_dev->crtc_idx);

    return 0;

free_res:
    drmModeFreeResources(res);

    return -1;
}

static int drm_open(const char * path)
{
    int fd, flags;
    uint64_t has_dumb;
    int ret;

    fd = open(path, O_RDWR);
    if(fd < 0) {
        LV_LOG_ERROR("cannot open \"%s\"", path);
        return -1;
    }

    /* set FD_CLOEXEC flag */
    if((flags = fcntl(fd, F_GETFD)) < 0 ||
       fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        LV_LOG_ERROR("fcntl FD_CLOEXEC failed");
        goto err;
    }

    /* check capability */
    ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb);
    if(ret < 0 || has_dumb == 0) {
        LV_LOG_ERROR("drmGetCap DRM_CAP_DUMB_BUFFER failed or \"%s\" doesn't have dumb "
                     "buffer", path);
        goto err;
    }

    return fd;
err:
    close(fd);
    return -1;
}

static int drm_setup(drm_dev_t * drm_dev, const char * device_path, int64_t connector_id, unsigned int fourcc)
{
    int ret;

    drm_dev->fd = drm_open(device_path);
    if(drm_dev->fd < 0)
        return -1;

    ret = drmSetClientCap(drm_dev->fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if(ret) {
        LV_LOG_ERROR("No atomic modesetting support: %s", strerror(errno));
        goto err;
    }

    ret = drm_find_connector(drm_dev, connector_id);
    if(ret) {
        LV_LOG_ERROR("available drm devices not found");
        goto err;
    }

    ret = find_plane(drm_dev, fourcc, &drm_dev->plane_id, drm_dev->crtc_id, drm_dev->crtc_idx);
    if(ret) {
        LV_LOG_ERROR("Cannot find plane");
        goto err;
    }

    drm_dev->plane = drmModeGetPlane(drm_dev->fd, drm_dev->plane_id);
    if(!drm_dev->plane) {
        LV_LOG_ERROR("Cannot get plane");
        goto err;
    }

    drm_dev->crtc = drmModeGetCrtc(drm_dev->fd, drm_dev->crtc_id);
    if(!drm_dev->crtc) {
        LV_LOG_ERROR("Cannot get crtc");
        goto err;
    }

    drm_dev->conn = drmModeGetConnector(drm_dev->fd, drm_dev->conn_id);
    if(!drm_dev->conn) {
        LV_LOG_ERROR("Cannot get connector");
        goto err;
    }

    ret = drm_get_plane_props(drm_dev);
    if(ret) {
        LV_LOG_ERROR("Cannot get plane props");
        goto err;
    }

    ret = drm_get_crtc_props(drm_dev);
    if(ret) {
        LV_LOG_ERROR("Cannot get crtc props");
        goto err;
    }

    ret = drm_get_conn_props(drm_dev);
    if(ret) {
        LV_LOG_ERROR("Cannot get connector props");
        goto err;
    }

    drm_dev->drm_event_ctx.version = DRM_EVENT_CONTEXT_VERSION;
    drm_dev->drm_event_ctx.page_flip_handler = page_flip_handler;
    drm_dev->fourcc = fourcc;

    LV_LOG_INFO("drm: Found plane_id: %u connector_id: %d crtc_id: %d",
                drm_dev->plane_id, drm_dev->conn_id, drm_dev->crtc_id);

    LV_LOG_INFO("drm: %dx%d (%dmm X% dmm) pixel format %c%c%c%c",
                drm_dev->width, drm_dev->height, drm_dev->mmWidth, drm_dev->mmHeight,
                (fourcc >> 0) & 0xff, (fourcc >> 8) & 0xff, (fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff);


#if LV_USE_LINUX_DRM_GBM_BUFFERS

    /* Create GBM device and buffer */
    drm_dev->gbm_device = gbm_create_device(drm_dev->fd);

    if(drm_dev->gbm_device == NULL) {
        LV_LOG_ERROR("Failed to create GBM device");
        goto err;
    }

    LV_LOG_INFO("GBM device backend: %s", gbm_device_get_backend_name(drm_dev->gbm_device));
#endif

    return 0;

err:
    close(drm_dev->fd);
    return -1;
}

#if !LV_USE_LINUX_DRM_GBM_BUFFERS
static int drm_allocate_dumb(drm_dev_t * drm_dev, drm_buffer_t * buf)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    int ret;

    /* create dumb buffer */
    lv_memzero(&creq, sizeof(creq));
    creq.width = drm_dev->width;
    creq.height = drm_dev->height;
    creq.bpp = LV_COLOR_DEPTH;
    ret = drmIoctl(drm_dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if(ret < 0) {
        LV_LOG_ERROR("DRM_IOCTL_MODE_CREATE_DUMB fail");
        return -1;
    }

    buf->handle = creq.handle;
    buf->pitch = creq.pitch;
    buf->size = creq.size;

    /* prepare buffer for memory mapping */
    lv_memzero(&mreq, sizeof(mreq));
    mreq.handle = creq.handle;
    ret = drmIoctl(drm_dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if(ret) {
        LV_LOG_ERROR("DRM_IOCTL_MODE_MAP_DUMB fail");
        return -1;
    }

    buf->offset = mreq.offset;
    LV_LOG_INFO("size %lu pitch %u offset %u", buf->size, buf->pitch, buf->offset);

    /* perform actual memory mapping */
    buf->map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_dev->fd, mreq.offset);
    if(buf->map == MAP_FAILED) {
        LV_LOG_ERROR("mmap fail");
        return -1;
    }

    /* clear the framebuffer to 0 (= full transparency in ARGB8888) */
    lv_memzero(buf->map, creq.size);

    /* create framebuffer object for the dumb-buffer */
    handles[0] = creq.handle;
    pitches[0] = creq.pitch;
    offsets[0] = 0;
    ret = drmModeAddFB2(drm_dev->fd, drm_dev->width, drm_dev->height, drm_dev->fourcc,
                        handles, pitches, offsets, &buf->fb_handle, 0);
    if(ret) {
        LV_LOG_ERROR("drmModeAddFB fail");
        return -1;
    }

    return 0;
}
#endif /*!LV_USE_LINUX_DRM_GBM_BUFFERS*/

#if LV_USE_LINUX_DRM_GBM_BUFFERS

static int create_gbm_buffer(drm_dev_t * drm_dev, drm_buffer_t * buf)
{

    struct gbm_bo * gbm_bo;
    int prime_fd;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint32_t n_planes;
    int res;

    /* gbm_bo_format does not define anything other than ARGB8888 or XRGB8888 */
    if(LV_COLOR_DEPTH != 32) {
        LV_LOG_ERROR("Unsupported color format");
        return -1;
    }

    /* Create a linear GBM buffer object - best practice when modifiers are not used */
    if(!(gbm_bo = gbm_bo_create(drm_dev->gbm_device,
                                drm_dev->width, drm_dev->height, GBM_BO_FORMAT_XRGB8888,
                                GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR))) {

        LV_LOG_ERROR("Unable to create gbm buffer object");
        return -1;
    }

    /* Currently only, one plane per dma-buf/prime fd is supported - but some GPUs feature
     * several planes (multiple fds or sometimes a single fd for multiple planes).
     * current implementation is kept simple for now */

    n_planes = gbm_bo_get_plane_count(gbm_bo);

    if(n_planes != 1) {
        LV_LOG_ERROR("The current implementation only supports a single plane per fd");
        return -1;
    }

    uint32_t h = gbm_bo_get_height(gbm_bo);
    pitches[0] = buf->pitch = gbm_bo_get_stride_for_plane(gbm_bo, 0);
    offsets[0] = buf->offset = gbm_bo_get_offset(gbm_bo, 0);
    buf->size = h * buf->pitch;

    LV_LOG_INFO("Created GBM BO of size: %lu pitch: %u offset: %u",
                buf->size, buf->pitch, buf->offset);

    prime_fd = gbm_bo_get_fd_for_plane(gbm_bo, 0);

    if(prime_fd < 0) {
        LV_LOG_ERROR("Failed to get prime fd for plane 0");
        return -1;

    }

    buf->map = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, prime_fd, 0);

    if(buf->map == MAP_FAILED) {
        LV_LOG_ERROR("Failed to mmap dma-buf fd.");
        return -1;
    }

    /* Used to perform DMA_BUF_SYNC ioctl calls during the rendering cycle */
    buf->handle = prime_fd;

    /* Convert prime fd to a libdrm buffer handle */
    drmPrimeFDToHandle(drm_dev->fd, buf->handle, &handles[0]);

    /* create libdrm framebuffer */
    res = drmModeAddFB2(drm_dev->fd, drm_dev->width, drm_dev->height, drm_dev->fourcc,
                        handles, pitches, offsets, &buf->fb_handle, 0);

    if(res) {
        LV_LOG_ERROR("drmModeAddFB2 failed");
        return -1;
    }

    if(drmCloseBufferHandle(drm_dev->fd, handles[0]) != 0) {
        LV_LOG_ERROR("drmCloseBufferHandle failed");
        return -1;
    }

    return 0;

}

#endif /* LV_USE_LINUX_DRM_GBM_BUFFERS */

static int drm_setup_buffers(drm_dev_t * drm_dev)
{
    int ret;

#if LV_USE_LINUX_DRM_GBM_BUFFERS

    ret = create_gbm_buffer(drm_dev, &drm_dev->drm_bufs[0]);
    if(ret < 0) {
        return ret;
    }

    ret = create_gbm_buffer(drm_dev, &drm_dev->drm_bufs[1]);
    if(ret < 0) {
        return ret;
    }

#else

    /* Use dumb buffers */
    ret = drm_allocate_dumb(drm_dev, &drm_dev->drm_bufs[0]);
    if(ret)
        return ret;

    ret = drm_allocate_dumb(drm_dev, &drm_dev->drm_bufs[1]);
    if(ret)
        return ret;

#endif

    return 0;
}

static void drm_flush_wait(lv_display_t * disp)
{
    drm_dev_t * drm_dev = lv_display_get_driver_data(disp);

    if(drm_dev->req == NULL) return;

    struct pollfd pfd;
    pfd.fd = drm_dev->fd;
    pfd.events = POLLIN;

    /* 3 秒超时保护：防止任何原因（commit 失败未清 req、page flip 事件丢失等）
     * 导致 drm_flush_wait 死等。超时后强制清 req 并继续，避免整个应用卡死。 */
    int timeout_ms = 3000;
    int waited_ms = 0;
    while(drm_dev->req) {
        int ret;
        int poll_timeout = timeout_ms - waited_ms;
        if(poll_timeout <= 0) poll_timeout = 1;
        do {
            ret = poll(&pfd, 1, poll_timeout);
        } while(ret == -1 && errno == EINTR);

        if(ret > 0) {
            drmHandleEvent(drm_dev->fd, &drm_dev->drm_event_ctx);
            waited_ms = 0;  /* 收到事件后重置等待计时 */
        }
        else if(ret == 0) {
            waited_ms += poll_timeout;
            if(waited_ms >= timeout_ms) {
                LV_LOG_ERROR("drm_flush_wait timeout: no page flip event for %d ms, forcing continue", timeout_ms);
                drmModeAtomicFree(drm_dev->req);
                drm_dev->req = NULL;
                return;
            }
        }
        else {
            LV_LOG_ERROR("poll failed: %s", strerror(errno));
            drmModeAtomicFree(drm_dev->req);
            drm_dev->req = NULL;
            return;
        }
    }
}

static void drm_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    if(!lv_display_flush_is_last(disp)) return;

    drm_dev_t * drm_dev = lv_display_get_driver_data(disp);

    /* In FULL mode, px_map points to the render buffer (logical resolution).
     * Copy/rotate it to the active DRM dumb buffer (physical resolution),
     * then trigger a page flip to display it. */
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);

    /* Get source stride from the active draw buffer */
    lv_draw_buf_t * buf_act = lv_display_get_buf_active(disp);
    uint32_t src_stride = buf_act ? buf_act->header.stride : (uint32_t)(w * (LV_COLOR_DEPTH / 8));

    /* Select the destination dumb buffer (alternate for double-buffering) */
    drm_buffer_t * dst = &drm_dev->drm_bufs[drm_dev->dumb_buf_idx];

    lv_display_rotation_t rot = lv_display_get_rotation(disp);
    drm_rotate_copy_to_dumb(drm_dev, rot, px_map, src_stride, w, h, dst);

    /* Flip the dumb buffer to the display */
    if(drm_dmabuf_set_plane(drm_dev, dst)) {
        LV_LOG_ERROR("Flush fail");
        return;
    }

    /* Alternate dumb buffer for next frame */
    drm_dev->dumb_buf_idx = (drm_dev->dumb_buf_idx + 1) % BUFFER_CNT;
}

/**
 * Copy/rotate pixels from the LVGL render buffer (logical resolution) to
 * the DRM dumb buffer (physical resolution).
 *
 * Coordinate mapping (matching LVGL's lv_display_rotate_area / matrix):
 *   ROTATION_0:   direct copy (logical == physical layout)
 *   ROTATION_90:  logical (lx,ly) → physical (ly, phys_h-1-lx)
 *   ROTATION_180: logical (lx,ly) → physical (w-1-lx, h-1-ly)
 *   ROTATION_270: logical (lx,ly) → physical (phys_w-1-ly, lx)
 *
 * where phys_w/phys_h are the physical (dumb buffer) dimensions.
 *
 * 旋转切换时清空目标 dumb buffer：旋转刚发生那一帧，dumb buffer 里残留
 * 旧方向的内容（其它行/列位置），新内容（旋转后）只覆盖 w*h 个像素，
 * 物理 buffer 边缘可能仍是旧方向残影，导致花屏。每次 rot 变化时把
 * 整个目标 buffer 写成黑色（BG=0x0000），确保新方向的边界清晰。
 */
static void drm_rotate_copy_to_dumb(drm_dev_t * drm_dev, lv_display_rotation_t rot,
                                    const uint8_t * src, uint32_t src_stride,
                                    int32_t w, int32_t h,
                                    drm_buffer_t * dst)
{
    uint8_t * dst_map = dst->map;
    uint32_t dst_stride = dst->pitch;
    int32_t phys_w = (int32_t)drm_dev->width;
    int32_t phys_h = (int32_t)drm_dev->height;
    uint32_t px_size = LV_COLOR_DEPTH / 8;

    /* 旋转切换检测：第一次进入 (s_last_rot == -1) 或方向发生变化时
     * 把两块 dumb buffer 全部清成黑色（0x0000），避免旧方向残影。
     * 由于 drm_flush 每帧交替 dumb_buf_idx，只清当前目标 buffer 会导致
     * 另一块 buffer 在下一帧直接显示旧方向内容，出现单帧花屏。
     * 后续帧 s_last_rot 已是当前 rot，跳过 memset，正常旋转复制。 */
    static lv_display_rotation_t s_last_rot = (lv_display_rotation_t)-1;
    if(s_last_rot != rot) {
        lv_display_rotation_t prev = s_last_rot;
        size_t total = (size_t)dst_stride * (size_t)phys_h;
        int i;
        for(i = 0; i < BUFFER_CNT; i++) {
            lv_memzero(drm_dev->drm_bufs[i].map, total);
        }
        s_last_rot = rot;
        LV_LOG_USER("drm_rotate_copy_to_dumb: rotation changed %d -> %d, cleared both dumb buffers (%u bytes x2)",
                    (int)prev, (int)rot, (unsigned)total);
    }

    if(rot == LV_DISPLAY_ROTATION_0) {
        /* Direct copy: same layout, row by row */
        int32_t y;
        for(y = 0; y < h; y++) {
            lv_memcpy(dst_map + y * dst_stride,
                      src + y * src_stride,
                      (uint32_t)w * px_size);
        }
    }
    else if(rot == LV_DISPLAY_ROTATION_90) {
        /* 90° CCW: logical (lx,ly) → physical (ly, phys_h-1-lx)
         * Source is w×h (logical), dest is phys_w×phys_h (physical),
         * where w=phys_h, h=phys_w (logical dims are swapped). */
        int32_t lx, ly;
        for(ly = 0; ly < h; ly++) {
            const uint8_t * src_row = src + ly * src_stride;
            int32_t px = ly;
            for(lx = 0; lx < w; lx++) {
                int32_t py = phys_h - 1 - lx;
                uint8_t * dst_px = dst_map + (uint32_t)py * dst_stride + (uint32_t)px * px_size;
                lv_memcpy(dst_px, src_row + (uint32_t)lx * px_size, px_size);
            }
        }
    }
    else if(rot == LV_DISPLAY_ROTATION_180) {
        /* 180°: logical (lx,ly) → physical (w-1-lx, h-1-ly) */
        int32_t ly;
        for(ly = 0; ly < h; ly++) {
            const uint8_t * src_row = src + ly * src_stride;
            int32_t py = h - 1 - ly;
            uint8_t * dst_row = dst_map + (uint32_t)py * dst_stride;
            int32_t lx;
            for(lx = 0; lx < w; lx++) {
                int32_t px = w - 1 - lx;
                lv_memcpy(dst_row + (uint32_t)px * px_size,
                          src_row + (uint32_t)lx * px_size, px_size);
            }
        }
    }
    else if(rot == LV_DISPLAY_ROTATION_270) {
        /* 270° CCW (90° CW): logical (lx,ly) → physical (phys_w-1-ly, lx) */
        int32_t lx, ly;
        for(ly = 0; ly < h; ly++) {
            const uint8_t * src_row = src + ly * src_stride;
            int32_t px = phys_w - 1 - ly;
            for(lx = 0; lx < w; lx++) {
                int32_t py = lx;
                uint8_t * dst_px = dst_map + (uint32_t)py * dst_stride + (uint32_t)px * px_size;
                lv_memcpy(dst_px, src_row + (uint32_t)lx * px_size, px_size);
            }
        }
    }
}

static uint32_t tick_get_cb(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t time_ms = t.tv_sec * 1000 + (t.tv_nsec / 1000000);
    return time_ms;
}

#endif /*LV_USE_LINUX_DRM && !LV_LINUX_DRM_USE_EGL*/
