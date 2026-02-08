#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#include "util.h"
#include "renderer.h"
#include "mandelbrot.h"

// 1 = show initialization and exit details
// 2 = show calculate operation starts and ends + swap
#define DEBUG_THREAD 2
#define DEBUG_STRIPING 0
#define DEBUG_PANNING 0
#define DEBUG_BUFFER_SEMAPHORE 0
#define DEBUG_WORKER 0
#define DEBUG_REDRAW 0
#define DEBUG_TIME 1

#define MAX_THREADS 16
#define MAX_QUEUE 100

// User params
volatile int desiredWidth = 622;
volatile int desiredHeight = 433;
// volatile double desiredZoom = 0.2;
// volatile double desiredOffsetX = -0.6;
// volatile double desiredOffsetY = 0;
volatile double desiredZoom = 0.01;
volatile double desiredOffsetX = -0.74;
volatile double desiredOffsetY = -0.22;

typedef struct {
    int width;
    int height;
    double pixelStep;
    double offsetX;
    double offsetY;
} DesiredParams;

#define STRIPING 3
#define striping_row_started(arr, y) ((arr)[y][0] || (arr)[y][1] || (arr)[y][2])
#define striping_row_done(arr, y) ((arr)[y][0] && (arr)[y][1] && (arr)[y][2])
#define striping_done(arr) (striping_row_done(arr, 0) && striping_row_done(arr, 1) && striping_row_done(arr, 2))

typedef struct {
    int tag;
    fracInt *array;
    /** Currently being calculated on */
    int wip;
    /** When calculate thread finishes output, it may have been panned since then,
        in which case pan thread adjusts it before it displays. */
    bool freshlyCalculated;
    /** For what desired user params it was rendered */
    DesiredParams params;
    /** How many pixels are missing in that direction */
    int missingL; int missingR; int missingT; int missingB;
    /** Current striping status - also modified when panning */
    bool stripeProgress[STRIPING][STRIPING];
    /** How many microseconds the first row took to determine how striped the other stripes should be */
    int rowMicros;
} BufferArray;

volatile BufferArray mainBuffer = { 0 };
volatile BufferArray swapBuffer = { 0 };

// Threading
HANDLE statusSemaphore = 0;
HANDLE bufferSemaphore = 0;
HANDLE taskSemaphore = 0;

volatile bool threadsRunning = false;

HANDLE panThreadPointer = 0;
unsigned __stdcall PanThreadFunction( void* pArguments );

HANDLE calculateThreadPointer = 0;
unsigned __stdcall CalculateThreadFunction( void* pArguments );

unsigned int workerThreadCount = 0;
HANDLE workerThreadPointers[MAX_THREADS] = { 0 };
unsigned __stdcall WorkerThreadFunction( void* pArguments );

typedef struct {
    bool taken;
    fracInt *target; int maxIters;
    double centerX; double centerY;
    double pixelStep;
    int width; int height;
    short hstriping; short hstripeOffset; bool hfillIn;
    short vstriping; short vstripeOffset; bool vfillIn;
    int yStart; int yEnd;
    int r1xStart; int r1xEnd;
    bool region2; int r2xStart; int r2xEnd;
} WorkerTask;

volatile WorkerTask taskQueue[MAX_QUEUE] = { 0 };
volatile int tasksTotal = 0;
volatile int tasksLeft = 0;

// Fractal specific stuff
int *palette = 0;
const int maxIters = 1000;

DWORD waitForBufferSemaphore(DWORD ms, char label) {
    DWORD result = WaitForSingleObject(bufferSemaphore, ms);
    if (DEBUG_BUFFER_SEMAPHORE)
        printf("%c Waited for bufferSemaphore: %d (%c)\n", label, result, result == 0 ? 's' : 'f');
    return result;
}
DWORD releaseBufferSemaphore(char label) {
    long count = -1;
    BOOL result = ReleaseSemaphore(bufferSemaphore, 1, &count);
    if (DEBUG_BUFFER_SEMAPHORE)
        printf("%c Released bufferSemaphore: (%c) times %d\n", label, result != 0 ? 's' : 'f', count);
    return result;
}

