/* sdl-probe — the G-7a proving binary (TAPESTRY.md section 9/18.9).
 *
 * The first SDL program on Thylacine: SDL_Init(VIDEO) resolves the
 * thylacine bootstrap, SDL_CreateWindow mints a tapestry surface,
 * SDL_GetWindowSurface hands back weave slot 0, SDL_UpdateWindowSurface
 * is a synchronous tpresent — the WHOLE §9 mapping through stock SDL API
 * calls, no driver internals touched. Draws the quadrant pattern (the
 * G-0 screendump family: TL red / TR green / BL blue / BR white) plus an
 * animated sweep bar (proves REPEATED presents + the recycle gate), pumps
 * events each frame (proves the pump thread + non-blocking PumpEvents),
 * then tears down cleanly.
 *
 * Output contract (greppable, the probe convention):
 *   "sdl-probe: PASS driver=thylacine WxH frames=N"  on success
 *   "sdl-probe: FAIL <stage>: <SDL error>"           on any failure
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#define PROBE_W 640
#define PROBE_H 400
#define PROBE_FRAMES 30

static int fail(const char *stage)
{
    printf("sdl-probe: FAIL %s: %s\n", stage, SDL_GetError());
    return 1;
}

int main(int argc, char **argv)
{
    /* Optional hold: `sdl-probe N` keeps the final frame presented for N
     * extra seconds (1 Hz re-presents) — the screendump window for the
     * interactive scenario. Default 0 = pure probe. */
    int hold_s = (argc > 1) ? atoi(argv[1]) : 0;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return fail("init");
    }
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!driver || strcmp(driver, "thylacine") != 0) {
        printf("sdl-probe: FAIL driver: got %s\n", driver ? driver : "(none)");
        SDL_Quit();
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("sdl-probe",
                                       SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED,
                                       PROBE_W, PROBE_H, 0);
    if (!win) {
        SDL_Quit();
        return fail("window");
    }

    int frames = 0;
    for (; frames < PROBE_FRAMES; frames++) {
        SDL_Surface *s = SDL_GetWindowSurface(win);
        if (!s) {
            SDL_DestroyWindow(win);
            SDL_Quit();
            return fail("surface");
        }
        int w = s->w, h = s->h;
        SDL_Rect tl = { 0, 0, w / 2, h / 2 };
        SDL_Rect tr = { w / 2, 0, w - w / 2, h / 2 };
        SDL_Rect bl = { 0, h / 2, w / 2, h - h / 2 };
        SDL_Rect br = { w / 2, h / 2, w - w / 2, h - h / 2 };
        SDL_FillRect(s, &tl, SDL_MapRGB(s->format, 0xFF, 0x00, 0x00));
        SDL_FillRect(s, &tr, SDL_MapRGB(s->format, 0x00, 0xFF, 0x00));
        SDL_FillRect(s, &bl, SDL_MapRGB(s->format, 0x00, 0x00, 0xFF));
        SDL_FillRect(s, &br, SDL_MapRGB(s->format, 0xFF, 0xFF, 0xFF));
        /* The animated sweep bar: distinct per frame, so a stuck present
         * (one frame repeated) is distinguishable from a live loop. */
        SDL_Rect bar = { (frames * w) / PROBE_FRAMES, 0, 8, h };
        if (bar.x + bar.w > w) {
            bar.w = w - bar.x;
        }
        SDL_FillRect(s, &bar, SDL_MapRGB(s->format, 0x00, 0x00, 0x00));

        if (SDL_UpdateWindowSurface(win) != 0) {
            SDL_DestroyWindow(win);
            SDL_Quit();
            return fail("present");
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN &&
                 ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
                goto done;
            }
        }
        SDL_Delay(16); /* proves the 0022 torpor-backed nanosleep */
    }

done:
    if (hold_s > 0) {
        printf("sdl-probe: SHOWING (hold %d s)\n", hold_s);
        fflush(stdout);
        int t;
        for (t = 0; t < hold_s; t++) {
            SDL_Surface *s = SDL_GetWindowSurface(win);
            if (s && SDL_UpdateWindowSurface(win) != 0) {
                break;
            }
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
            }
            SDL_Delay(1000);
        }
    }
    printf("sdl-probe: PASS driver=thylacine %dx%d frames=%d\n",
           PROBE_W, PROBE_H, frames);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
