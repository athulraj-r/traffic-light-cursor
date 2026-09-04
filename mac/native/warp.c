#include <node_api.h>
#include <ApplicationServices/ApplicationServices.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#define STATE_RED    0
#define STATE_YELLOW 1
#define STATE_GREEN  2

static volatile int g_trafficState = STATE_RED;
static volatile bool g_cursorControlEnabled = true;
static volatile bool g_reverseControls = false;
static volatile bool g_speedingPenalized = false;
static volatile bool g_chaos = false;
static volatile double g_yellowMultiplier = 0.15;
static CGPoint g_frozenPosition = {0, 0};

static CFMachPortRef g_tap = NULL;
static CFRunLoopRef g_runLoop = NULL;
static pthread_t g_thread;
static volatile bool g_hookRunning = false;

static CGEventRef EventTapCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon) {
    if (type == (CGEventType)kCGEventTapDisabledByTimeout) {
        if (g_tap) {
            CGEventTapEnable(g_tap, true);
        }
        return event;
    }

    if (!g_cursorControlEnabled) {
        return event;
    }

    if (type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged || type == kCGEventRightMouseDragged) {
        // 1. Immobilized during speeding penalty
        if (g_speedingPenalized) {
            CGWarpMouseCursorPosition(g_frozenPosition);
            return NULL;
        }

        // 2. REVERSE GEAR: Invert steering
        if (g_reverseControls) {
            int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
            int64_t dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);
            if (dx != 0 || dy != 0) {
                g_frozenPosition.x -= (double)dx;
                g_frozenPosition.y -= (double)dy;
                CGWarpMouseCursorPosition(g_frozenPosition);
            }
            return NULL;
        }

        // 3. RED light suppression -> cursor frozen
        if (g_trafficState == STATE_RED) {
            CGWarpMouseCursorPosition(g_frozenPosition);
            return NULL;
        }

        int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
        int64_t dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);

        // 4. YELLOW light handling: slow crawl + micro-jitter in chaos
        if (g_trafficState == STATE_YELLOW) {
            double mult = g_yellowMultiplier;
            double stepX = (double)dx * mult;
            double stepY = (double)dy * mult;

            if (g_chaos && (rand() % 6 == 0)) {
                stepX += (double)(rand() % 5 - 2);
                stepY += (double)(rand() % 5 - 2);
            }

            g_frozenPosition.x += stepX;
            g_frozenPosition.y += stepY;
            CGWarpMouseCursorPosition(g_frozenPosition);
            return NULL;
        }

        // 5. GREEN light handling: full speed with occasional chaos jump
        if (g_trafficState == STATE_GREEN) {
            if (g_chaos && (rand() % 8 == 0)) {
                CGPoint cur = CGEventGetLocation(event);
                cur.x += (CGFloat)(rand() % 9 - 4);
                cur.y += (CGFloat)(rand() % 9 - 4);
                g_frozenPosition = cur;
                CGWarpMouseCursorPosition(cur);
                return NULL;
            }
            g_frozenPosition = CGEventGetLocation(event);
            return event;
        }
    }

    return event;
}

static void* HookThread(void* arg) {
    CGEventMask mask = CGEventMaskBit(kCGEventMouseMoved) |
                       CGEventMaskBit(kCGEventLeftMouseDragged) |
                       CGEventMaskBit(kCGEventRightMouseDragged);

    g_tap = CGEventTapCreate(
        kCGSessionEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionDefault,
        mask,
        EventTapCallback,
        NULL
    );

    if (!g_tap) {
        // Fallback to ListenOnly if Default fails
        g_tap = CGEventTapCreate(
            kCGSessionEventTap,
            kCGHeadInsertEventTap,
            kCGEventTapOptionListenOnly,
            mask,
            EventTapCallback,
            NULL
        );
    }

    if (!g_tap) {
        g_hookRunning = false;
        return NULL;
    }

    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_tap, 0);
    g_runLoop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(g_runLoop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(g_tap, true);

    g_hookRunning = true;
    CFRunLoopRun();

    if (g_tap) {
        CFRelease(g_tap);
        g_tap = NULL;
    }
    if (source) {
        CFRelease(source);
    }
    g_hookRunning = false;
    return NULL;
}

// ---------------------------------------------------------------------------
// N-API BINDINGS
// ---------------------------------------------------------------------------

