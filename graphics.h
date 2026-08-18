#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <pspgu.h>

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

void initGraphics();
void startFrame();
void endFrame_noSwap();
void swapBuffers();
void endFrame();
void clearScreen(u32 color);

#endif
