/* Linked cabinet display/grid mapping. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include "../src/cabinet_layout.h"

static int failures, checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while (0)

int main(void)
{
    cabinet_layout_t l;
    /* One-screen four-player order is top-left, top-right, bottom-left,
     * bottom-right. Two screens group players 1/2 and 3/4 respectively. */
    for (int p = 0; p < 4; p++) {
        CHECK(cabinet_layout(4, p, 1, &l));
        CHECK(l.display == 0 && l.columns == 2 && l.rows == 2);
        CHECK(l.column == p % 2 && l.row == p / 2);
        CHECK(cabinet_layout(4, p, 2, &l));
        CHECK(l.display == p / 2 && l.column == p % 2 && l.row == 0);
        CHECK(l.on_display == 2 && l.columns == 2 && l.rows == 1);
        CHECK(cabinet_layout(4, p, 5, &l));
        CHECK(l.display == p && l.on_display == 1 && l.columns == 1 && l.rows == 1);
    }
    CHECK(cabinet_layout(2, 1, 1, &l) && l.column == 1 && l.rows == 1);
    CHECK(cabinet_layout(2, 1, 2, &l) && l.display == 1 && l.on_display == 1);
    CHECK(cabinet_layout(3, 2, 1, &l) && l.column == 0 && l.row == 1);
    /* Every supported topology has unique, valid cells and distributes
     * cabinets across displays with at most one cabinet difference. */
    for (int players = 2; players <= 4; players++) for (int displays = 1; displays <= 5; displays++) {
        int occupied[4][4] = {{0}}, population[4] = {0};
        int used = displays < players ? displays : players;
        for (int p = 0; p < players; p++) {
            CHECK(cabinet_layout(players, p, displays, &l));
            CHECK(l.display < used && l.column < l.columns && l.row < l.rows);
            CHECK(!occupied[l.display][l.row * l.columns + l.column]++);
            population[l.display]++;
        }
        for (int a = 0; a < used; a++) for (int b = 0; b < used; b++)
            CHECK(population[a] - population[b] <= 1);
    }
    CHECK(!cabinet_layout(1, 0, 1, &l));
    CHECK(!cabinet_layout(5, 0, 1, &l));
    CHECK(!cabinet_layout(4, 4, 1, &l));
    CHECK(!cabinet_layout(4, -1, 1, &l));
    CHECK(!cabinet_layout(4, 0, 0, &l));
    CHECK(!cabinet_layout(4, 0, 1, NULL));
    printf("cabinet layout tests: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
