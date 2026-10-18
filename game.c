/*
 * ZONA DE TIRO - jogo 2D top-down shooter (1-2 jogadores, co-op)
 * Logica 100% independente de plataforma. Desenha so com plat_rect().
 */
#include "game.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TILE     32
#define MAP_W    128            /* 4096 px de largura  */
#define MAP_H    64             /* 2048 px de altura   */
#define WORLD_W  (MAP_W * TILE)
#define WORLD_H  (MAP_H * TILE)
#define UNREACH  0xFFFF

#define MAX_ENEMY   64
#define MAX_BULLET  160
#define MAX_PICKUP  16
#define MAX_PART    96
#define MAX_MRUN    900

#define PSPEED  2.4f
#define PRAD    7.0f
#define MAX_HP  10

enum { T_GRASS, T_SAND, T_WALL, T_WATER };
enum { ST_MENU, ST_PLAY, ST_PAUSE, ST_OVER };
enum { E_GRUNT, E_SHOOTER, E_BRUTE };

/* cores */
#define C_GRASS   0x2f6b26
#define C_GRASS2  0x34722a
#define C_TUFT    0x4a8f3a
#define C_SAND    0xc9b57a
#define C_SAND2   0xb59f63
#define C_WALL    0x6b6b78
#define C_WALL_HI 0x9a9aaa
#define C_WALL_LO 0x45454f
#define C_WATER   0x2a5fb0
#define C_WATER_HI 0x5c8fe0

/* ------------------------------------------------------------------ */
/* estado global                                                      */
/* ------------------------------------------------------------------ */
static int   g_sw, g_sh;
static float g_spd = 1.0f;
static int   state, menu_sel, menu_msg, quit_flag, over_t;
static unsigned frame;
static int   prev_btn[2];

static uint8_t  map[MAP_H][MAP_W];
static uint16_t dist[MAP_H][MAP_W];
static uint16_t bfs_q[MAP_W * MAP_H];

typedef struct { uint8_t x, y, w, t; } MRun;
static MRun mrun[MAX_MRUN];
static int  nmrun;

typedef struct {
    float x, y, ax, ay;
    int active, alive, hp, cd, inv, rapid, score, respawn;
} Player;
static Player pl[2];

typedef struct { float x, y; int type, hp, alive, cd, hit; } Enemy;
static Enemy en[MAX_ENEMY];

typedef struct { float x, y, vx, vy; int life, owner, alive; } Bullet;
static Bullet bl[MAX_BULLET];

typedef struct { float x, y; int type, life, alive; } Pickup;
static Pickup pk[MAX_PICKUP];

typedef struct { float x, y, vx, vy; int life; uint32_t c; } Part;
static Part part[MAX_PART];
static unsigned part_i;

static int wave, q_grunt, q_shoot, q_brute, spawn_t, wave_delay, banner;

/* ------------------------------------------------------------------ */
/* utilidades                                                         */
/* ------------------------------------------------------------------ */
static uint32_t rng_state = 1;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }
static int rnd_range(int a, int b) { return a + (int)(rnd() % (uint32_t)(b - a + 1)); }

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static int in_map(int x, int y) { return x >= 0 && y >= 0 && x < MAP_W && y < MAP_H; }
static int tile_at(int tx, int ty) { return in_map(tx, ty) ? map[ty][tx] : T_WALL; }
static int walkable_tile(int tx, int ty) { int t = tile_at(tx, ty); return t != T_WALL && t != T_WATER; }

/* ------------------------------------------------------------------ */
/* geracao do mapa                                                    */
/* ------------------------------------------------------------------ */
static int cell_type(int cx, int cy)
{
    int w = 0, wa = 0;
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            int t = map[cy * 2 + dy][cx * 2 + dx];
            if (t == T_WALL) w = 1; else if (t == T_WATER) wa = 1;
        }
    return w ? 2 : (wa ? 1 : 0);
}

static void build_minimap(void)
{
    nmrun = 0;
    for (int cy = 0; cy < MAP_H / 2; cy++) {
        int cx = 0;
        while (cx < MAP_W / 2) {
            int t = cell_type(cx, cy);
            if (!t) { cx++; continue; }
            int s = cx;
            while (cx < MAP_W / 2 && cell_type(cx, cy) == t) cx++;
            if (nmrun < MAX_MRUN) {
                mrun[nmrun].x = (uint8_t)s; mrun[nmrun].y = (uint8_t)cy;
                mrun[nmrun].w = (uint8_t)(cx - s); mrun[nmrun].t = (uint8_t)t;
                nmrun++;
            }
        }
    }
}

