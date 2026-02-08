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
    short hstriping, short hstripeOffset, bool hfillIn,
    short vstriping, short vstripeOffset, bool vfillIn,
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
    
    int yInc = 1, xInc = 1;
    short yStripeOffset = 0, r1xStripeOffset = 0, r2xStripeOffset = 0;
    // Align starts with striping and offset (move right by as little as possible)
    if (vstriping >= 2) {
        yInc = vstriping;
        yStripeOffset = (10000 * vstriping - (top + yStart) + vstripeOffset) % vstriping;
    }
    if (hstriping >= 2) {
        xInc = hstriping;
        r1xStripeOffset = (10000 * hstriping - (left + r1xStart) + hstripeOffset) % hstriping;
        if (region2) {
            r2xStripeOffset = (10000 * hstriping - (left + r2xStart) + hstripeOffset) % hstriping;
            // We are incrementing from r1xStart by hstriping, therefore once the iterator reaches r1xEnd,
            // it will be at r1xEnd aligned by striping. We can align it ourselves for ease of calculating jump.
            int r1xStripeEndOffset = (10000 * hstriping - (left + r1xEnd) + hstripeOffset) % hstriping;
            region2Jump = r2xStart + r2xStripeOffset - (r1xEnd + r1xStripeEndOffset);
        }
    }

    fracInt *iter = target + (yStart + yStripeOffset) * width;
    size_t rowStep = width * yInc;
    for (
        int iy = top + yStart + yStripeOffset, py = yStart + yStripeOffset, row = 0;
        iy < bottom && py < yEnd;
        iy += yInc, py += yInc, row++
    ) {
        double y = centerY + pixelStep * iy;

        for (
            int ix = left + r1xStart + r1xStripeOffset, px = r1xStart + r1xStripeOffset, col = 0;
            ix < right && px < r2xEnd;
            ix += xInc, px += xInc, col++
        ) {
            // If region2 is disabled, r1xEnd = r2xEnd
            if (px >= r1xEnd && px < r2xStart) {
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
            *(iter + px) = iters;
            if (hfillIn) {
                // Fill left
                if (col == 0 && r1xStripeOffset > 0) {
                    for (int filli = 1; filli <= r1xStripeOffset; filli++)
                        *(iter + px - filli) = iters;
                } else if (region2 && r2xStripeOffset > 0 && px == r2xStart + r2xStripeOffset) {
                    for (int filli = 1; filli <= r2xStripeOffset; filli++)
                        *(iter + px - filli) = iters;
                }
                // Fill right
                int boundary = px < r1xEnd ? r1xEnd : r2xEnd;
                for (int destPx = px + 1, destCol = 1; destPx < boundary && destCol < hstriping; destPx++, destCol++)
                    *(iter + destPx) = iters;
            }
        }

        // Fill in skipped stripes
        if (vfillIn) {
            size_t r1rowLength = (r1xEnd - r1xStart) * sizeof(fracInt);
            size_t r2rowLength = (r2xEnd - r2xStart) * sizeof(fracInt);
            // Fill above
            if (row == 0 && yStripeOffset > 0) {
                for (int filli = 1; filli <= yStripeOffset; filli++) {
                    memcpy(iter - filli * width + r1xStart, iter + r1xStart, r1rowLength);
                    if (region2) memcpy(iter - filli * width + r2xStart, iter + r2xStart, r2rowLength);
                }
            }
            // Fill below
            fracInt *destIter = iter;
            for (
                int destPy = py + 1, destRow = 1;
                destPy < yEnd && destRow < vstriping;
                destPy++, destRow++
            ) {
                destIter += width;
                memcpy(destIter + r1xStart, iter + r1xStart, r1rowLength);
                if (region2) memcpy(destIter + r2xStart, iter + r2xStart, r2rowLength);
            }
        }

        iter += rowStep;
    }
}
