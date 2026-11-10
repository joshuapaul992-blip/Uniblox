/* Uniblox - tiny test harness shared by the host unit test files. */
#ifndef UB_TEST_H
#define UB_TEST_H

#include <stdio.h>
#include <string.h>

extern int g_failures, g_checks;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(a, b) do { \
    g_checks++; \
    if (strcmp((a), (b)) != 0) { g_failures++; \
        printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } \
} while (0)

void test_crt(void);
void test_compress(void);
void test_world(void);
void test_physics(void);
void test_libc(void);
void test_script(void);
void test_game(void);
void test_ui(void);
void test_editor(void);
void test_editor_app(void);
void test_vec(void);
void test_palette(void);
void test_render(void);
void test_render_2d(void);
void test_render_fuzz(void);
void test_meshes(void);

#endif
