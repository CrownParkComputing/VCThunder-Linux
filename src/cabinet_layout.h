/* Linked cabinet placement, independent of the desktop API. SPDX-License-Identifier: MIT */
#ifndef VCT_CABINET_LAYOUT_H
#define VCT_CABINET_LAYOUT_H
typedef struct {
    int display, column, row, columns, rows, on_display;
} cabinet_layout_t;

static inline int cabinet_layout(int players, int cabinet, int displays,
                                 cabinet_layout_t *out)
{
    if (!out || players < 2 || players > 4 || cabinet < 0 ||
        cabinet >= players || displays < 1) return 0;
    if (displays > players) displays = players;
    int display = cabinet * displays / players;
    int first = (display * players + displays - 1) / displays;
    int end = ((display + 1) * players + displays - 1) / displays;
    int count = end - first, columns = count > 1 ? 2 : 1;
    *out = (cabinet_layout_t){display, (cabinet - first) % columns,
        (cabinet - first) / columns, columns, (count + columns - 1) / columns, count};
    return 1;
}
#endif
