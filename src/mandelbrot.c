#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "mandelbrot.h"

void calculate(
    double centerX, double centerY,
    double pixelStep,
    int width, int height,
    int maxIters, fracInt *array,
    int xStart, int xEnd, int yStart, int yEnd
) {
    int left   = -(int)floor((float)width / 2);
    int right  = (int)ceil((float)width / 2);
    int top    = -(int)floor((float)height / 2);
    int bottom = (int)ceil((float)height / 2);

    fracInt *iter = array + yStart * width;
    for (
        int iy = top + yStart, py = yStart;
        iy < bottom && py < yEnd;
        iy++, py++
    ) {
        double y = centerY + pixelStep * iy;

        for (
            int ix = left + xStart, px = xStart;
            ix < right && px < xEnd;
            ix++, px++
        ) {
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
