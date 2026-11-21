#ifndef LKG_MENU_VOLUME_H
#define LKG_MENU_VOLUME_H

int volume_get_percent(void);
int volume_set_percent(int percent);
void volume_preview_play(void);
void volume_preview_poll(void);
void volume_preview_stop(void);

#endif
