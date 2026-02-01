#include <stdbool.h>

int rendererInitialize();
void rendererExit();
bool tryRedraw32(uint32_t *pixels, int width, int height);
void resizeFrame(int width, int height);
void panFrame(int xPixels, int yPixels);
void zoomFrame(int xPixel, int yPixel, int level);
