#ifndef _EXOKIT_BINDINGS_H_
#define _EXOKIT_BINDINGS_H_

#include <v8.h>
#include <node.h>
#include <image-context.h>
#include <imageData-context.h>
#include <imageBitmap-context.h>
#include <canvas-context.h>
#include <path2d-context.h>
#include <canvas-gradient.h>
#include <canvas-pattern.h>
#include <webgl.h>
#include <AudioContext.h>
#include <Video.h>
#include <browser.h>
#if _WIN32
#include <leapmotion.h>
#endif
#if defined(LUMIN)
#include <magicleap.h>
#endif

std::pair<Local<Object>, Local<FunctionTemplate>> makeGl();
std::pair<Local<Object>, Local<FunctionTemplate>> makeGl2(Local<FunctionTemplate> baseCtor);
Local<Object> makeImage();
Local<Object> makeImageData();
Local<Object> makeImageBitmap();
Local<Object> makeCanvasRenderingContext2D(Local<Value> imageDataCons, Local<Value> canvasGradientCons, Local<Value> canvasPatternCons);
Local<Object> makePath2D();
Local<Object> makeCanvasGradient();
Local<Object> makeCanvasPattern();
Local<Object> makeAudio();
Local<Object> makeVideo(Local<Value> imageDataCons);
Local<Object> makeBrowser();

#endif
