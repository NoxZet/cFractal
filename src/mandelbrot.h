#include <stdint.h>
#include <stdbool.h>

typedef uint16_t fracInt;

/**
 * @param hstriping 0 = disabled, >1 = number of steps
 * @param hstripeOffset 0 = render at center, 1 = render one right of center, ...
 * @param hfillIn Copy rendered stripe into nonrendered
 * @param p1xStart left side of the first region to render
 * @param p1xEnd Right side of the first region to render
 * @param region2 Enable rendering of the second region
 */
void calculate(
    fracInt *target, int maxIters,
    double centerX, double centerY,
    double pixelStep,
    int width, int height,
    short hstriping, short hstripeOffset, bool hfillIn,
    short vstriping, short vstripeOffset, bool vfillIn,
    int yStart, int yEnd,
    int r1xStart, int r1xEnd,
    bool region2, int r2xStart, int r2xEnd
);