static napi_value StartHook(napi_env env, napi_callback_info info) {
    if (!g_hookRunning) {
        // Initialize anchor to current cursor position
        CGEventRef ev = CGEventCreate(NULL);
        if (ev) {
            g_frozenPosition = CGEventGetLocation(ev);
            CFRelease(ev);
        }
        pthread_create(&g_thread, NULL, HookThread, NULL);
    }
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value StopHook(napi_env env, napi_callback_info info) {
    g_cursorControlEnabled = false;
    if (g_runLoop) {
        CFRunLoopStop(g_runLoop);
    }
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value SetState(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    if (argc >= 1) {
        char buf[32] = {0};
        size_t str_len;
        napi_get_value_string_utf8(env, args[0], buf, sizeof(buf) - 1, &str_len);

        int newState = STATE_RED;
        if (strcmp(buf, "YELLOW") == 0) newState = STATE_YELLOW;
        else if (strcmp(buf, "GREEN") == 0) newState = STATE_GREEN;

        if (g_trafficState != newState) {
            // Anchor from current location on state change
            CGEventRef ev = CGEventCreate(NULL);
            if (ev) {
                g_frozenPosition = CGEventGetLocation(ev);
                CFRelease(ev);
            }
            g_trafficState = newState;
        }
    }
    return NULL;
}

static napi_value SetMultiplier(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    if (argc >= 1) {
        double m = 0.15;
        napi_get_value_double(env, args[0], &m);
        g_yellowMultiplier = m;
    }
    return NULL;
}

static napi_value SetEnabled(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    if (argc >= 1) {
        bool en = true;
        napi_get_value_bool(env, args[0], &en);
        g_cursorControlEnabled = en;
        if (en) {
            CGEventRef ev = CGEventCreate(NULL);
            if (ev) {
                g_frozenPosition = CGEventGetLocation(ev);
                CFRelease(ev);
            }
        }
    }
    return NULL;
}

static napi_value SetReverseControls(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    if (argc >= 1) {
        bool rev = false;
        napi_get_value_bool(env, args[0], &rev);
        g_reverseControls = rev;
        if (rev) {
            CGEventRef ev = CGEventCreate(NULL);
            if (ev) {
                g_frozenPosition = CGEventGetLocation(ev);
                CFRelease(ev);
            }
        }
    }
    return NULL;
}

static napi_value SetSpeedingPenalized(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    if (argc >= 1) {
        bool pen = false;
        napi_get_value_bool(env, args[0], &pen);
        g_speedingPenalized = pen;
        if (pen) {
            CGEventRef ev = CGEventCreate(NULL);
            if (ev) {
                g_frozenPosition = CGEventGetLocation(ev);
                CFRelease(ev);
            }
        }
    }
    return NULL;
}

static napi_value SetChaos(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    if (argc >= 1) {
        bool ch = false;
        napi_get_value_bool(env, args[0], &ch);
        g_chaos = ch;
    }
    return NULL;
}

static napi_value GetCursorPosition(napi_env env, napi_callback_info info) {
    CGEventRef ev = CGEventCreate(NULL);
    double x = 0, y = 0;
    if (ev) {
        CGPoint pt = CGEventGetLocation(ev);
        x = (double)pt.x;
        y = (double)pt.y;
        CFRelease(ev);
    }

    napi_value obj, valX, valY;
    napi_create_object(env, &obj);
    napi_create_double(env, x, &valX);
    napi_create_double(env, y, &valY);
    napi_set_named_property(env, obj, "x", valX);
    napi_set_named_property(env, obj, "y", valY);

    return obj;
}

static napi_value WarpMouse(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    if (argc >= 2) {
        double x = 0, y = 0;
        napi_get_value_double(env, args[0], &x);
        napi_get_value_double(env, args[1], &y);
        CGWarpMouseCursorPosition(CGPointMake((CGFloat)x, (CGFloat)y));
    }
    return NULL;
}

static napi_value Init(napi_env env, napi_value exports) {
    napi_value fn;

    napi_create_function(env, "startHook", NAPI_AUTO_LENGTH, StartHook, NULL, &fn);
    napi_set_named_property(env, exports, "startHook", fn);

    napi_create_function(env, "stopHook", NAPI_AUTO_LENGTH, StopHook, NULL, &fn);
    napi_set_named_property(env, exports, "stopHook", fn);

    napi_create_function(env, "setState", NAPI_AUTO_LENGTH, SetState, NULL, &fn);
    napi_set_named_property(env, exports, "setState", fn);

    napi_create_function(env, "setMultiplier", NAPI_AUTO_LENGTH, SetMultiplier, NULL, &fn);
    napi_set_named_property(env, exports, "setMultiplier", fn);

    napi_create_function(env, "setEnabled", NAPI_AUTO_LENGTH, SetEnabled, NULL, &fn);
    napi_set_named_property(env, exports, "setEnabled", fn);

    napi_create_function(env, "setReverseControls", NAPI_AUTO_LENGTH, SetReverseControls, NULL, &fn);
    napi_set_named_property(env, exports, "setReverseControls", fn);

    napi_create_function(env, "setSpeedingPenalized", NAPI_AUTO_LENGTH, SetSpeedingPenalized, NULL, &fn);
    napi_set_named_property(env, exports, "setSpeedingPenalized", fn);

    napi_create_function(env, "setChaos", NAPI_AUTO_LENGTH, SetChaos, NULL, &fn);
    napi_set_named_property(env, exports, "setChaos", fn);

    napi_create_function(env, "getCursorPosition", NAPI_AUTO_LENGTH, GetCursorPosition, NULL, &fn);
    napi_set_named_property(env, exports, "getCursorPosition", fn);

    napi_create_function(env, "warpMouse", NAPI_AUTO_LENGTH, WarpMouse, NULL, &fn);
    napi_set_named_property(env, exports, "warpMouse", fn);

    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