static void gen_map(uint32_t seed)
{
    rng_state = seed ? seed : 1;
    memset(map, T_GRASS, sizeof map);

    /* lagos */
    for (int i = 0; i < 16; i++) {
        int cx = rnd_range(8, MAP_W - 9), cy = rnd_range(6, MAP_H - 7);
        int rx = rnd_range(3, 7), ry = rnd_range(2, 5);
        for (int y = cy - ry; y <= cy + ry; y++)
            for (int x = cx - rx; x <= cx + rx; x++) {
                if (!in_map(x, y)) continue;
                int dx = x - cx, dy = y - cy;
                if (dx * dx * ry * ry + dy * dy * rx * rx <= rx * rx * ry * ry) map[y][x] = T_WATER;
            }
    }
    /* areia na margem */
    for (int y = 1; y < MAP_H - 1; y++)
        for (int x = 1; x < MAP_W - 1; x++)
            if (map[y][x] == T_GRASS &&
                (map[y][x-1] == T_WATER || map[y][x+1] == T_WATER ||
                 map[y-1][x] == T_WATER || map[y+1][x] == T_WATER))
                map[y][x] = T_SAND;

    /* construcoes com portas */
    int placed = 0;
    for (int tries = 0; tries < 500 && placed < 34; tries++) {
        int w = rnd_range(6, 12), h = rnd_range(5, 9);
        int x0 = rnd_range(3, MAP_W - w - 4), y0 = rnd_range(3, MAP_H - h - 4);
        int ok = 1;
        for (int y = y0 - 1; y <= y0 + h && ok; y++)
            for (int x = x0 - 1; x <= x0 + w && ok; x++) {
                int t = map[y][x];
                if (t == T_WATER || t == T_WALL) ok = 0;
            }
        if (!ok) continue;
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++) {
                int border = (x == x0 || x == x0 + w - 1 || y == y0 || y == y0 + h - 1);
                map[y][x] = border ? T_WALL : T_SAND;
            }
        int dxp = rnd_range(x0 + 1, x0 + w - 3);
        int wy = (rnd() & 1) ? y0 : y0 + h - 1;
        map[wy][dxp] = map[wy][dxp + 1] = T_SAND;
        int dyp = rnd_range(y0 + 1, y0 + h - 3);
        int wx = (rnd() & 1) ? x0 : x0 + w - 1;
        map[dyp][wx] = map[dyp + 1][wx] = T_SAND;
        if (w >= 9 && h >= 7) map[y0 + h / 2][x0 + w / 2] = T_WALL;
        placed++;
    }
    /* borda do mundo (2 tiles) */
    for (int x = 0; x < MAP_W; x++) { map[0][x] = map[1][x] = map[MAP_H-1][x] = map[MAP_H-2][x] = T_WALL; }
    for (int y = 0; y < MAP_H; y++) { map[y][0] = map[y][1] = map[y][MAP_W-1] = map[y][MAP_W-2] = T_WALL; }
    /* area de spawn limpa no centro */
    int cx = MAP_W / 2, cy = MAP_H / 2;
    for (int y = cy - 8; y <= cy + 8; y++)
        for (int x = cx - 8; x <= cx + 8; x++)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= 64) map[y][x] = T_GRASS;

    build_minimap();
}

/* ------------------------------------------------------------------ */
/* colisao / movimento / visao                                        */
/* ------------------------------------------------------------------ */
static int blocked(float x, float y, float r)
{
    int x0 = (int)((x - r) / TILE), x1 = (int)((x + r) / TILE);
    int y0 = (int)((y - r) / TILE), y1 = (int)((y + r) / TILE);
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++)
            if (!walkable_tile(tx, ty)) return 1;
    return 0;
}

static void move_box(float *x, float *y, float dx, float dy, float r)
{
    float nx = *x + dx;
    if (!blocked(nx, *y, r)) *x = nx;
    float ny = *y + dy;
    if (!blocked(*x, ny, r)) *y = ny;
}

static int los(float x0, float y0, float x1, float y1, int block_water)
{
    float dx = x1 - x0, dy = y1 - y0;
    int n = (int)(sqrtf(dx * dx + dy * dy) / 8.0f) + 1;
    for (int i = 1; i < n; i++) {
        float t = (float)i / (float)n;
        int tt = tile_at((int)((x0 + dx * t) / TILE), (int)((y0 + dy * t) / TILE));
        if (tt == T_WALL || (block_water && tt == T_WATER)) return 0;
    }
    return 1;
}

static void step_toward(float *x, float *y, float tx, float ty, float sp, float r)
{
    float dx = tx - *x, dy = ty - *y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < 0.01f) return;
    if (d < sp) sp = d;
    move_box(x, y, dx / d * sp, dy / d * sp, r);
}

/* campo de distancia (BFS) ate os jogadores vivos: navegacao dos inimigos */
static void build_flow(void)
{
    static const int dxs[4] = { 1, -1, 0, 0 }, dys[4] = { 0, 0, 1, -1 };
    int head = 0, tail = 0;
    memset(dist, 0xFF, sizeof dist);
    for (int i = 0; i < 2; i++) {
        if (!pl[i].active || !pl[i].alive) continue;
        int tx = (int)(pl[i].x / TILE), ty = (int)(pl[i].y / TILE);
        if (in_map(tx, ty) && walkable_tile(tx, ty) && dist[ty][tx] == UNREACH) {
            dist[ty][tx] = 0; bfs_q[tail++] = (uint16_t)(ty * MAP_W + tx);
        }
    }
    while (head < tail) {
        int cur = bfs_q[head++], cx = cur % MAP_W, cy = cur / MAP_W;
        uint16_t d = (uint16_t)(dist[cy][cx] + 1);
        for (int k = 0; k < 4; k++) {
            int nx = cx + dxs[k], ny = cy + dys[k];
            if (in_map(nx, ny) && walkable_tile(nx, ny) && dist[ny][nx] == UNREACH) {
                dist[ny][nx] = d; bfs_q[tail++] = (uint16_t)(ny * MAP_W + nx);
            }
        }
    }
}

static void flow_target(int cx, int cy, float *tx, float *ty)
{
    uint16_t best = dist[cy][cx];
    int bx = cx, by = cy;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = cx + dx, ny = cy + dy;
            if (!in_map(nx, ny) || dist[ny][nx] == UNREACH) continue;
            if (dx && dy && (!walkable_tile(cx + dx, cy) || !walkable_tile(cx, cy + dy))) continue;
            if (dist[ny][nx] < best) { best = dist[ny][nx]; bx = nx; by = ny; }
        }
    *tx = bx * TILE + TILE * 0.5f;
    *ty = by * TILE + TILE * 0.5f;
}

