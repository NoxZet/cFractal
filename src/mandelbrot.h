#include <stdint.h>

typedef uint16_t fracInt;

void calculate(
    double centerX, double centerY,
    double pixelStep,
    int width, int height,
    int maxIters, fracInt *array,
    int xStart, int xEnd, int yStart, int yEnd
);