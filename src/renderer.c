#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "util.h"
#include "renderer.h"
#include "mandelbrot.h"

#define DEBUG_THREAD 2
#define TIME_CALCULATE

#ifdef TIME_CALCULATE
#include <time.h>
#endif

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
    /**  */
    short stripeProgress; short stripeOffset;
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
    int xStart; int xEnd; int yStart; int yEnd;
} WorkerTask;

volatile WorkerTask taskQueue[MAX_QUEUE] = { 0 };
volatile int tasksTotal = 0;
volatile int tasksLeft = 0;

// Fractal specific stuff
int *palette = 0;
const int maxIters = 1000;
const short striping = 4;

DWORD waitForBufferSemaphore(DWORD ms, char label) {
    DWORD result = WaitForSingleObject(bufferSemaphore, ms);
    if (DEBUG_THREAD >= 5)
        printf("%c Waited for bufferSemaphore: %d (%c)\n", label, result, result == 0 ? 's' : 'f');
    return result;
}
DWORD releaseBufferSemaphore(char label) {
    long count = -1;
    BOOL result = ReleaseSemaphore(bufferSemaphore, 1, &count);
    if (DEBUG_THREAD >= 5)
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
                
                // Pan the buffer array (move the content data based on pan)
                if (!(
                    mainBuffer.missingL >= mainBuffer.params.width || mainBuffer.missingR >= mainBuffer.params.width
                    || mainBuffer.missingT >= mainBuffer.params.height || mainBuffer.missingB >= mainBuffer.params.height
                )) {
                    if (DEBUG_THREAD >= 2) printf("Panning!!\n");
                    int width = mainBuffer.params.width;
                    int rowLength = width - abs(shiftX);
                    int sourceX = shiftX > 0 ? 0 : -shiftX;
                    int targetX = shiftX > 0 ? shiftX : 0;
                    if (DEBUG_THREAD >= 2) printf("SourceX %d, TargetX %d\n", sourceX, targetX);
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
    #ifdef TIME_CALCULATE
    LARGE_INTEGER perfFrequency, perfStart, perfEnd;
    QueryPerformanceFrequency(&perfFrequency);
    #endif

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

            tasksTotal = MAX_QUEUE;
            tasksLeft = 0;
            for (int y = 0; y < tasksTotal; y++) {
                int top = (int)round((double)target.height / tasksTotal * y);
                int bottom = (int)round((double)target.height / tasksTotal * (y + 1));
                taskQueue[tasksLeft] = (WorkerTask){false, swapArray, maxIters,
                    target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                    0, target.width, top, bottom};
                tasksLeft++;
            }
            #ifdef TIME_CALCULATE
            QueryPerformanceCounter(&perfStart);
            #endif
            ReleaseSemaphore(taskSemaphore, 1, NULL);

            while (tasksLeft > 0) {
                Sleep(3);
            }
            
            #ifdef TIME_CALCULATE
            QueryPerformanceCounter(&perfEnd);
            printf("Calculating zoom took %dms\n", (perfEnd.QuadPart * 1000 - perfStart.QuadPart * 1000) / perfFrequency.QuadPart);
            #endif

            // Set finalized parameters
            swapBuffer.freshlyCalculated = true;
            swapBuffer.params = target;
            swapBuffer.missingB = swapBuffer.missingT = swapBuffer.missingL = swapBuffer.missingR = 0;
            MemoryBarrier();
            swapBuffer.wip = 0;
            if (DEBUG_THREAD >= 2) printf("Done!!\n");
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
            swapBuffer.wip = 1;
            releaseBufferSemaphore('C');
            
            if (DEBUG_THREAD >= 2) printf("Calculating move!!\n");
            if (missingL) {
                calculate(swapArray, maxIters,
                    target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                    missingT, target.height - missingB, 0, missingL, false, 0, 0);
            }
            if (missingR) {
                calculate(swapArray, maxIters,
                    target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                    missingT, target.height - missingB, target.width - missingR, target.width, false, 0, 0);
            }
            if (missingT) {
                calculate(swapArray, maxIters,
                    target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                    0, missingT, 0, target.width, false, 0, 0);
            }
            if (missingB) {
                calculate(swapArray, maxIters,
                    target.offsetX, target.offsetY, target.pixelStep, target.width, target.height,
                    target.height - missingB, target.height, 0, target.width, false, 0, 0);
            }

            // Set finalized parameters
            swapBuffer.freshlyCalculated = true;
            swapBuffer.params = target;
            swapBuffer.missingB = swapBuffer.missingT = swapBuffer.missingL = swapBuffer.missingR = 0;
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
        //if (currentTaskI == -1)
            Sleep(3);

        // Find next task
        if (tasksLeft <= 0) continue;

        currentTaskI = -1;
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
        if (DEBUG_THREAD >= 3) printf("Calculating thread %d task %d\n", workerId, currentTaskI);
        calculate(currentTask.target, currentTask.maxIters,
            currentTask.centerX, currentTask.centerY, currentTask.pixelStep, currentTask.width, currentTask.height,
            currentTask.yStart, currentTask.yEnd, currentTask.xStart, currentTask.xEnd, false, 0, 0);

        // Announce task done
        if (WaitForSingleObject(taskSemaphore, INFINITE) != 0) continue;
        if (DEBUG_THREAD >= 3) printf("Finished thread %d!!\n", workerId);
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
        desiredZoom *= 1.4;
    }
    if (level < 0) {
        desiredZoom /= 1.4;
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