int rendererInitialize(unsigned int inThreadCount) {
    palette = calloc(sizeof(int), (maxIters + 1) * 4);
    for (int i = 0; i < 20; i++) {
        palette[i * 4] = (i + 15) * 2;
        palette[i * 4 + 1] = (i + 15) * 3;
        palette[i * 4 + 2] = (i + 15) * 7;
    }
    for (int i = 20; i < 259; i++) {
        palette[i * 4] = (19 + 15) * 2;
        palette[i * 4 + 1] = (i + 15) * 3 - (i - 20) * 0.65;
        palette[i * 4 + 2] = 258 - i;
    }

    statusSemaphore = CreateSemaphore(NULL, 1, 1, NULL);
    if (!statusSemaphore) return 1;
    bufferSemaphore = CreateSemaphore(NULL, 1, 1, NULL);
    if (!bufferSemaphore) return 1;
    taskSemaphore = CreateSemaphore(NULL, 1, 1, NULL);
    if (!taskSemaphore) return 1;

    workerThreadCount = min(16, max(1, inThreadCount));
    threadsRunning = true;
    panThreadPointer = (HANDLE)_beginthreadex(NULL, 0, PanThreadFunction, NULL, 0, NULL);
    if (!panThreadPointer) return 1;
    calculateThreadPointer = (HANDLE)_beginthreadex(NULL, 0, CalculateThreadFunction, NULL, 0, NULL);
    if (!calculateThreadPointer) return 1;

    if (DEBUG_THREAD) printf("Starting %d worker threads\n", workerThreadCount);
    for (int i = 0; i < workerThreadCount; i++) {
        workerThreadPointers[i] = (HANDLE)_beginthreadex(NULL, 0, WorkerThreadFunction, (void*)(uintptr_t)i, 0, NULL);
        if (!workerThreadPointers[i]) return 1;
    }

    return 0;
}

void rendererExit() {
    threadsRunning = false;
    if (DEBUG_THREAD) printf("Exit awaiting threads\n");
    if (panThreadPointer) {
        WaitForSingleObject(panThreadPointer, INFINITE);
    }
    if (calculateThreadPointer) {
        WaitForSingleObject(calculateThreadPointer, INFINITE);
    }
    if (calculateThreadPointer) {
        WaitForMultipleObjects(workerThreadCount, workerThreadPointers, true, INFINITE);
    }
    if (DEBUG_THREAD) printf("Wait on buffer\n");
    if (bufferSemaphore) {
        waitForBufferSemaphore(INFINITE, 'E');
    }
    if (DEBUG_THREAD) printf("Freeing buffer\n");
    if (palette) free(palette);
    if (mainBuffer.array) free(mainBuffer.array);
    if (swapBuffer.array) free(swapBuffer.array);
    if (DEBUG_THREAD) printf("rendererExit finished\n");
}

/** Only use with status semaphore */
double getCurrentPixelStep() {
    return desiredZoom * 2 / min(desiredWidth, desiredHeight);
}

DesiredParams getCurrentDesired() {
    return (DesiredParams){
        desiredWidth, desiredHeight,
        getCurrentPixelStep(),
        desiredOffsetX, desiredOffsetY,
    };
}

/**
 * Does simple panning of the mainBuffer and retrieves swapBuffer when ready
 */
