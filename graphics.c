#include <pspge.h>
#include <pspdisplay.h>
#include <psputils.h>
#include "graphics.h"

static unsigned int __attribute__((aligned(16))) list[262144];

void initGraphics() {
    sceGuInit();

    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void*)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void*)0x88000, BUF_WIDTH);
    sceGuDepthBuffer((void*)0x110000, BUF_WIDTH);

    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    
    sceGuClearColor(0xff000000); // Black background
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0,0);
    
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void startFrame() {
    sceGuStart(GU_DIRECT, list);
}

void endFrame() {
    sceGuFinish();
    sceGuSync(0,0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

void clearScreen(u32 color) {
    sceGuClearColor(color);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}
