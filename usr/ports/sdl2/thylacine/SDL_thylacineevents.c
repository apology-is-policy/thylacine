/* SDL_thylacine event pump (G-7a).
 *
 * The tapestry event fid PARKS an empty read (the server's deferred-reply
 * mechanism), and SDL_PumpEvents must never block — so a dedicated pthread
 * blocks on the fid and feeds a bounded mutex ring; PumpEvents drains the
 * ring and translates on the SDL thread. Fd discipline: the pump thread
 * touches ONLY event_fd; ctl/present/weave (including the reweave's
 * close-and-remap) stay on the SDL thread. Shutdown RETIRES the surface
 * (ctl "destroy") so tapestryd reads the event fid EMPTY (stream end) →
 * the parked read returns 0 → the pump exits → join succeeds. (Closing
 * event_fd would NOT cancel the parked read — #844: the pump holds its own
 * ref-held Spoor across the blocking read, so a sibling close never runs
 * the Dev close hook. See THYLACINE_StopEventPump.)
 *
 * Translation notes:
 *   - TEV_KEY.code is a raw evdev keycode (the compositor owns the
 *     keymap); the stock linux_scancode_table maps it to SDL_Scancode.
 *   - The compositor-resolved rune feeds SDL_TEXTINPUT for printables
 *     (>= 0x20, != DEL) on press, when text input is active.
 *   - A size-changing TEV_CONFIGURE acks + reweaves HERE (SDL thread),
 *     then reports SDL_WINDOWEVENT_RESIZED so SDL core invalidates the
 *     window surface; a stale-serial ack (-2) is skipped — a newer
 *     CONFIGURE is already queued. A same-size CONFIGURE is the
 *     compositor's full-redraw request -> SDL_WINDOWEVENT_EXPOSED.
 *   - TEV_PTR / TEV_SCROLL (G-7c): surface-relative tablet input;
 *     relative mode computes deltas driver-side (see PTR_MOVE).
 */
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_THYLACINE

#include "SDL_video.h"
#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/scancodes_linux.h"

#include "SDL_thylacinevideo.h"
#include "SDL_thylacineevents_c.h"

#include <thyla/syscall.h>

static void *THYLACINE_PumpMain(void *arg)
{
    SDL_WindowData *wd = (SDL_WindowData *)arg;
    ThylaEvent tmp[8];

    for (;;) {
        int n = thyla_tap_read_events(&wd->tap, tmp, 8);
        int i;
        if (n <= 0) {
            /* EOF (surface retired / fd closed at shutdown) or error:
             * the pump is done. */
            return NULL;
        }
        pthread_mutex_lock(&wd->lock);
        for (i = 0; i < n; i++) {
            if (tmp[i].kind == THYLA_TEV_FRAME) {
                /* #51: FRAME is the pacing signal, consumed HERE (the
                 * present path waits on it); it never rides the ring --
                 * translation dropped it anyway, and a paced app's
                 * blocked present must not fill the ring with ticks. */
                wd->frame_seq++;
                pthread_cond_signal(&wd->frame_cv);
                continue;
            }
            if (wd->q_len >= THYLACINE_EVQ_CAP) {
                break; /* bounded: drop the newest */
            }
            wd->q[(wd->q_head + wd->q_len) % THYLACINE_EVQ_CAP] = tmp[i];
            wd->q_len++;
        }
        pthread_mutex_unlock(&wd->lock);
    }
}

int THYLACINE_StartEventPump(SDL_Window *window)
{
    SDL_WindowData *wd = (SDL_WindowData *)window->driverdata;

    if (pthread_mutex_init(&wd->lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&wd->frame_cv, NULL) != 0) {
        pthread_mutex_destroy(&wd->lock);
        return -1;
    }
    wd->nopace = (SDL_getenv("SDL_THYLACINE_NOPACE") != NULL);
    if (pthread_create(&wd->pump, NULL, THYLACINE_PumpMain, wd) != 0) {
        pthread_cond_destroy(&wd->frame_cv);
        pthread_mutex_destroy(&wd->lock);
        return -1;
    }
    wd->pump_started = 1;
    return 0;
}