static int nearest_player(float x, float y, float *outd)
{
    int best = -1; float bd = 1e30f;
    for (int i = 0; i < 2; i++) {
        if (!pl[i].active || !pl[i].alive) continue;
        float dx = pl[i].x - x, dy = pl[i].y - y, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    if (outd) *outd = best >= 0 ? sqrtf(bd) : 1e9f;
    return best;
}

/* ------------------------------------------------------------------ */
/* entidades                                                          */
/* ------------------------------------------------------------------ */
static void add_part(float x, float y, uint32_t c, int n)
{
    for (int i = 0; i < n; i++) {
        Part *p = &part[part_i++ % MAX_PART];
        float a = (float)(rnd() % 628) / 100.0f, sp = 0.6f + (float)(rnd() % 150) / 100.0f;
        p->x = x; p->y = y; p->vx = cosf(a) * sp; p->vy = sinf(a) * sp;
        p->life = 14 + (int)(rnd() % 14); p->c = c;
    }
}

static void fire_bullet(float x, float y, float dx, float dy, float sp, int owner, int life)
{
    for (int i = 0; i < MAX_BULLET; i++)
        if (!bl[i].alive) {
            bl[i].x = x; bl[i].y = y; bl[i].vx = dx * sp; bl[i].vy = dy * sp;
            bl[i].life = life; bl[i].owner = owner; bl[i].alive = 1;
            return;
        }
}

static void spawn_pickup(float x, float y, int type)
{
    for (int i = 0; i < MAX_PICKUP; i++)
        if (!pk[i].alive) { pk[i].x = x; pk[i].y = y; pk[i].type = type; pk[i].life = 900; pk[i].alive = 1; return; }
}

static float enemy_radius(int t) { return t == E_BRUTE ? 13.0f : (t == E_SHOOTER ? 9.0f : 8.0f); }
static float enemy_speed(int t)
{
    float w = wave * 0.025f; if (w > 0.6f) w = 0.6f;
    float b = t == E_BRUTE ? 0.85f : (t == E_SHOOTER ? 1.0f : 1.35f);
    return b + w;
}

static int enemies_alive(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENEMY; i++) n += en[i].alive;
    return n;
}

static int spawn_enemy(int type)
{
    int slot = -1;
    for (int i = 0; i < MAX_ENEMY; i++) if (!en[i].alive) { slot = i; break; }
    if (slot < 0) return 0;
    for (int tries = 0; tries < 80; tries++) {
        int tx = rnd_range(3, MAP_W - 4), ty = rnd_range(3, MAP_H - 4);
        if (dist[ty][tx] == UNREACH || !walkable_tile(tx, ty)) continue;
        float x = tx * TILE + TILE * 0.5f, y = ty * TILE + TILE * 0.5f, d;
        nearest_player(x, y, &d);
        float mn = tries < 50 ? 320.0f : 200.0f, mx = tries < 50 ? 900.0f : 3000.0f;
        if (d < mn || d > mx) continue;
        Enemy *e = &en[slot];
        e->x = x; e->y = y; e->type = type; e->alive = 1; e->hit = 0;
        e->cd = 40 + (int)(rnd() % 60);
        e->hp = type == E_GRUNT ? 2 + wave / 6 : (type == E_SHOOTER ? 3 + wave / 6 : 8 + wave / 2);
        return 1;
    }
    return 0;
}

static void hurt_player(int i, int dmg)
{
    Player *p = &pl[i];
    if (!p->alive || p->inv > 0) return;
    p->hp -= dmg; p->inv = 55;
    add_part(p->x, p->y, 0xd03030, 8);
    if (p->hp <= 0) { p->hp = 0; p->alive = 0; p->respawn = 420; add_part(p->x, p->y, 0xffffff, 18); }
}

static void kill_enemy(Enemy *e, int owner)
{
    e->alive = 0;
    int pts = e->type == E_GRUNT ? 10 : (e->type == E_SHOOTER ? 25 : 50);
    if (owner >= 0 && owner < 2) pl[owner].score += pts;
    add_part(e->x, e->y, e->type == E_GRUNT ? 0xd03a3a : (e->type == E_SHOOTER ? 0x9a4ad0 : 0xb03030), 10);
    int r = (int)(rnd() % 100);
    if (r < 14) spawn_pickup(e->x, e->y, 0); else if (r < 20) spawn_pickup(e->x, e->y, 1);
}

/* ------------------------------------------------------------------ */
/* ondas                                                              */
/* ------------------------------------------------------------------ */
static void start_wave(void)
{
    int np = pl[0].active + pl[1].active; if (np < 1) np = 1;
    float m = np == 2 ? 1.5f : 1.0f;
    wave++;
    q_grunt = (int)((5 + wave * 2) * m);
    q_shoot = wave >= 2 ? (int)((wave - 1) * m) : 0; if (q_shoot > 14) q_shoot = 14;
    q_brute = wave >= 3 ? (int)(((wave - 1) / 2) * m) : 0; if (q_brute > 8) q_brute = 8;
    spawn_t = 0; banner = 150;
    build_flow();
    if (wave > 1)
        for (int i = 0; i < 2; i++)
            if (pl[i].active && pl[i].alive) spawn_pickup(pl[i].x + 28, pl[i].y, 0);
}

static void update_waves(void)
{
    if (banner > 0) banner--;
    if (wave_delay > 0) { if (--wave_delay == 0) start_wave(); return; }
    int tot = q_grunt + q_shoot + q_brute;
    if (tot > 0) {
        if (--spawn_t <= 0 && enemies_alive() < 40) {
            int r = (int)(rnd() % (uint32_t)tot), type;
            if (r < q_grunt) type = E_GRUNT; else if (r < q_grunt + q_shoot) type = E_SHOOTER; else type = E_BRUTE;
            if (spawn_enemy(type)) {
                if (type == E_GRUNT) q_grunt--; else if (type == E_SHOOTER) q_shoot--; else q_brute--;
            }
            spawn_t = 22;
        }
    } else if (enemies_alive() == 0) {
        wave_delay = 180;
    }
}

