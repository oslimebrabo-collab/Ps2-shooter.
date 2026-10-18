#ifndef GAME_H
#define GAME_H
#include <stdint.h>

/* Botoes logicos (independentes de plataforma) */
enum {
    IN_UP = 1, IN_DOWN = 2, IN_LEFT = 4, IN_RIGHT = 8,
    IN_FIRE = 16, IN_START = 32, IN_SELECT = 64
};

typedef struct {
    int connected;
    int buttons;     /* mascara IN_* */
    int lx, ly;      /* analogico esquerdo -128..127 */
    int rx, ry;      /* analogico direito  -128..127 */
} Input;

void game_init(int screen_w, int screen_h);
void game_update(const Input in[2]);
void game_draw(void);
int  game_wants_quit(void);

/* Fornecida pela plataforma (main.c): retangulo preenchido, 0xRRGGBB */
void plat_rect(int x, int y, int w, int h, uint32_t rgb);

#endif
