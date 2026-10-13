/**
 * @file main.c
 *
 * LVGL example application using LVGL on Unix-like
 * operating systems - It is used to test if the build works
 * with dynamic linking and finding the library on the system
 *
 * NOTE: before compiling this program be sure
 * to first compile & install LVGL on your system
 * detailed instructions are available in the README.md
 *
 * Author: EDGEMTech Ltd, erik.tagirov@edgemtech.ch
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include <lvgl.h>
#include <lvgl/demos/lv_demos.h>
#include <lvgl/driver_backends.h>
#include <lvgl/simulator_settings.h>

extern simulator_settings_t settings;

int main(int argc, char **argv)
{

    /* Initialize LVGL. */
    lv_init();

    settings.window_width = 800;
    settings.window_height = 480;

    /* Initialize the configured default backend (DRM when LV_USE_LINUX_DRM=1,
     * FBDEV when LV_USE_LINUX_FBDEV=1, etc.) */
    driver_backends_register();
    if (driver_backends_init_backend(NULL) == -1) {
        fprintf(stderr, "Failed to initialize display backend\n");
        exit(EXIT_FAILURE);
    }

    /* Enable EVDEV touch/pointer input */
#if LV_USE_EVDEV
    if (driver_backends_init_backend("EVDEV") == -1) {
        fprintf(stderr, "Failed to initialize evdev backend\n");
        exit(EXIT_FAILURE);
    }
#endif

    /*Create a Demo*/
    lv_demo_widgets();
    lv_demo_widgets_start_slideshow();

    /* Enter the run loop - does not return */
    driver_backends_run_loop();

    return 0;
}