/* ------------------------------------------------------------------ */
/* atualizacao                                                        */
/* ------------------------------------------------------------------ */
static void update_player(int i, const Input *in)
{
    Player *p = &pl[i];
    if (!p->active) return;
    if (p->inv > 0) p->inv--;
    if (p->rapid > 0) p->rapid--;
    if (p->cd > 0) p->cd--;

    if (!p->alive) {
        Player *o = &pl[1 - i];
        if (o->active && o->alive && --p->respawn <= 0) {
            p->alive = 1; p->hp = 5; p->inv = 120; p->x = o->x + 24; p->y = o->y;
            if (blocked(p->x, p->y, PRAD)) { p->x = o->x; p->y = o->y; }
        }
        return;
    }

    float mx = 0, my = 0;
    if (in->buttons & IN_LEFT)  mx -= 1;
    if (in->buttons & IN_RIGHT) mx += 1;
    if (in->buttons & IN_UP)    my -= 1;
    if (in->buttons & IN_DOWN)  my += 1;
    float sx = in->lx / 127.0f, sy = in->ly / 127.0f;
    if (fabsf(sx) < 0.2f) sx = 0;
    if (fabsf(sy) < 0.2f) sy = 0;
    mx += sx; my += sy;
    float m = sqrtf(mx * mx + my * my);
    if (m > 1.0f) { mx /= m; my /= m; m = 1.0f; }
    move_box(&p->x, &p->y, mx * PSPEED * g_spd, my * PSPEED * g_spd, PRAD);

    float ax = in->rx / 127.0f, ay = in->ry / 127.0f, am = sqrtf(ax * ax + ay * ay);
    if (am > 0.35f) { p->ax = ax / am; p->ay = ay / am; }
    else if (m > 0.2f) { p->ax = mx / m; p->ay = my / m; }

    if ((in->buttons & IN_FIRE) && p->cd <= 0) {
        fire_bullet(p->x + p->ax * 12, p->y + p->ay * 12, p->ax, p->ay, 7.0f * g_spd, i, 70);
        p->cd = p->rapid > 0 ? 4 : 8;
    }
}

static void update_bullets(void)
{
    for (int i = 0; i < MAX_BULLET; i++) {
        Bullet *b = &bl[i];
        if (!b->alive) continue;
        b->x += b->vx; b->y += b->vy;
        if (--b->life <= 0) { b->alive = 0; continue; }
        if (b->x < 0 || b->y < 0 || tile_at((int)(b->x / TILE), (int)(b->y / TILE)) == T_WALL) {
            add_part(b->x, b->y, 0xcccccc, 3); b->alive = 0; continue;
        }
        if (b->owner < 2) {
            for (int j = 0; j < MAX_ENEMY; j++) {
                Enemy *e = &en[j];
                if (!e->alive) continue;
                float dx = e->x - b->x, dy = e->y - b->y, r = enemy_radius(e->type) + 3;
                if (dx * dx + dy * dy < r * r) {
                    e->hp--; e->hit = 4; b->alive = 0;
                    add_part(b->x, b->y, 0xffe040, 2);
                    if (e->hp <= 0) kill_enemy(e, b->owner);
                    break;
                }
            }
        } else {
            for (int j = 0; j < 2; j++) {
                if (!pl[j].active || !pl[j].alive) continue;
                float dx = pl[j].x - b->x, dy = pl[j].y - b->y;
                if (dx * dx + dy * dy < 11.0f * 11.0f) { hurt_player(j, 1); b->alive = 0; break; }
            }
        }
    }
}

static void update_enemies(void)
{
    for (int i = 0; i < MAX_ENEMY; i++) {
        Enemy *e = &en[i];
        if (!e->alive) continue;
        if (e->hit > 0) e->hit--;
        float d;
        int pi = nearest_player(e->x, e->y, &d);
        if (pi < 0) continue;
        Player *p = &pl[pi];
        float r = enemy_radius(e->type), sp = enemy_speed(e->type) * g_spd;
        int tx = (int)(e->x / TILE), ty = (int)(e->y / TILE);
        int direct = (d < 48.0f) || (d < 260.0f && los(e->x, e->y, p->x, p->y, 1));
        float gx = p->x, gy = p->y;
        if (!direct && in_map(tx, ty) && dist[ty][tx] != UNREACH && dist[ty][tx] > 0)
            flow_target(tx, ty, &gx, &gy);

        if (e->type == E_SHOOTER) {
            int see = d < 300.0f && los(e->x, e->y, p->x, p->y, 0);
            if (see && d < 150.0f && d > 0.01f)
                move_box(&e->x, &e->y, (e->x - p->x) / d * sp, (e->y - p->y) / d * sp, r);
            else if (!(see && d < 230.0f))
                step_toward(&e->x, &e->y, gx, gy, sp, r);
            if (see && --e->cd <= 0 && d > 0.01f) {
                float ax = (p->x - e->x) / d, ay = (p->y - e->y) / d;
                float j = ((float)(rnd() % 100) - 50.0f) / 500.0f;
                fire_bullet(e->x + ax * 10, e->y + ay * 10, ax - ay * j, ay + ax * j, 3.2f * g_spd, 2, 200);
                e->cd = 100 - wave * 3; if (e->cd < 45) e->cd = 45;
            }
        } else {
            step_toward(&e->x, &e->y, gx, gy, sp, r);
        }
        if (d < r + 8.0f) hurt_player(pi, e->type == E_BRUTE ? 2 : 1);
    }
    /* separacao entre inimigos */
    for (int i = 0; i < MAX_ENEMY; i++) {
        if (!en[i].alive) continue;
        for (int j = i + 1; j < MAX_ENEMY; j++) {
            if (!en[j].alive) continue;
            float dx = en[j].x - en[i].x, dy = en[j].y - en[i].y;
            float rr = enemy_radius(en[i].type) + enemy_radius(en[j].type);
            float d2 = dx * dx + dy * dy;
            if (d2 < rr * rr && d2 > 0.0001f) {
                float d = sqrtf(d2), push = (rr - d) * 0.25f;
                move_box(&en[i].x, &en[i].y, -dx / d * push, -dy / d * push, enemy_radius(en[i].type));
                move_box(&en[j].x, &en[j].y,  dx / d * push,  dy / d * push, enemy_radius(en[j].type));
            }
        }
    }
}