void THYLACINE_StopEventPump(SDL_Window *window)
{
    SDL_WindowData *wd = (SDL_WindowData *)window->driverdata;

    if (!wd->pump_started) {
        return;
    }
    /* Wake the parked pump so the join is bounded. Closing event_fd from
     * HERE does NOT cancel the pump's blocked t_read (#844: the pump holds
     * its own ref-held Spoor across the read, so a sibling close drops the
     * refcount to 1, not 0 — the Dev close hook that would cancel the
     * deferred read never runs). The parked read wakes only on a tapestryd
     * event delivery. So RETIRE the surface (ctl "destroy") — tapestryd
     * then reads the event fid EMPTY (stream end) → thyla_tap_read_events
     * returns 0 → the pump exits. Bounded + frame-clock-independent (the
     * periodic FRAME event, ≤ 1/clock_hz, is only the FALLBACK wake if the
     * destroy write couldn't be sent).
     *
     * event_fd is left OWNED by the pump until the join; thyla_tap_close
     * (called after this by DestroyWindow) closes it. So no thread ever
     * touches event_fd concurrently with the pump — the join races nothing. */
    thyla_tap_request_close(&wd->tap);
    pthread_join(wd->pump, NULL);
    pthread_cond_destroy(&wd->frame_cv);
    pthread_mutex_destroy(&wd->lock);
    wd->pump_started = 0;
}

/* UTF-32 -> UTF-8 (bounded 4 bytes + NUL). */
static int THYLACINE_EncodeUtf8(uint32_t cp, char out[5])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = 0;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = 0;
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
        return 3;
    }
    if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        out[4] = 0;
        return 4;
    }
    out[0] = 0;
    return 0;
}

