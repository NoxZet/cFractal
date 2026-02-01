#include <stdint.h>
#include <stdbool.h>

typedef uint16_t fracInt;

void calculate(
    fracInt *target, int maxIters,
    double centerX, double centerY,
    double pixelStep,
    int width, int height,
    int yStart, int yEnd,
    int r1xStart, int r1xEnd,
    bool region2, int r2xStart, int r2xEnd
);