static void update_pickups(void)
{
    for (int i = 0; i < MAX_PICKUP; i++) {
        Pickup *k = &pk[i];
        if (!k->alive) continue;
        if (--k->life <= 0) { k->alive = 0; continue; }
        for (int j = 0; j < 2; j++) {
            if (!pl[j].active || !pl[j].alive) continue;
            float dx = pl[j].x - k->x, dy = pl[j].y - k->y;
            if (dx * dx + dy * dy < 18.0f * 18.0f) {
                if (k->type == 0) { pl[j].hp += 3; if (pl[j].hp > MAX_HP) pl[j].hp = MAX_HP; }
                else pl[j].rapid = 600;
                add_part(k->x, k->y, k->type == 0 ? 0x40ff70 : 0xffe040, 8);
                k->alive = 0; break;
            }
        }
    }
}

static void update_particles(void)
{
    for (int i = 0; i < MAX_PART; i++) {
        Part *p = &part[i];
        if (p->life <= 0) continue;
        p->life--; p->x += p->vx; p->y += p->vy; p->vx *= 0.92f; p->vy *= 0.92f;
    }
}

static void init_player(int i, float x, float y)
{
    Player *p = &pl[i];
    memset(p, 0, sizeof *p);
    p->active = 1; p->alive = 1; p->hp = MAX_HP; p->x = x; p->y = y;
    p->ax = i ? -1.0f : 1.0f; p->ay = 0;
}

static void start_game(int np)
{
    memset(pl, 0, sizeof pl); memset(en, 0, sizeof en); memset(bl, 0, sizeof bl);
    memset(pk, 0, sizeof pk); memset(part, 0, sizeof part);
    gen_map(0x9E3779B9u ^ (frame * 2654435761u));
    init_player(0, WORLD_W * 0.5f - 24, WORLD_H * 0.5f);
    if (np == 2) init_player(1, WORLD_W * 0.5f + 24, WORLD_H * 0.5f);
    wave = 0; q_grunt = q_shoot = q_brute = 0; wave_delay = 60; banner = 0; over_t = 0;
    build_flow();
    state = ST_PLAY;
}

void game_init(int w, int h)
{
    g_sw = w; g_sh = h;
    g_spd = h > 480 ? 1.2f : 1.0f;      /* PAL roda a 50Hz: compensa a velocidade */
    gen_map(0xC0FFEEu);
    state = ST_MENU; menu_sel = 0;
}

int game_wants_quit(void) { return quit_flag; }

void game_update(const Input in[2])
{
    int pr[2];
    for (int i = 0; i < 2; i++) pr[i] = in[i].buttons & ~prev_btn[i];
    frame++;

    switch (state) {
    case ST_MENU:
        if (menu_msg > 0) menu_msg--;
        if ((pr[0] | pr[1]) & (IN_UP | IN_DOWN)) menu_sel ^= 1;
        if ((pr[0] | pr[1]) & (IN_START | IN_FIRE)) {
            if (menu_sel == 1 && !in[1].connected) menu_msg = 180;
            else start_game(menu_sel + 1);
        }
        if ((pr[0] | pr[1]) & IN_SELECT) quit_flag = 1;
        break;

    case ST_PLAY:
        if (!pl[1].active && in[1].connected && (pr[1] & IN_START)) {     /* P2 entra no meio do jogo */
            float x = pl[0].alive ? pl[0].x + 24 : WORLD_W * 0.5f, y = pl[0].alive ? pl[0].y : WORLD_H * 0.5f;
            if (blocked(x, y, PRAD)) { x -= 24; }
            init_player(1, x, y);
        } else if ((pr[0] & IN_START) || (pl[1].active && (pr[1] & IN_START))) {
            state = ST_PAUSE; break;
        }
        for (int i = 0; i < 2; i++) update_player(i, &in[i]);
        update_bullets();
        if ((frame & 7) == 0) build_flow();
        update_enemies();
        update_waves();
        update_pickups();
        update_particles();
        if (!(pl[0].active && pl[0].alive) && !(pl[1].active && pl[1].alive)) { state = ST_OVER; over_t = 0; }
        break;

    case ST_PAUSE:
        if ((pr[0] | pr[1]) & IN_START) state = ST_PLAY;
        if ((pr[0] | pr[1]) & IN_SELECT) state = ST_MENU;
        break;

    case ST_OVER:
        update_particles();
        if (++over_t > 60 && ((pr[0] | pr[1]) & IN_START)) state = ST_MENU;
        break;
    }
    for (int i = 0; i < 2; i++) prev_btn[i] = in[i].buttons;
}

/* ------------------------------------------------------------------ */
/* desenho                                                            */
/* ------------------------------------------------------------------ */
static int cx0, cy0, cx1, cy1;
static void set_clip(int x, int y, int w, int h) { cx0 = x; cy0 = y; cx1 = x + w; cy1 = y + h; }

static void rect(int x, int y, int w, int h, uint32_t c)
{
    int x1 = x + w, y1 = y + h;
    if (x < cx0) x = cx0;
    if (y < cy0) y = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x1 <= x || y1 <= y) return;
    plat_rect(x, y, x1 - x, y1 - y, c);
}