unsigned __stdcall PanThreadFunction( void* pArguments ) {
    int currentTag = 0;
    while (threadsRunning) {
        Sleep(3);
        if (WaitForSingleObject(statusSemaphore, 1000) != 0) continue;
        DesiredParams target = getCurrentDesired();
        ReleaseSemaphore(statusSemaphore, 1, NULL);

        if (waitForBufferSemaphore(3, 'P') != 0) {
            continue;
        }
        
        // swapBuffer processed and ready
        if (swapBuffer.wip == 0) {
            MemoryBarrier();
            if (swapBuffer.freshlyCalculated) {
                if (DEBUG_THREAD >= 2) printf("Swap!!\n");
                currentTag++;

                BufferArray oldSwap = swapBuffer;
                swapBuffer = mainBuffer;
                mainBuffer = oldSwap;
                if (DEBUG_STRIPING >= 2) printf("Progress on main after swap: {%c%c%c}{%c%c%c}{%c%c%c}\n",
                    mainBuffer.stripeProgress[0][0]?'-':' ', mainBuffer.stripeProgress[0][1]?'-':' ', mainBuffer.stripeProgress[0][2]?'-':' ',
                    mainBuffer.stripeProgress[1][0]?'-':' ', mainBuffer.stripeProgress[1][1]?'-':' ', mainBuffer.stripeProgress[1][2]?'-':' ',
                    mainBuffer.stripeProgress[2][0]?'-':' ', mainBuffer.stripeProgress[2][1]?'-':' ', mainBuffer.stripeProgress[2][2]?'-':' ');
                mainBuffer.freshlyCalculated = false;
                mainBuffer.tag = currentTag;
            }
        }
        if (mainBuffer.array) {
            // Pan mainBuffer
            if (target.offsetX != mainBuffer.params.offsetX || target.offsetY != mainBuffer.params.offsetY) {
                currentTag++;

                int shiftX = (int)round((mainBuffer.params.offsetX - target.offsetX) / target.pixelStep);
                int shiftY = (int)round((mainBuffer.params.offsetY - target.offsetY) / target.pixelStep);
                if (shiftX > 0) {
                    mainBuffer.missingL += shiftX;
                } else if (shiftX < 0) {
                    mainBuffer.missingR += -shiftX;
                }
                if (shiftY > 0) {
                    mainBuffer.missingT += shiftY;
                } else if (shiftY < 0) {
                    mainBuffer.missingB += -shiftY;
                }
                mainBuffer.params.offsetX = target.offsetX;
                mainBuffer.params.offsetY = target.offsetY;
                mainBuffer.tag = currentTag;

                // Shift stripe progress
                if (!striping_done(mainBuffer.stripeProgress)) {
                    int cols = (shiftX + STRIPING * 10000) % STRIPING;
                    int rows = (shiftY + STRIPING * 10000) % STRIPING;
                    bool *progress = (bool*)mainBuffer.stripeProgress;
                    if (cols != 0) {
                        bool tmp[STRIPING - 1];
                        for (size_t y = 0; y < STRIPING; y++) {
                            bool *progress = (bool*)mainBuffer.stripeProgress;
                            memcpy(tmp, progress + STRIPING - cols, cols * sizeof(bool));
                            memmove(progress + cols, progress, (STRIPING - cols) * sizeof(bool));
                            memcpy(progress, tmp, cols * sizeof(bool));
                        }
                    }
                    if (rows != 0) {
                        bool tmp[STRIPING - 1][STRIPING];
                        memcpy(tmp, progress + (STRIPING - rows) * STRIPING, rows * STRIPING * sizeof(bool));
                        memmove(progress + rows * STRIPING, progress, (STRIPING - rows) * STRIPING * sizeof(bool));
                        memcpy(progress, tmp, rows * STRIPING * sizeof(bool));
                    }
                }
                
                // Pan the buffer array (move the content data based on pan)
                if (!(
                    mainBuffer.missingL >= mainBuffer.params.width || mainBuffer.missingR >= mainBuffer.params.width
                    || mainBuffer.missingT >= mainBuffer.params.height || mainBuffer.missingB >= mainBuffer.params.height
                )) {
                    if (DEBUG_PANNING) printf("Panning by x: %d; y: %d!!\n", shiftX, shiftY);
                    int width = mainBuffer.params.width;
                    int rowLength = width - abs(shiftX);
                    int sourceX = shiftX > 0 ? 0 : -shiftX;
                    int targetX = shiftX > 0 ? shiftX : 0;
                    if (shiftY >= 0) {
                        int targetY = mainBuffer.params.height - 1;
                        int sourceY = targetY - shiftY;
                        for (; sourceY >= 0; sourceY--, targetY--) {
                            memmove(
                                mainBuffer.array + (targetY * width + targetX),
                                mainBuffer.array + (sourceY * width + sourceX),
                                rowLength * sizeof(fracInt)
                            );
                        }
                    } else {
                        int targetY = 0;
                        int sourceY = -shiftY;
                        for (; sourceY < mainBuffer.params.height; sourceY++, targetY++) {
                            memmove(
                                mainBuffer.array + (targetY * width + targetX),
                                mainBuffer.array + (sourceY * width + sourceX),
                                rowLength * sizeof(fracInt)
                            );
                        }
                    }
                }
            }
        }
        releaseBufferSemaphore('P');
    }
    if (DEBUG_THREAD) printf("Finishing PanThreadFunction\n");
    return 0;
}

