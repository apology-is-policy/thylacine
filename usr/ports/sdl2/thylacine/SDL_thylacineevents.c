/* SDL_thylacine event pump (G-7a).
 *
 * The tapestry event fid PARKS an empty read (the server's deferred-reply
 * mechanism), and SDL_PumpEvents must never block — so a dedicated pthread
 * blocks on the fid and feeds a bounded mutex ring; PumpEvents drains the
 * ring and translates on the SDL thread. Fd discipline: the pump thread
 * touches ONLY event_fd; ctl/present/weave (including the reweave's
 * close-and-remap) stay on the SDL thread. Shutdown closes event_fd from
 * the SDL thread — the kernel's cancel-at-close discipline completes the
 * parked read, the thread exits, join succeeds.
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
 *   - TEV_PTR_* / TEV_SCROLL arrive with the tablet device (G-7c).
 */
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_THYLACINE

#include "SDL_video.h"
#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"
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
    if (pthread_create(&wd->pump, NULL, THYLACINE_PumpMain, wd) != 0) {
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
    /* Cancel-at-close: the fid clunk cancels the parked event read; the
     * pump sees <= 0 and exits. */
    if (wd->tap.event_fd >= 0) {
        t_close(wd->tap.event_fd);
        wd->tap.event_fd = -1;
    }
    pthread_join(wd->pump, NULL);
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
    case THYLA_TEV_FRAME:
    case THYLA_TEV_PTR_MOVE: /* G-7c: the tablet device */
    case THYLA_TEV_PTR_BTN:
    case THYLA_TEV_SCROLL:
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