/* fonte 3x5 */
#define G(a,b,c,d,e) ((uint16_t)(((a)<<12)|((b)<<9)|((c)<<6)|((d)<<3)|(e)))
static const uint16_t font_alpha[26] = {
    G(0b010,0b101,0b111,0b101,0b101), G(0b110,0b101,0b110,0b101,0b110), G(0b011,0b100,0b100,0b100,0b011),
    G(0b110,0b101,0b101,0b101,0b110), G(0b111,0b100,0b110,0b100,0b111), G(0b111,0b100,0b110,0b100,0b100),
    G(0b011,0b100,0b101,0b101,0b011), G(0b101,0b101,0b111,0b101,0b101), G(0b111,0b010,0b010,0b010,0b111),
    G(0b001,0b001,0b001,0b101,0b010), G(0b101,0b101,0b110,0b101,0b101), G(0b100,0b100,0b100,0b100,0b111),
    G(0b101,0b111,0b111,0b101,0b101), G(0b110,0b101,0b101,0b101,0b101), G(0b010,0b101,0b101,0b101,0b010),
    G(0b110,0b101,0b110,0b100,0b100), G(0b010,0b101,0b101,0b110,0b011), G(0b110,0b101,0b110,0b101,0b101),
    G(0b011,0b100,0b010,0b001,0b110), G(0b111,0b010,0b010,0b010,0b010), G(0b101,0b101,0b101,0b101,0b111),
    G(0b101,0b101,0b101,0b101,0b010), G(0b101,0b101,0b111,0b111,0b101), G(0b101,0b101,0b010,0b101,0b101),
    G(0b101,0b101,0b010,0b010,0b010), G(0b111,0b001,0b010,0b100,0b111)
};
static const uint16_t font_digit[10] = {
    G(0b111,0b101,0b101,0b101,0b111), G(0b010,0b110,0b010,0b010,0b111), G(0b110,0b001,0b010,0b100,0b111),
    G(0b110,0b001,0b010,0b001,0b110), G(0b101,0b101,0b111,0b001,0b001), G(0b111,0b100,0b110,0b001,0b110),
    G(0b011,0b100,0b111,0b101,0b111), G(0b111,0b001,0b010,0b010,0b010), G(0b111,0b101,0b111,0b101,0b111),
    G(0b111,0b101,0b111,0b001,0b110)
};

static uint16_t glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return font_alpha[c - 'A'];
    if (c >= '0' && c <= '9') return font_digit[c - '0'];
    switch (c) {
    case ':': return G(0b000,0b010,0b000,0b010,0b000);
    case '-': return G(0b000,0b000,0b111,0b000,0b000);
    case '!': return G(0b010,0b010,0b010,0b000,0b010);
    case '.': return G(0b000,0b000,0b000,0b000,0b010);
    case '/': return G(0b001,0b001,0b010,0b100,0b100);
    case '>': return G(0b100,0b010,0b001,0b010,0b100);
    case '+': return G(0b000,0b010,0b111,0b010,0b000);
    }
    return 0;
}

static void draw_glyph(int x, int y, int s, uint32_t col, uint16_t g)
{
    for (int r = 0; r < 5; r++) {
        int bits = (g >> (3 * (4 - r))) & 7, c = 0;
        while (c < 3) {
            if ((bits >> (2 - c)) & 1) {
                int st = c;
                while (c < 3 && ((bits >> (2 - c)) & 1)) c++;
                rect(x + st * s, y + r * s, (c - st) * s, s, col);
            } else c++;
        }
    }
}

static int text_w(const char *s, int sc) { int n = (int)strlen(s); return n ? n * 4 * sc - sc : 0; }

static void text(int x, int y, int sc, uint32_t col, const char *s)
{
    for (; *s; s++, x += 4 * sc) if (*s != ' ') draw_glyph(x, y, sc, col, glyph(*s));
}
static void text_sh(int x, int y, int sc, uint32_t col, const char *s)
{
    text(x + sc, y + sc, sc, 0x000000, s); text(x, y, sc, col, s);
}
static void text_c(int x, int w, int y, int sc, uint32_t col, const char *s)
{
    text(x + (w - text_w(s, sc)) / 2, y, sc, col, s);
}

/* viewport atual */
static int view_x, view_y, view_w, view_h, view_cx, view_cy;
static int WX(float x) { return view_x + (int)x - view_cx; }
static int WY(float y) { return view_y + (int)y - view_cy; }
static int on_screen(float x, float y, int m)
{
    int sx = WX(x), sy = WY(y);
    return sx >= view_x - m && sx <= view_x + view_w + m && sy >= view_y - m && sy <= view_y + view_h + m;
}

static void draw_tiles(void)
{
    int tx0 = view_cx / TILE, ty0 = view_cy / TILE;
    int tx1 = (view_cx + view_w - 1) / TILE, ty1 = (view_cy + view_h - 1) / TILE;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            int sx = view_x + tx * TILE - view_cx, sy = view_y + ty * TILE - view_cy;
            uint32_t h = hash2(tx, ty);
            switch (tile_at(tx, ty)) {
            case T_GRASS:
                if (h % 9 == 0) rect(sx, sy, TILE, TILE, C_GRASS2);
                if (h % 5 == 0) rect(sx + (int)((h >> 8) % 24) + 2, sy + (int)((h >> 12) % 24) + 2, 2, 3, C_TUFT);
                break;
            case T_SAND:
                rect(sx, sy, TILE, TILE, C_SAND);
                if (h % 4 == 0) rect(sx + (int)((h >> 8) % 24) + 2, sy + (int)((h >> 12) % 24) + 2, 3, 2, C_SAND2);
                break;
            case T_WALL:
                rect(sx, sy, TILE, TILE, C_WALL);
                rect(sx, sy, TILE, 5, C_WALL_HI);
                rect(sx, sy + TILE - 4, TILE, 4, C_WALL_LO);
                break;
            case T_WATER: {
                rect(sx, sy, TILE, TILE, C_WATER);
                int o = (int)(((frame >> 3) + (unsigned)(tx * 7 + ty * 3)) % 24);
                rect(sx + o, sy + 6 + (int)((h >> 4) % 3) * 9, 8, 2, C_WATER_HI);
                break; }
            }
        }
}

static void draw_player(int i)
{
    Player *p = &pl[i];
    if (!p->active || !on_screen(p->x, p->y, 20)) return;
    int sx = WX(p->x), sy = WY(p->y);
    if (!p->alive) { rect(sx - 8, sy - 2, 16, 4, 0x555555); rect(sx - 2, sy - 8, 4, 16, 0x555555); return; }
    if (p->inv > 0 && ((p->inv >> 2) & 1)) return;
    uint32_t body = i ? 0xff9a2e : 0x2e7bff, dark = i ? 0x99500f : 0x143f99;
    rect(sx - 7, sy + 7, 14, 3, 0x1a3a14);
    rect(sx - 8, sy - 8, 16, 16, dark);
    rect(sx - 6, sy - 6, 12, 12, body);
    for (int k = 1; k <= 3; k++)
        rect(sx + (int)(p->ax * (6 + k * 3)) - 2, sy + (int)(p->ay * (6 + k * 3)) - 2, 4, 4, k == 3 ? 0xffffff : 0xcfcfcf);
}

