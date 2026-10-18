/*
 * ZONA DE TIRO - camada de plataforma PS2 (gsKit + libpad)
 * Compila com o PS2SDK (ps2dev) e roda como .elf no uLaunchELF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <libpad.h>
#include <gsKit.h>
#include <dmaKit.h>
#include "game.h"

static GSGLOBAL *gs;

/* ---------------- video ---------------- */
void plat_rect(int x, int y, int w, int h, uint32_t rgb)
{
    gsKit_prim_sprite(gs, (float)x, (float)y, (float)(x + w), (float)(y + h), 0,
                      GS_SETREG_RGBAQ((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0x80, 0x00));
}

static void video_init(void)
{
    gs = gsKit_init_global();            /* detecta NTSC (640x448) ou PAL (640x512) */
    gs->PSM = GS_PSM_CT24;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
}

/* ---------------- IOP / controles ---------------- */
static void reset_iop(void)
{
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);
}

static char padbuf[2][256] __attribute__((aligned(64)));
static int pad_mode_set[2];
static Input last_in[2];

static void pad_start(void)
{
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, padbuf[0]);
    padPortOpen(1, 0, padbuf[1]);
}

static void pad_poll(int port, Input *out)
{
    int state = padGetState(port, 0);

    if (state == PAD_STATE_DISCONN) {
        memset(&last_in[port], 0, sizeof(Input));
        pad_mode_set[port] = 0;
        *out = last_in[port];
        return;
    }
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
        *out = last_in[port];            /* estado transitorio: mantem a ultima leitura */
        return;
    }
    if (!pad_mode_set[port]) {           /* tenta ligar o modo analogico (DualShock) uma vez */
        int n = padInfoMode(port, 0, PAD_MODETABLE, -1);
        pad_mode_set[port] = 1;
        for (int i = 0; i < n; i++) {
            if (padInfoMode(port, 0, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK) {
                padSetMainMode(port, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
                break;
            }
        }
        last_in[port].connected = 1;
        *out = last_in[port];
        return;
    }

    struct padButtonStatus b;
    if (!padRead(port, 0, &b)) { *out = last_in[port]; return; }

    u32 p = 0xFFFF ^ b.btns;
    Input in;
    memset(&in, 0, sizeof in);
    in.connected = 1;
    if (p & PAD_UP)    in.buttons |= IN_UP;
    if (p & PAD_DOWN)  in.buttons |= IN_DOWN;
    if (p & PAD_LEFT)  in.buttons |= IN_LEFT;
    if (p & PAD_RIGHT) in.buttons |= IN_RIGHT;
    if (p & (PAD_CROSS | PAD_SQUARE | PAD_R1 | PAD_R2)) in.buttons |= IN_FIRE;
    if (p & PAD_START)  in.buttons |= IN_START;
    if (p & PAD_SELECT) in.buttons |= IN_SELECT;

    if (padInfoMode(port, 0, PAD_MODECURID, 0) == PAD_TYPE_DUALSHOCK) {
        in.lx = (int)b.ljoy_h - 128; in.ly = (int)b.ljoy_v - 128;
        in.rx = (int)b.rjoy_h - 128; in.ry = (int)b.rjoy_v - 128;
    }
    last_in[port] = in;
    *out = in;
}

/* ---------------- main ---------------- */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    Input in[2];
    memset(in, 0, sizeof in);

    reset_iop();
    pad_start();
    video_init();
    game_init(gs->Width, gs->Height);

    while (!game_wants_quit()) {
        pad_poll(0, &in[0]);
        pad_poll(1, &in[1]);
        game_update(in);

        gsKit_clear(gs, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
        game_draw();
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }

    padPortClose(0, 0);
    padPortClose(1, 0);
    padEnd();
    return 0;     /* volta para o browser do PS2 (abra o uLaunchELF de novo) */
}
