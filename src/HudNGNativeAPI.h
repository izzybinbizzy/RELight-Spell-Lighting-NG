#ifndef HUDNG_NATIVE_API_H
#define HUDNG_NATIVE_API_H
#include <stdint.h>
#if defined(_WIN32)
#define HUDNG_NATIVE_CALL __cdecl
#if defined(HUDNG_NATIVE_EXPORTS)
#define HUDNG_NATIVE_EXPORT __declspec(dllexport)
#else
#define HUDNG_NATIVE_EXPORT
#endif
#else
#define HUDNG_NATIVE_CALL
#define HUDNG_NATIVE_EXPORT
#endif
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
#define HUDNG_NATIVE_API_VERSION 1u
#define HUDNG_NATIVE_MAX_WIDGETS 128u
typedef uint64_t HudNGNativeHandle;
typedef enum HudNGNativeStatus {
    HUDNG_NATIVE_OK=0, HUDNG_NATIVE_INVALID_ARGUMENT=1,
    HUDNG_NATIVE_DUPLICATE=2, HUDNG_NATIVE_CAPACITY=3,
    HUDNG_NATIVE_STALE_HANDLE=4, HUDNG_NATIVE_INTERNAL_ERROR=5
} HudNGNativeStatus;
/* Fixed POD buffers must be NUL terminated. Identity: [A-Za-z0-9_-], 1..63.
   Label: printable bytes, 1..127. The service copies all data synchronously. */
typedef struct HudNGNativeDescriptor {
    uint32_t structSize;
    uint32_t supportsEditingPreview; /* 0 or 1; producer owns sample rendering */
    char providerID[64];
    char widgetID[64];
    char displayLabel[128];
} HudNGNativeDescriptor;
/* Report BASELINE, untransformed geometry every frame, even when intrinsically
   hidden. Coordinates are physical pixels in the same space as the viewport.
   Never feed last frame's adjusted geometry back into Query. */
typedef struct HudNGNativeFrame {
    uint32_t structSize;
    uint32_t intrinsicVisible; /* 0 or 1 */
    double leftPx, topPx, widthPx, heightPx;
    double anchorXpx, anchorYpx;
    double viewportLeftPx, viewportTopPx, viewportWidthPx, viewportHeightPx;
} HudNGNativeFrame;
/* Apply once: p' = anchor + scale * (p - anchor) + offset.
   Multiply the producer's normal alpha by opacityMultiplier. editingPreview
   permits the producer to supply its own preview for normally hidden content;
   HUD NG never calls the producer or changes producer state.
   Initialize result.structSize before Query. With a valid result size, failed
   queries initialize it to identity. All pointers must address valid memory.
   If the DLL/export/version is absent, use identity and draw normally. */
typedef struct HudNGNativeResult {
    uint32_t structSize;
    uint32_t editingPreview;
    double offsetXpx, offsetYpx, scaleX, scaleY, opacityMultiplier;
} HudNGNativeResult;
typedef struct HudNGNativeAPI {
    uint32_t structSize;
    uint32_t version;
    HudNGNativeStatus (HUDNG_NATIVE_CALL *Register)(const HudNGNativeDescriptor*, HudNGNativeHandle*);
    HudNGNativeStatus (HUDNG_NATIVE_CALL *Unregister)(HudNGNativeHandle);
    HudNGNativeStatus (HUDNG_NATIVE_CALL *Query)(HudNGNativeHandle, const HudNGNativeFrame*, HudNGNativeResult*);
} HudNGNativeAPI;
#pragma pack(pop)
/* Query with GetProcAddress, do not link an import library. Unregister before
   producer unload. No callbacks or producer pointers are retained. Calls may
   be concurrent. DLL unload concurrent with calls is not supported. */
HUDNG_NATIVE_EXPORT const HudNGNativeAPI* HUDNG_NATIVE_CALL HudNG_GetNativeAPI(uint32_t version);
#ifdef __cplusplus
}
#endif
#endif