static void THYLACINE_HandleEvent(SDL_Window *window, SDL_WindowData *wd,
                                  const ThylaEvent *ev)
{
    switch (ev->kind) {
    case THYLA_TEV_KEY:
    {
        SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
        if (ev->code < SDL_arraysize(linux_scancode_table)) {
            sc = linux_scancode_table[ev->code];
        }
        if (sc != SDL_SCANCODE_UNKNOWN) {
            SDL_SendKeyboardKey(ev->value ? SDL_PRESSED : SDL_RELEASED, sc);
        }
        if (ev->value && ev->rune >= 0x20 && ev->rune != 0x7F &&
            SDL_IsTextInputActive()) {
            char utf8[5];
            if (THYLACINE_EncodeUtf8(ev->rune, utf8) > 0) {
                SDL_SendKeyboardText(utf8);
            }
        }
        break;
    }
    case THYLA_TEV_CONFIGURE:
    {
        uint32_t cw = ev->value >> 16;
        uint32_t ch = ev->value & 0xffff;
        if (cw == wd->tap.w && ch == wd->tap.h) {
            /* Full-redraw request. */
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_EXPOSED, 0, 0);
            break;
        }
        if (!(window->flags & SDL_WINDOW_RESIZABLE)) {
            /* Fork 2 (letterbox): a FIXED-size SDL window DECLINES the
             * compositor's size offer -- the surface keeps its dims and
             * the compositor letterboxes the mismatch (aspect-preserving
             * scale, centered). Acking would reweave to the pane size
             * while the app keeps rendering its fixed frame into the
             * corner of the bigger surface (the pre-fix zoomed-Quake
             * top-left artifact). The offer is a standing CONFIGURE --
             * unacked is protocol-legal (offers coalesce; the battery's
             * negative probes pin it). EXPOSED so the app repaints. */
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_EXPOSED, 0, 0);
            break;
        }
        {
            int rc = thyla_tap_reweave(&wd->tap, cw, ch, ev->code);
            if (rc == 0) {
                SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED,
                                    (int)cw, (int)ch);
                SDL_SendWindowEvent(window, SDL_WINDOWEVENT_EXPOSED, 0, 0);
            } else if (rc == -1) {
                /* Non-Busy reweave failure is fatal for the surface (the
                 * libtapestry contract); tell the app to close. */
                SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0);
            }
            /* rc == -2: stale serial — a newer CONFIGURE is queued. */
        }
        break;
    }
    case THYLA_TEV_FOCUS:
        if (ev->value) {
            SDL_SetKeyboardFocus(window);
        } else if (SDL_GetKeyboardFocus() == window) {
            SDL_SetKeyboardFocus(NULL);
        }
        break;
    case THYLA_TEV_CLOSE:
        SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0);
        break;
    case THYLA_TEV_PTR_MOVE:
    {
        /* value packs the surface-relative x<<16|y (section 18.4).
         * Translation (this read, ptr_x/ptr_y/ptr_valid, every
         * SDL_SendMouse*) runs on the SDL MAIN thread only -- PumpEvents
         * drains the pump thread's ring; the pump thread itself never
         * touches SDL state (G-7c audit F2: do NOT move translation onto
         * the pump thread -- SDL_Mouse state is unsynchronized).
         * In relative mode MOVE only tracks position: EVERY motion
         * (tablet abs or mouse rel) now arrives with a TEV_PTR_REL twin
         * carrying the exact deltas, so a successive-position diff here
         * would double-count. */
        int x = (int)(ev->value >> 16);
        int y = (int)(ev->value & 0xffff);
        if (!SDL_GetMouse()->relative_mode) {
            SDL_SendMouseMotion(window, 0, 0, x, y);
        }
        wd->ptr_x = x;
        wd->ptr_y = y;
        wd->ptr_valid = 1;
        break;
    }
    case THYLA_TEV_PTR_REL:
    {
        /* value packs signed display-pixel deltas dx<<16|dy (i16 each);
         * the compositor routes it to the FOCUSED surface -- exact from
         * a relative device, synthesized from consecutive abs motion
         * (abs-only frontends). Consumed only in relative mode (Quake
         * mouse-look); absolute consumers use PTR_MOVE. */
        if (SDL_GetMouse()->relative_mode) {
            int dx = (Sint16)(ev->value >> 16);
            int dy = (Sint16)(ev->value & 0xffff);
            SDL_SendMouseMotion(window, 0, 1, dx, dy);
        }
        break;
    }
    case THYLA_TEV_PTR_BTN:
    {
        /* code = the evdev BTN_* button; value = press/release. */
        Uint8 btn;
        switch (ev->code) {
        case 0x110: btn = SDL_BUTTON_LEFT;   break;
        case 0x111: btn = SDL_BUTTON_RIGHT;  break;
        case 0x112: btn = SDL_BUTTON_MIDDLE; break;
        case 0x113: btn = SDL_BUTTON_X1;     break;
        case 0x114: btn = SDL_BUTTON_X2;     break;
        default:    btn = 0;                 break;
        }
        if (btn) {
            SDL_SendMouseButton(window, 0,
                                ev->value ? SDL_PRESSED : SDL_RELEASED, btn);
        }
        break;
    }
    case THYLA_TEV_SCROLL:
        /* value = the signed wheel delta (i32 wrap); positive = up. */
        SDL_SendMouseWheel(window, 0, 0.0f, (float)(Sint32)ev->value,
                           SDL_MOUSEWHEEL_NORMAL);
        break;
    case THYLA_TEV_FRAME:
    default:
        break;
    }
}

void THYLACINE_PumpEvents(_THIS)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    SDL_Window *window = vd->window;
    SDL_WindowData *wd;
    ThylaEvent batch[32];
    int n, i;

    if (!window) {
        return;
    }
    wd = (SDL_WindowData *)window->driverdata;
    if (!wd || !wd->pump_started) {
        return;
    }

    pthread_mutex_lock(&wd->lock);
    n = wd->q_len < 32 ? wd->q_len : 32;
    for (i = 0; i < n; i++) {
        batch[i] = wd->q[(wd->q_head + i) % THYLACINE_EVQ_CAP];
    }
    wd->q_head = (wd->q_head + n) % THYLACINE_EVQ_CAP;
    wd->q_len -= n;
    pthread_mutex_unlock(&wd->lock);

    for (i = 0; i < n; i++) {
        THYLACINE_HandleEvent(window, wd, &batch[i]);
    }
}

#endif /* SDL_VIDEO_DRIVER_THYLACINE */