void reallocSwapBuffer(int width, int height) {
    swapBuffer.array = realloc(swapBuffer.array, width * height * sizeof(fracInt));
    swapBuffer.params.width = width;
    swapBuffer.params.height = height;
}

/**
 * Does fractal calculations in swapBuffer
 */
unsigned __stdcall CalculateThreadFunction( void* pArguments ) {
    LARGE_INTEGER perfFrequency, perfStart, perfEnd;
    QueryPerformanceFrequency(&perfFrequency);

    int lastTouchedTag = -1;
    while (threadsRunning) {
        // Get desired user params
        Sleep(3);
        if (WaitForSingleObject(statusSemaphore, 1000) != 0) continue;
        DesiredParams target = getCurrentDesired();
        ReleaseSemaphore(statusSemaphore, 1, NULL);

        if (target.width < 4 && target.height < 4) {
            continue;
        }
        
        // WaitForSingleObject(bufferSemaphore, 5)
        if (waitForBufferSemaphore(5, 'C') != 0) {
            continue;
        }

        // Buffer is still the same, wait for pan thread to acknowledge (and process) the swap
        if (lastTouchedTag == mainBuffer.tag) {
            releaseBufferSemaphore('C');
            continue;
        }

        // Zoom level is different, rerender from scratch
        if (target.pixelStep != mainBuffer.params.pixelStep) {
            lastTouchedTag = mainBuffer.tag;
            if (!swapBuffer.array || swapBuffer.params.width != target.width || swapBuffer.params.height != target.height) {
                reallocSwapBuffer(target.width, target.height);
            }
            fracInt *swapArray = swapBuffer.array;
            MemoryBarrier();
            // While wip is set to > 0, main/pan threads aren't allowed to touch it
            swapBuffer.wip = 1;
            // ReleaseSemaphore(bufferSemaphore, 1, NULL)
            releaseBufferSemaphore('C');

            if (DEBUG_THREAD >= 2) printf("Calculating scale!!\n");

            // Schedule task regions
            if (WaitForSingleObject(taskSemaphore, INFINITE) != 0) {
                fprintf(stderr, "Calculate thread could not acquire task semaphore\n");
                continue;
            }

            tasksTotal = min(MAX_QUEUE, target.height * 3);
            tasksLeft = 0;
            for (int y = 0; y < tasksTotal; y++) {
                int top = (int)round((double)target.height / tasksTotal * y);
                int bottom = (int)round((double)target.height / tasksTotal * (y + 1));
                taskQueue[tasksLeft] = (WorkerTask){false, swapArray, maxIters,
                    target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                    STRIPING, 0, true, STRIPING, 0, true,
                    top, bottom, 0, target.width, false, 0, 0};
                tasksLeft++;
            }
            QueryPerformanceCounter(&perfStart);
            ReleaseSemaphore(taskSemaphore, 1, NULL);

            while (tasksLeft >= 1) {
                Sleep(3);
            }
            
            QueryPerformanceCounter(&perfEnd);
            swapBuffer.rowMicros = (perfEnd.QuadPart * 1000000 - perfStart.QuadPart * 1000000) / perfFrequency.QuadPart;
            if (DEBUG_TIME) {
                printf("Calculating scale took %dms\n", (perfEnd.QuadPart * 1000 - perfStart.QuadPart * 1000) / perfFrequency.QuadPart);
            }

            // Set finalized parameters
            swapBuffer.freshlyCalculated = true;
            swapBuffer.params = target;
            swapBuffer.missingB = swapBuffer.missingT = swapBuffer.missingL = swapBuffer.missingR = 0;
            memset((bool*)swapBuffer.stripeProgress, false, sizeof(swapBuffer.stripeProgress));
            swapBuffer.stripeProgress[0][0] = true;
            MemoryBarrier();
            swapBuffer.wip = 0;
            if (DEBUG_THREAD >= 2) printf("Done!!\n");
        }
        else if (!striping_done(mainBuffer.stripeProgress)) {
            DesiredParams target = mainBuffer.params;
            int missingL = mainBuffer.missingL, missingR = mainBuffer.missingR,
                missingT = mainBuffer.missingT, missingB = mainBuffer.missingB;
            // There is no partial area left, which makes it technically complete
            if (missingL + missingR >= target.width || missingT + missingB >= target.height) {
                memset((bool*)swapBuffer.stripeProgress, true, sizeof(swapBuffer.stripeProgress));
                releaseBufferSemaphore('C');
                continue;
            }

            lastTouchedTag = mainBuffer.tag;
            if (!swapBuffer.array || swapBuffer.params.width != mainBuffer.params.width || swapBuffer.params.height != mainBuffer.params.height) {
                reallocSwapBuffer(mainBuffer.params.width, mainBuffer.params.height);
            }
            memcpy(swapBuffer.array, mainBuffer.array, mainBuffer.params.width * mainBuffer.params.height * sizeof(fracInt));

            // Get relevant data from mainBuffer
            fracInt *swapArray = swapBuffer.array;
            int rowMicros = mainBuffer.rowMicros;
            bool stripeProgress[STRIPING][STRIPING];
            memcpy(stripeProgress, (bool*)mainBuffer.stripeProgress, sizeof(stripeProgress));

            MemoryBarrier();
            swapBuffer.wip = 1;
            releaseBufferSemaphore('C');
            
            if (DEBUG_STRIPING) printf("Calculating scale striping progress!!\n");

            // Schedule task regions
            if (WaitForSingleObject(taskSemaphore, INFINITE) != 0) {
                fprintf(stderr, "Calculate thread could not acquire task semaphore\n");
                continue;
            }

            // Find the correct stripes to do next
            if (DEBUG_STRIPING >= 2) printf("Progress before calculation: {%c%c%c}{%c%c%c}{%c%c%c}\n",
                stripeProgress[0][0]?'-':' ', stripeProgress[0][1]?'-':' ', stripeProgress[0][2]?'-':' ',
                stripeProgress[1][0]?'-':' ', stripeProgress[1][1]?'-':' ', stripeProgress[1][2]?'-':' ',
                stripeProgress[2][0]?'-':' ', stripeProgress[2][1]?'-':' ', stripeProgress[2][2]?'-':' ');
            size_t finishedRowCount = 0;
            size_t rowStarted = 100;
            size_t rowNotStarted = 100;
            for (size_t row = 0; row < STRIPING; row++) {
                if (striping_row_done(stripeProgress, row))
                    finishedRowCount++;
                else if (striping_row_started(stripeProgress, row))
                    rowStarted = row;
                else
                    rowNotStarted = row;
            }

            size_t vstripe = 0;
            size_t hstriping = 0;
            size_t hstripe = 0;
            bool hfillIn = false;
            // Unfinished row, do its last unfinished column
            if (rowStarted != 100) {
                vstripe = rowStarted;
                hstriping = STRIPING;
                for (size_t col = 0; col < STRIPING; col++) {
                    if (!stripeProgress[rowStarted][col])
                        hstripe = col;
                }
                stripeProgress[rowStarted][hstripe] = true;
            }
            // Start/do last unstarted row
            else {
                vstripe = rowNotStarted;
                if (finishedRowCount == 0 || rowMicros >= 80 * 1000) {
                    hstriping = STRIPING;
                    hfillIn = true;
                    stripeProgress[vstripe][0] = true;
                } else {
                    for (size_t col = 0; col < STRIPING; col++)
                        stripeProgress[vstripe][col] = true;
                }
            }
            
            // memset(stripeProgress, true, sizeof(stripeProgress));

            int height = target.height - missingB - missingT;
            tasksTotal = min(MAX_QUEUE, height * 3);
            int padding = missingT;
            tasksLeft = 0;
            for (int y = 0; y < tasksTotal; y++) {
                int top = padding + (int)round((double)height / tasksTotal * y);
                int bottom = padding + (int)round((double)height / tasksTotal * (y + 1));
                taskQueue[tasksLeft] = (WorkerTask){false, swapArray, maxIters,
                    target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                    hstriping, hstripe, hfillIn, STRIPING, vstripe, false,
                    top, bottom, missingL, target.width - missingR, false, 0, 0};
                tasksLeft++;
            }

            if (DEBUG_STRIPING >= 2) printf("Calculating for yoff=%d; xoff=%d/%d fill:%c\n",
                vstripe, hstripe, hstriping, hfillIn ? 'Y' : 'N');
            QueryPerformanceCounter(&perfStart);
            ReleaseSemaphore(taskSemaphore, 1, NULL);

            while (tasksLeft >= 1) {
                Sleep(3);
            }
            
            QueryPerformanceCounter(&perfEnd);
            if (finishedRowCount == 0)
                swapBuffer.rowMicros += (perfEnd.QuadPart * 1000000 - perfStart.QuadPart * 1000000) / perfFrequency.QuadPart;
            if (DEBUG_TIME) {
                printf("Calculating scale striping progress took %dms\n", (perfEnd.QuadPart * 1000 - perfStart.QuadPart * 1000) / perfFrequency.QuadPart);
            }

            // Set finalized parameters
            swapBuffer.freshlyCalculated = true;
            swapBuffer.params = target;
            swapBuffer.missingB = missingB; swapBuffer.missingT = missingT;
            swapBuffer.missingL = missingL; swapBuffer.missingR = missingR;
            memcpy((bool*)swapBuffer.stripeProgress, stripeProgress, sizeof(stripeProgress));
            MemoryBarrier();
            swapBuffer.wip = 0;
            if (DEBUG_STRIPING) printf("Done!!\n");
        }
        else if (mainBuffer.missingL || mainBuffer.missingR || mainBuffer.missingT || mainBuffer.missingB) {
            if (!swapBuffer.array || swapBuffer.params.width != mainBuffer.params.width || swapBuffer.params.height != mainBuffer.params.height) {
                reallocSwapBuffer(mainBuffer.params.width, mainBuffer.params.height);
            }
            memcpy(swapBuffer.array, mainBuffer.array, mainBuffer.params.width * mainBuffer.params.height * sizeof(fracInt));

            // Get relevant data from mainBuffer
            fracInt *swapArray = swapBuffer.array;
            DesiredParams target = mainBuffer.params;
            int missingL = mainBuffer.missingL, missingR = mainBuffer.missingR,
                missingT = mainBuffer.missingT, missingB = mainBuffer.missingB;
            if (missingL + missingR >= target.width || missingT + missingB >= target.height) {
                missingL = target.width;
                missingR = missingT = missingB = 0;
            }
            swapBuffer.wip = 1;
            releaseBufferSemaphore('C');
            
            if (DEBUG_THREAD >= 2) printf("Calculating move!!\n");

            // Schedule task regions
            if (WaitForSingleObject(taskSemaphore, INFINITE) != 0) {
                fprintf(stderr, "Calculate thread could not acquire task semaphore\n");
                continue;
            }

            int newArea = target.width * target.height
                - (target.width - missingL - missingR) * (target.height - missingT - missingB);
            tasksTotal = max(3, min(MAX_QUEUE, max(workerThreadCount, newArea / 10000)));
            tasksLeft = 0;

            int fullWidthTasks = 0;
            if (missingT || missingB) {
                // Use tasks out of the total pool based on proportional share of area
                int fullWidthArea = target.width * (missingT + missingB);
                fullWidthTasks = max(2, min(tasksTotal - 1, tasksTotal * fullWidthArea / newArea));
                if (!missingL && !missingR) {
                    fullWidthTasks = tasksTotal;
                }
                int topTasks = !missingT ? 0
                             : !missingB ? fullWidthTasks
                             : min(fullWidthTasks - 1, (1, (fullWidthTasks * missingT * target.width / fullWidthArea)));
                int bottomTasks = fullWidthTasks - topTasks;

                for (int y = 0; y < topTasks; y++) {
                    int top = (int)round((double)missingT / topTasks * y);
                    int bottom = (int)round((double)missingT / topTasks * (y + 1));
                    if (bottom - top == 0) continue;
                    taskQueue[tasksLeft] = (WorkerTask){false, swapArray, maxIters,
                        target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                        0, 0, false, 0, 0, false,
                        top, bottom, 0, target.width, false, 0, 0};
                    tasksLeft++;
                }
                int paddingB = target.height - missingB;
                for (int y = 0; y < bottomTasks; y++) {
                    int top = paddingB + (int)round((double)missingB / bottomTasks * y);
                    int bottom = paddingB + (int)round((double)missingB / bottomTasks * (y + 1));
                    if (bottom - top == 0) continue;
                    taskQueue[tasksLeft] = (WorkerTask){false, swapArray, maxIters,
                        target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                        0, 0, false, 0, 0, false,
                        top, bottom, 0, target.width, false, 0, 0};
                    tasksLeft++;
                }
            }

            if (missingL || missingR) {
                int sideTasks = tasksTotal - fullWidthTasks;
                int r1xStart, r1xEnd, r2xStart = 0, r2xEnd = 0;
                bool region2 = false;
                if (missingL && missingR) {
                    r1xStart = 0;
                    r1xEnd = missingL;
                    region2 = true;
                    r2xStart = target.width - missingR;
                    r2xEnd = target.width;
                } else if (missingL) {
                    r1xStart = 0;
                    r1xEnd = missingL;
                } else {
                    r1xStart = target.width - missingR;
                    r1xEnd = target.width;
                }
                int height = target.height - missingB - missingT;
                int padding = missingT;
                for (int y = 0; y < sideTasks; y++) {
                    int top = padding + (int)round((double)height / sideTasks * y);
                    int bottom = padding + (int)round((double)height / sideTasks * (y + 1));
                    if (bottom - top == 0) continue;
                    taskQueue[tasksLeft] = (WorkerTask){false, swapArray, maxIters,
                        target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                        0, 0, false, 0, 0, false,
                        top, bottom, r1xStart, r1xEnd, region2, r2xStart, r2xEnd};
                    tasksLeft++;
                }
            }
            
            tasksTotal = tasksLeft;
            
            if (DEBUG_TIME) {
                QueryPerformanceCounter(&perfStart);
            }
            ReleaseSemaphore(taskSemaphore, 1, NULL);

            while (tasksLeft > 0) {
                Sleep(3);
            }
            
            if (DEBUG_TIME) {
                QueryPerformanceCounter(&perfEnd);
                printf("Calculating move took %dms\n", (perfEnd.QuadPart * 1000 - perfStart.QuadPart * 1000) / perfFrequency.QuadPart);
            }

            // Set finalized parameters
            swapBuffer.freshlyCalculated = true;
            swapBuffer.params = target;
            swapBuffer.missingB = swapBuffer.missingT = swapBuffer.missingL = swapBuffer.missingR = 0;
            MemoryBarrier();
            swapBuffer.wip = 0;
            if (DEBUG_THREAD >= 2) printf("Done!!\n");
        }
        else {
            releaseBufferSemaphore('C');
        }
    }
    if (DEBUG_THREAD) printf("Finishing CalculateThreadFunction\n");
    return 0;
}

