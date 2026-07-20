/* SDL_thylacine event plumbing (G-7a). See SDL_thylacinevideo.h. */
#ifndef SDL_thylacineevents_c_h_
#define SDL_thylacineevents_c_h_

#include "SDL_thylacinevideo.h"

/* Start/stop the per-window event pump thread. */
extern int THYLACINE_StartEventPump(SDL_Window *window);
extern void THYLACINE_StopEventPump(SDL_Window *window);

/* The device PumpEvents hook: drain the ring + translate to SDL. */
extern void THYLACINE_PumpEvents(_THIS);

#endif /* SDL_thylacineevents_c_h_ */
