/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VCT_NATIVE_MENU_H
#define VCT_NATIVE_MENU_H
/* preferred: -1 chooses an installed game; 0 Hydro, 1 Offroad. */
int native_menu_launcher(int preferred, int force_setup);
int native_menu_draw(void *renderer, int width, int height);
int native_menu_blocks_input(void);
void native_menu_event(const void *event);
void native_menu_cleanup(void);
#endif