unsigned __stdcall WorkerThreadFunction( void* pArguments ) {
    unsigned int workerId = (unsigned int)(uintptr_t)pArguments;
    int currentTaskI = -1;
    WorkerTask currentTask = { 0 };
    while (threadsRunning) {
        // If thread was performing a task last loop, don't waste time with Sleep
        if (currentTaskI == -1)
            Sleep(3);

        currentTaskI = -1;
        // Find next task
        if (tasksLeft <= 0) continue;

        if (WaitForSingleObject(taskSemaphore, 1000) != 0) continue;
        for (size_t i = 0; i < tasksTotal; i++) {
            if (!taskQueue[i].taken) {
                taskQueue[i].taken = true;
                currentTaskI = i;
                currentTask = taskQueue[i];
                break;
            }
        }
        ReleaseSemaphore(taskSemaphore, 1, NULL);
        if (currentTaskI == -1) continue;

        // Calculate
        if (DEBUG_WORKER) printf("Calculating thread %d task %d\n", workerId, currentTaskI);
        calculate(currentTask.target, currentTask.maxIters,
            currentTask.centerX, currentTask.centerY, currentTask.pixelStep, currentTask.width, currentTask.height,
            currentTask.hstriping, currentTask.hstripeOffset, currentTask.hfillIn,
            currentTask.vstriping, currentTask.vstripeOffset, currentTask.vfillIn,
            currentTask.yStart, currentTask.yEnd,
            currentTask.r1xStart, currentTask.r1xEnd,
            currentTask.region2, currentTask.r2xStart, currentTask.r2xEnd);

        // Announce task done
        if (WaitForSingleObject(taskSemaphore, INFINITE) != 0) continue;
        if (DEBUG_WORKER) printf("Finished thread %d!!\n", workerId);
        tasksLeft--;
        ReleaseSemaphore(taskSemaphore, 1, NULL);
    }
    if (DEBUG_THREAD) printf("Finishing WorkerThreadFunction %d\n", workerId);
    return 0;
}