static void draw_enemy(const Enemy *e)
{
    if (!on_screen(e->x, e->y, 24)) return;
    int sx = WX(e->x), sy = WY(e->y);
    uint32_t dk, bd, ey = 0xffe040;
    int r = (int)enemy_radius(e->type);
    if (e->type == E_GRUNT)        { dk = 0x7a1414; bd = 0xd03a3a; }
    else if (e->type == E_SHOOTER) { dk = 0x4a1a6a; bd = 0x9a4ad0; }
    else                           { dk = 0x4a0f0f; bd = 0xa02828; }
    if (e->hit > 0) bd = 0xffffff;
    rect(sx - r + 1, sy + r - 2, 2 * r - 2, 3, 0x1a3a14);
    rect(sx - r, sy - r, 2 * r, 2 * r, dk);
    rect(sx - r + 2, sy - r + 2, 2 * r - 4, 2 * r - 4, bd);
    rect(sx - r / 2 - 1, sy - r / 3, 3, 3, ey);
    rect(sx + r / 2 - 2, sy - r / 3, 3, 3, ey);
    if (e->type == E_SHOOTER) rect(sx - 2, sy + 2, 4, 4, 0xffffff);
}

static void draw_entities(void)
{
    for (int i = 0; i < MAX_PICKUP; i++) {
        Pickup *k = &pk[i];
        if (!k->alive || !on_screen(k->x, k->y, 16)) continue;
        if (k->life < 180 && ((k->life >> 3) & 1)) continue;
        int sx = WX(k->x), sy = WY(k->y) + (int)((frame >> 4) & 1);
        if (k->type == 0) {
            rect(sx - 7, sy - 7, 14, 14, 0xffffff);
            rect(sx - 2, sy - 5, 4, 10, 0xe03030); rect(sx - 5, sy - 2, 10, 4, 0xe03030);
        } else {
            rect(sx - 7, sy - 7, 14, 14, 0x403000);
            rect(sx - 5, sy - 5, 10, 10, 0xffe040);
            rect(sx - 1, sy - 4, 3, 8, 0x403000);
        }
    }
    for (int i = 0; i < MAX_ENEMY; i++) if (en[i].alive) draw_enemy(&en[i]);
    for (int i = 0; i < MAX_BULLET; i++) {
        Bullet *b = &bl[i];
        if (!b->alive || !on_screen(b->x, b->y, 6)) continue;
        int sx = WX(b->x), sy = WY(b->y);
        if (b->owner < 2) { rect(sx - 2, sy - 2, 4, 4, 0xffe040); rect(sx - 1, sy - 1, 2, 2, 0xffffff); }
        else              { rect(sx - 3, sy - 3, 6, 6, 0xff4aa0); rect(sx - 1, sy - 1, 2, 2, 0xffffff); }
    }
    draw_player(0); draw_player(1);
    for (int i = 0; i < 2; i++) {
        Player *p = &pl[i];
        if (p->active && p->alive && on_screen(p->x, p->y, 80))
            rect(WX(p->x + p->ax * 56) - 1, WY(p->y + p->ay * 56) - 1, 3, 3, 0xffffff);
    }
    for (int i = 0; i < MAX_PART; i++) {
        Part *p = &part[i];
        if (p->life > 0 && on_screen(p->x, p->y, 4)) rect(WX(p->x), WY(p->y), 2, 2, p->c);
    }
}

static void draw_hud(int i)
{
    Player *p = &pl[i];
    char buf[32];
    int vx = view_x, vy = view_y;
    text_sh(vx + 8, vy + 8, 2, i ? 0xffb066 : 0x6fa8ff, i ? "P2" : "P1");
    for (int k = 0; k < MAX_HP; k++) {
        uint32_t c = k < p->hp ? (p->hp > 6 ? 0x3ddc5a : (p->hp > 3 ? 0xf0c030 : 0xe03030)) : 0x2a2a2a;
        rect(vx + 8 + k * 11, vy + 28, 9, 8, c);
    }
    snprintf(buf, sizeof buf, "PTS %d", p->score);
    text_sh(vx + 8, vy + 42, 2, 0xffffff, buf);
    if (p->rapid > 0) text_sh(vx + 8, vy + 58, 2, 0xffe040, "TIRO RAPIDO");
    if (!p->alive) {
        Player *o = &pl[1 - i];
        if (o->active && o->alive) snprintf(buf, sizeof buf, "REVIVE EM %d", p->respawn / 60 + 1);
        else snprintf(buf, sizeof buf, "ELIMINADO");
        text_c(vx, view_w, view_y + view_h / 2 - 20, 3, 0xff6060, buf);
    }
}

static void draw_view(int vx, int vy, int vw, int vh, int camx, int camy, int who)
{
    view_x = vx; view_y = vy; view_w = vw; view_h = vh; view_cx = camx; view_cy = camy;
    set_clip(vx, vy, vw, vh);
    rect(vx, vy, vw, vh, C_GRASS);
    draw_tiles();
    if (who < 0) return;
    draw_entities();
    draw_hud(who);
}

static void cam_for(int who, int vw, int vh, int *cx, int *cy)
{
    int t = who;
    if (!pl[t].alive && pl[1 - t].active && pl[1 - t].alive) t = 1 - t;
    int x = (int)(pl[t].x - vw * 0.5f), y = (int)(pl[t].y - vh * 0.5f);
    if (x > WORLD_W - vw) x = WORLD_W - vw;
    if (y > WORLD_H - vh) y = WORLD_H - vh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    *cx = x; *cy = y;
}

