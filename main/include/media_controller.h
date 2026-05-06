#ifndef MEDIA_CONTROLLER_H
#define MEDIA_CONTROLLER_H

#include "lvgl/lvgl.h"

#define STREAMPLAYER_KEY_SUBTITLE 0xF100u
#define STREAMPLAYER_KEY_CTRL_LEFT 0xF101u
#define STREAMPLAYER_KEY_CTRL_RIGHT 0xF102u

#ifdef __cplusplus
extern "C" {
#endif

void media_controller_init(void);
void media_controller_attach_keyboard(lv_indev_t *preferred);

#ifdef __cplusplus
}
#endif

#endif