// The rest is never gonna be called before successful rendererInitialize
void panFrame(int xPixels, int yPixels) {
    if (WaitForSingleObject(statusSemaphore, 100) != 0) return;
    desiredOffsetX -= (double)xPixels * getCurrentPixelStep();
    desiredOffsetY -= (double)yPixels * getCurrentPixelStep();
    ReleaseSemaphore(statusSemaphore, 1, NULL);
}

void zoomFrame(int xPixel, int yPixel, int level) {
    if (WaitForSingleObject(statusSemaphore, 100) != 0) return;
    if (level > 0) {
        desiredZoom *= 1.5;
    }
    if (level < 0) {
        desiredZoom /= 1.5;
    }
    ReleaseSemaphore(statusSemaphore, 1, NULL);
}

void resizeFrame(int width, int height) {
    if (WaitForSingleObject(statusSemaphore, INFINITE) != 0) return;
    desiredWidth = width;
    desiredHeight = height;
    ReleaseSemaphore(statusSemaphore, 1, NULL);
}

int lastDraw = -1;
bool tryRedraw32(uint32_t *pixels, int width, int height) {
    if (waitForBufferSemaphore(100, 'D') != 0) return false;

    if (DEBUG_REDRAW)
        printf("bfr.array %p, bfr.w %d == %d, bfr.h %d == %d\n",
            mainBuffer.array, mainBuffer.params.width, width, mainBuffer.params.height, height);
    if (mainBuffer.array && mainBuffer.params.width == width && mainBuffer.params.height == height) {
        memset(pixels, 0, width * height * sizeof(uint32_t));
        uint8_t *pixelData = (uint8_t*)pixels;
        
        for (int i = 0; i < width * height; i++) {
            int value = mainBuffer.array[i];
            pixelData[i * 4 + 2] = palette[value * 4];
            pixelData[i * 4 + 1] = palette[value * 4 + 1];
            pixelData[i * 4 + 0] = palette[value * 4 + 2];
        }
        
        releaseBufferSemaphore('D');
        return true;
    }
    releaseBufferSemaphore('D');
    return false;
}