static void draw_minimap(void)
{
    int mx = g_sw / 2 - 64, my = g_sh - 76;
    rect(mx - 2, my - 2, 132, 68, 0x9a9aa8);
    rect(mx, my, 128, 64, 0x1d3b17);
    for (int i = 0; i < nmrun; i++)
        rect(mx + mrun[i].x * 2, my + mrun[i].y * 2, mrun[i].w * 2, 2, mrun[i].t == 2 ? 0x8a8a9a : 0x2a5fb0);
    for (int i = 0; i < MAX_ENEMY; i++)
        if (en[i].alive) rect(mx + (int)(en[i].x * 128 / WORLD_W) - 1, my + (int)(en[i].y * 64 / WORLD_H) - 1, 2, 2, 0xff3030);
    for (int i = 0; i < 2; i++)
        if (pl[i].active && pl[i].alive)
            rect(mx + (int)(pl[i].x * 128 / WORLD_W) - 2, my + (int)(pl[i].y * 64 / WORLD_H) - 2, 4, 4, i ? 0xff9a2e : 0x2e7bff);
}

static void panel(int w, int h)
{
    int x = (g_sw - w) / 2, y = (g_sh - h) / 2;
    rect(x - 4, y - 4, w + 8, h + 8, 0xdddddd);
    rect(x, y, w, h, 0x101018);
}

static void draw_menu(void)
{
    int camx = (int)(WORLD_W / 2 - g_sw / 2 + sinf(frame * 0.003f) * 900.0f);
    int camy = (int)(WORLD_H / 2 - g_sh / 2 + cosf(frame * 0.002f) * 500.0f);
    draw_view(0, 0, g_sw, g_sh, camx, camy, -1);
    int pw = 460, ph = 340, px = (g_sw - pw) / 2, py = (g_sh - ph) / 2;
    panel(pw, ph);
    text_c(px, pw, py + 20, 6, 0xffd040, "ZONA DE TIRO");
    text_c(px, pw, py + 66, 2, 0x9a9aaa, "MAPA AMPLO - ATE 2 JOGADORES");
    text_c(px, pw, py + 110, 4, menu_sel == 0 ? 0xffffff : 0x707080, "1 JOGADOR");
    text_c(px, pw, py + 146, 4, menu_sel == 1 ? 0xffffff : 0x707080, "2 JOGADORES");
    text(px + 70, py + 110 + menu_sel * 36, 4, 0xffd040, ">");
    text_c(px, pw, py + 196, 2, 0x6fa8ff, "ANALOGICO ESQ: MOVER");
    text_c(px, pw, py + 214, 2, 0x6fa8ff, "ANALOGICO DIR: MIRAR");
    text_c(px, pw, py + 232, 2, 0x6fa8ff, "R1 / X / QUADRADO: ATIRAR");
    text_c(px, pw, py + 250, 2, 0x9a9aaa, "START: PAUSA   SELECT: SAIR");
    text_c(px, pw, py + 282, 3, 0xffffff, ((frame >> 5) & 1) ? "APERTE START" : "");
    if (menu_msg > 0) text_c(px, pw, py + 312, 2, 0xff6060, "CONECTE O CONTROLE NA PORTA 2!");
}

void game_draw(void)
{
    if (state == ST_MENU) { draw_menu(); return; }

    int np = pl[0].active + pl[1].active, cx, cy;
    if (np == 2) {
        int hw = g_sw / 2;
        cam_for(0, hw, g_sh, &cx, &cy);       draw_view(0, 0, hw, g_sh, cx, cy, 0);
        cam_for(1, g_sw - hw, g_sh, &cx, &cy); draw_view(hw, 0, g_sw - hw, g_sh, cx, cy, 1);
        set_clip(0, 0, g_sw, g_sh);
        rect(hw - 2, 0, 4, g_sh, 0x000000);
    } else {
        int who = pl[0].active ? 0 : 1;
        cam_for(who, g_sw, g_sh, &cx, &cy);
        draw_view(0, 0, g_sw, g_sh, cx, cy, who);
        set_clip(0, 0, g_sw, g_sh);
    }

    char buf[48];
    int left = enemies_alive() + q_grunt + q_shoot + q_brute;
    snprintf(buf, sizeof buf, "ONDA %d  INIMIGOS %d", wave, left);
    int tw = text_w(buf, 2);
    rect(g_sw / 2 - tw / 2 - 6, 4, tw + 12, 20, 0x000000);
    text(g_sw / 2 - tw / 2, 9, 2, 0xffffff, buf);
    draw_minimap();

    if (banner > 0 && wave > 0 && state == ST_PLAY) {
        snprintf(buf, sizeof buf, "ONDA %d", wave);
        text_c(0, g_sw, g_sh / 4, 6, 0xffd040, buf);
    }
    if (state == ST_PAUSE) {
        panel(300, 120);
        text_c(0, g_sw, g_sh / 2 - 44, 5, 0xffffff, "PAUSADO");
        text_c(0, g_sw, g_sh / 2 + 8, 2, 0x9a9aaa, "START: CONTINUAR");
        text_c(0, g_sw, g_sh / 2 + 28, 2, 0x9a9aaa, "SELECT: MENU");
    } else if (state == ST_OVER) {
        panel(360, 200);
        text_c(0, g_sw, g_sh / 2 - 84, 5, 0xff5050, "FIM DE JOGO");
        snprintf(buf, sizeof buf, "CHEGOU NA ONDA %d", wave);
        text_c(0, g_sw, g_sh / 2 - 34, 2, 0xffffff, buf);
        for (int i = 0; i < 2; i++) if (pl[i].active) {
            snprintf(buf, sizeof buf, "P%d  PTS %d", i + 1, pl[i].score);
            text_c(0, g_sw, g_sh / 2 - 8 + i * 22, 3, i ? 0xffb066 : 0x6fa8ff, buf);
        }
        if (over_t > 60) text_c(0, g_sw, g_sh / 2 + 62, 2, 0x9a9aaa, "START: MENU");
    }
}
