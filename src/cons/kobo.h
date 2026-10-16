// CONscripter: Kobo e-readers. SDL runs on its dummy video driver; each
// presented frame goes to the e-ink screen through FBInk, and the touch
// screen and page keys are read from evdev and pushed as SDL events.
#ifndef __KOBO_H__
#define __KOBO_H__

#include <SDL2/SDL.h>

namespace kobo {
// Opens the framebuffer. Returns the screen's long side, the window width
// that fills a landscape screen, or 0 if FBInk could not start.
int open();
// Starts reading the touch screen and page keys into the window's events.
void start(SDL_Window *window);
// Shows the frame just presented on the e-ink screen.
void present(SDL_Renderer *renderer);
// The event the power button sends: sleep() then, from the event loop.
// A fixed number after ONScripter's own events, which SDL doesn't know of
// (SDL_RegisterEvents would hand out ONS_TIMER_EVENT's).
const Uint32 sleep_event = SDL_USEREVENT + 10; // +9 is chfreq's, taken
// Shows the sleep screen, suspends the Kobo, and shows the game again once woken.
void sleep();
}

#endif
