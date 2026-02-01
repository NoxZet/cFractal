#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "mandelbrot.h"

void calculate(
    fracInt *target, int maxIters,
    double centerX, double centerY,
    double pixelStep,
    int width, int height,
    int yStart, int yEnd,
    int r1xStart, int r1xEnd,
    bool region2, int r2xStart, int r2xEnd
) {
    int left   = -(int)floor((float)width / 2);
    int right  = (int)ceil((float)width / 2);
    int top    = -(int)floor((float)height / 2);
    int bottom = (int)ceil((float)height / 2);

    if (!region2) {
        r2xEnd = r1xEnd;
    }
    int region2Jump = r2xStart - r1xEnd;

    fracInt *iter = target + yStart * width;
    for (
        int iy = top + yStart, py = yStart;
        iy < bottom && py < yEnd;
        iy++, py++
    ) {
        double y = centerY + pixelStep * iy;

        for (
            int ix = left + r1xStart, px = r1xStart;
            ix < right && px < r2xEnd;
            ix++, px++
        ) {
            if (px == r1xEnd) {
                ix += region2Jump;
                px += region2Jump;
            }
            double x = centerX + pixelStep * ix;
            // Real and Imaginary components
            double cr = 0;
            double ci = 0;
            fracInt iters = 0;
            while (cr < 4 && cr > -4 && ci < 4 && ci > -4 && iters < maxIters) {
                iters++;
                double newCr = cr * cr - ci * ci + x;
                ci = 2 * cr * ci + y;
                cr = newCr;
            }
            //asm("# hello asm");
            *(iter + px) = iters;
        }
        iter += width;
    }
}
