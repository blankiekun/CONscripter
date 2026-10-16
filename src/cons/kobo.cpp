// CONscripter: Kobo e-readers, c.f. kobo.h
#include "kobo.h"
#include <fbink.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <SDL2/SDL_ttf.h>
#include <glob.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

// The page keys of the Forma, Libra and Libra 2 (KOReader's RPgBack, RPgFwd)
#define KOBO_KEY_BACK 193
#define KOBO_KEY_FWD  194
#define LONG_PRESS_MS 600
#define QUIT_HOLD_MS  2000
#define SUSPEND_ATTEMPTS 5
#define WAKE_IGNORE_MS 1500 // the power press that woke the Kobo

namespace kobo {

static int fbfd = -1;
static FBInkConfig cfg;
static FBInkState state;
static bool rotate;   // a landscape window on a portrait screen
static bool flip;     // KOBO_FLIP=1: the game's top on the right edge, not the left
static int win_w, win_h, img_w, img_h, img_x, img_y;
static std::vector<Uint32> frame;
static std::vector<Uint8> cur, prev, sub;
static Uint32 last_present;
static Uint32 window_id;
static SDL_Window *window;
static volatile Uint32 power_ignore_until; // asleep, or just woken
static TTF_Font *title_font, *status_font;
static FBInkDump koreader_screen;

// KOReader only redraws what it changes once the game is gone: put its screen back
static void restoreScreen()
{
    FBInkConfig c = cfg;
    c.is_flashing = true;
    fbink_restore(fbfd, &c, &koreader_screen);
}

int open()
{
    fbfd = fbink_open();
    if (fbfd < 0) return 0;
    cfg.is_quiet = true;
    if (fbink_init(fbfd, &cfg) < 0) return 0;
    fbink_get_state(&cfg, &state);
    flip = getenv("KOBO_FLIP") && atoi(getenv("KOBO_FLIP"));
    if (fbink_dump(fbfd, &koreader_screen) == 0) atexit(restoreScreen);
    fprintf(stderr, "kobo: %s (%s), view %ux%u, rota %u -> canonical %u, touch swap %d mirror %d %d\n",
            state.device_name, state.device_codename, state.view_width, state.view_height,
            state.current_rota, state.rotation_map[state.current_rota & 3],
            state.touch_swap_axes, state.touch_mirror_x, state.touch_mirror_y);
    return state.view_width > state.view_height ? state.view_width : state.view_height;
}

static void show(const Uint32 *pixels, bool flash);

// Window pixel of the image pixel (x, y): the inverse of the rotation in present()
static void imageToWindow(int x, int y, int *wx, int *wy)
{
    if (!rotate) { *wx = x; *wy = y; return; }
    if (flip) { *wx = y; *wy = win_h - 1 - x; }
    else { *wx = win_w - 1 - y; *wy = x; }
}

void present(SDL_Renderer *renderer)
{
    if (fbfd < 0) return;
    int w, h;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    if (w != win_w || h != win_h) {
        win_w = w; win_h = h;
        rotate = (w > h) != (state.view_width > state.view_height);
        img_w = rotate ? h : w;
        img_h = rotate ? w : h;
        img_x = ((int)state.view_width - img_w) / 2;
        img_y = ((int)state.view_height - img_h) / 2;
        frame.resize(w * h);
        cur.resize(img_w * img_h);
        prev.clear();
    }
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, frame.data(), w * 4) < 0) return;
    show(frame.data(), false);
}

// Puts window pixels on the screen: what changed since the last, all with flash
static void show(const Uint32 *pixels, bool flash)
{
    // grayscale, rotated, and the box of what changed
    int x0 = img_w, y0 = img_h, x1 = -1, y1 = -1;
    for (int y = 0; y < img_h; y++) {
        Uint8 *row = &cur[y * img_w];
        for (int x = 0; x < img_w; x++) {
            int wx, wy;
            imageToWindow(x, y, &wx, &wy);
            Uint32 p = pixels[wy * win_w + wx];
            row[x] = (((p >> 16) & 0xff) * 77 + ((p >> 8) & 0xff) * 150 + (p & 0xff) * 29) >> 8;
        }
        if (!prev.empty() && !memcmp(row, &prev[y * img_w], img_w)) continue;
        for (int x = 0; x < img_w; x++)
            if (prev.empty() || row[x] != prev[y * img_w + x]) {
                if (x < x0) x0 = x;
                break;
            }
        for (int x = img_w - 1; x >= 0; x--)
            if (prev.empty() || row[x] != prev[y * img_w + x]) {
                if (x > x1) x1 = x;
                break;
            }
        if (y < y0) y0 = y;
        y1 = y;
    }
    if (x1 < 0) return;

    FBInkConfig c = cfg;
    Uint32 now = SDL_GetTicks();
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    // a new scene after a pause, not a frame of an animation: flash away the ghosting
    c.is_flashing = flash || (bw * bh * 2 >= img_w * img_h && now - last_present > 1500);
    c.is_cleared = prev.empty(); // the borders around the image
    last_present = now;

    sub.resize(bw * bh);
    for (int y = 0; y < bh; y++)
        memcpy(&sub[y * bw], &cur[(y0 + y) * img_w + x0], bw);
    fbink_print_raw_data(fbfd, sub.data(), bw, bh, sub.size(), img_x + x0, img_y + y0, &c);
    prev.swap(cur);
    cur.resize(img_w * img_h);
}

static bool writeFile(const char *path, const char *value)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    fputs(value, fp);
    if (fclose(fp) == 0) return true;
    fprintf(stderr, "kobo: writing %s to %s failed: %s\n", value, path, strerror(errno));
    return false;
}

static int readInt(const char *pattern)
{
    glob_t g;
    int v = -1;
    if (glob(pattern, 0, NULL, &g) == 0) {
        FILE *fp = fopen(g.gl_pathv[0], "r");
        if (fp) { if (fscanf(fp, "%d", &v) != 1) v = -1; fclose(fp); }
    }
    globfree(&g);
    return v;
}

static void drawText(SDL_Surface *canvas, TTF_Font *font, const char *text, int y)
{
    if (!font || !*text) return;
    SDL_Color black = {0, 0, 0, 255};
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, black);
    if (!s) return;
    SDL_Rect r = {(canvas->w - s->w) / 2, y, s->w, s->h};
    SDL_BlitSurface(s, NULL, canvas, &r);
    SDL_FreeSurface(s);
}

// The sleep screen: the game as it is, its title, and the battery, time and play time
static void showSplash()
{
    SDL_Surface *canvas = SDL_CreateRGBSurfaceWithFormat(0, win_w, win_h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface *game = SDL_CreateRGBSurfaceWithFormatFrom(frame.data(), win_w, win_h, 32, win_w * 4, SDL_PIXELFORMAT_ARGB8888);
    if (!canvas || !game) { SDL_FreeSurface(canvas); SDL_FreeSurface(game); return; }
    SDL_FillRect(canvas, NULL, 0xffffffff);

    int margin = win_h / 25;
    SDL_Rect border = {win_w / 10 - 3, margin - 3, win_w * 4 / 5 + 6, win_h * 4 / 5 + 6};
    SDL_FillRect(canvas, &border, 0xff000000);
    SDL_Rect shot = {win_w / 10, margin, win_w * 4 / 5, win_h * 4 / 5};
    SDL_BlitScaled(game, NULL, canvas, &shot);

    const char *title = SDL_GetWindowTitle(window);
    char cwd[256] = "";
    if (!title || !*title) {
        if (getcwd(cwd, sizeof(cwd))) title = strrchr(cwd, '/') ? strrchr(cwd, '/') + 1 : cwd;
        else title = "";
    }
    char status[160], when[64], battery[32] = "";
    time_t t = time(NULL);
    strftime(when, sizeof(when), "%a %d %b, %H:%M", localtime(&t));
    int capacity = readInt("/sys/class/power_supply/*/capacity");
    if (capacity >= 0) snprintf(battery, sizeof(battery), "Battery %d%%  ·  ", capacity);
    Uint32 played = SDL_GetTicks() / 60000; // minutes; the clock stops while asleep
    if (played >= 60) snprintf(status, sizeof(status), "%sPlayed %uh %02um  ·  %s", battery, played / 60, played % 60, when);
    else snprintf(status, sizeof(status), "%sPlayed %um  ·  %s", battery, played, when);

    int y = shot.y + shot.h + margin / 2;
    drawText(canvas, title_font, title, y);
    if (title_font) y += TTF_FontLineSkip(title_font);
    drawText(canvas, status_font, status, y);

    prev.clear();
    show((const Uint32 *)canvas->pixels, true);
    SDL_FreeSurface(game);
    SDL_FreeSurface(canvas);
}

void sleep()
{
    if (fbfd < 0 || frame.empty()) return;
    power_ignore_until = 0xffffffff;
    showSplash();

    // the frontlight stays on in suspend: off, and back as it was
    glob_t lights;
    std::vector<int> levels;
    if (glob("/sys/class/backlight/*/brightness", 0, NULL, &lights) != 0) lights.gl_pathc = 0;
    for (size_t i = 0; i < lights.gl_pathc; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s", lights.gl_pathv[i]);
        levels.push_back(readInt(path));
        writeFile(path, "0");
    }

    // as KOReader suspends a Kobo: flag it, 2 s, then suspend, which the
    // kernel refuses (EBUSY) while the e-ink or touch panel is busy: again then
    SDL_Delay(1000); // the sleep screen's refresh
    for (int attempt = 1; attempt <= SUSPEND_ATTEMPTS; attempt++) {
        writeFile("/sys/power/state-extended", "1");
        SDL_Delay(2000);
        sync();
        fprintf(stderr, "kobo: suspend (attempt %d)\n", attempt);
        bool slept = writeFile("/sys/power/state", "mem"); // returns once woken
        writeFile("/sys/power/state-extended", "0");
        if (slept) break;
    }
    fprintf(stderr, "kobo: resume\n");

    for (size_t i = 0; i < lights.gl_pathc; i++) {
        if (levels[i] < 0) continue;
        char value[16];
        snprintf(value, sizeof(value), "%d", levels[i]);
        writeFile(lights.gl_pathv[i], value);
    }
    if (lights.gl_pathc) globfree(&lights);

    prev.clear();
    show(frame.data(), true);
    power_ignore_until = SDL_GetTicks() + WAKE_IGNORE_MS;
}

static void pushButton(int button, int x, int y)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.button.windowID = window_id;
    e.button.button = button;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.state = SDL_PRESSED;
    SDL_PushEvent(&e);
    e.type = SDL_MOUSEBUTTONUP;
    e.button.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

static void pushMotion(int x, int y)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEMOTION;
    e.motion.windowID = window_id;
    e.motion.x = x;
    e.motion.y = y;
    SDL_PushEvent(&e);
}

static void pushKey(SDL_Keycode sym, SDL_Scancode scancode, bool down)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.windowID = window_id;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.sym = sym;
    e.key.keysym.scancode = scancode;
    SDL_PushEvent(&e);
}

static bool testBit(const unsigned long *bits, int bit)
{
    return bits[bit / (8 * sizeof(long))] & (1UL << (bit % (8 * sizeof(long))));
}

struct Touch {
    int fd;
    input_absinfo ax, ay;
    int x, y, slot;
    bool down, pending_down, pending_up;
    Uint32 down_time;
};

// Window pixel of a raw touch point
static void touchToWindow(const Touch &t, int *wx, int *wy)
{
    int x = t.x, y = t.y;
    input_absinfo ax = t.ax, ay = t.ay;
    if (state.touch_swap_axes) { int s = x; x = y; y = s; input_absinfo a = ax; ax = ay; ay = a; }
    if (state.touch_mirror_x) x = ax.maximum - (x - ax.minimum);
    if (state.touch_mirror_y) y = ay.maximum - (y - ay.minimum);

    // in the screen held upright, then in the view at its rotation
    int canonical = state.rotation_map[state.current_rota & 3];
    int vw = state.view_width, vh = state.view_height;
    int cw = canonical & 1 ? vh : vw, ch = canonical & 1 ? vw : vh;
    int cx = (long)(x - ax.minimum) * cw / (ax.maximum - ax.minimum + 1);
    int cy = (long)(y - ay.minimum) * ch / (ay.maximum - ay.minimum + 1);
    int vx = cx, vy = cy;
    switch (canonical) {
    case FB_ROTATE_CW:  vx = ch - 1 - cy; vy = cx; break;
    case FB_ROTATE_UD:  vx = cw - 1 - cx; vy = ch - 1 - cy; break;
    case FB_ROTATE_CCW: vx = cy; vy = cw - 1 - cx; break;
    }

    int ix = SDL_clamp(vx - img_x, 0, img_w - 1), iy = SDL_clamp(vy - img_y, 0, img_h - 1);
    imageToWindow(ix, iy, wx, wy);
    fprintf(stderr, "kobo: touch %d,%d -> view %d,%d -> window %d,%d\n", t.x, t.y, vx, vy, *wx, *wy);
}

static void touchEvent(Touch &t, const input_event &ev)
{
    if (ev.type == EV_ABS) {
        if (ev.code == ABS_MT_SLOT) t.slot = ev.value;
        if (t.slot != 0) return; // the first finger only
        switch (ev.code) {
        case ABS_X: case ABS_MT_POSITION_X: t.x = ev.value; break;
        case ABS_Y: case ABS_MT_POSITION_Y: t.y = ev.value; break;
        case ABS_MT_TRACKING_ID: (ev.value >= 0 ? t.pending_down : t.pending_up) = true; break;
        case ABS_MT_PRESSURE: if (ev.value == 0) t.pending_up = true; break;
        }
    }
    else if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
        (ev.value ? t.pending_down : t.pending_up) = true;
    else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        int wx, wy;
        if (t.pending_down && !t.down) {
            t.down = true;
            t.down_time = SDL_GetTicks();
            touchToWindow(t, &wx, &wy);
            pushMotion(wx, wy);
        }
        else if (t.pending_up && t.down) {
            t.down = false;
            touchToWindow(t, &wx, &wy);
            pushMotion(wx, wy);
            // a long press is a right click: the game's menu
            pushButton(SDL_GetTicks() - t.down_time >= LONG_PRESS_MS ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT, wx, wy);
        }
        t.pending_down = t.pending_up = false;
    }
}

static void keyEvent(const input_event &ev)
{
    static Uint32 back_down;
    if (ev.type != EV_KEY || ev.value == 2) return; // no auto repeat
    if (ev.code == KEY_POWER) {
        if (ev.value == 0 && SDL_GetTicks() >= power_ignore_until) {
            SDL_Event e;
            memset(&e, 0, sizeof(e));
            e.type = sleep_event;
            SDL_PushEvent(&e);
        }
        return;
    }
    if (ev.code == KOBO_KEY_FWD || ev.code == KEY_PAGEDOWN)
        pushKey(SDLK_RETURN, SDL_SCANCODE_RETURN, ev.value);
    else if (ev.code == KOBO_KEY_BACK || ev.code == KEY_PAGEUP) {
        if (ev.value) { back_down = SDL_GetTicks(); return; }
        // held: back to KOReader, whatever the game's menu has
        if (SDL_GetTicks() - back_down >= QUIT_HOLD_MS) {
            SDL_Event e;
            memset(&e, 0, sizeof(e));
            e.type = SDL_QUIT;
            SDL_PushEvent(&e);
            return;
        }
        pushKey(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, true);
        pushKey(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, false);
    }
}

static void *inputLoop(void *)
{
    std::vector<pollfd> fds;
    std::vector<Touch> touches; // by fds index; fd -1 for key devices
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long abs[ABS_CNT / (8 * sizeof(long)) + 1] = {}, key[KEY_CNT / (8 * sizeof(long)) + 1] = {};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
        Touch t = {};
        t.fd = -1;
        if (testBit(abs, ABS_MT_POSITION_X) || testBit(abs, ABS_X)) {
            int code_x = testBit(abs, ABS_MT_POSITION_X) ? ABS_MT_POSITION_X : ABS_X;
            ioctl(fd, EVIOCGABS(code_x), &t.ax);
            ioctl(fd, EVIOCGABS(code_x + 1), &t.ay); // ABS_Y, ABS_MT_POSITION_Y
            t.fd = fd;
        }
        else if (!testBit(key, KOBO_KEY_FWD) && !testBit(key, KEY_PAGEDOWN) && !testBit(key, KEY_POWER)) {
            ::close(fd); // the rest (the sleep cover...) stays with the system
            continue;
        }
        // while the game runs, KOReader (paused in os.execute) gets none of the taps
        // or power presses
        ioctl(fd, EVIOCGRAB, 1);
        fprintf(stderr, "kobo: %s: %s\n", path, t.fd >= 0 ? "touch" : "keys");
        fds.push_back({fd, POLLIN, 0});
        touches.push_back(t);
    }

    input_event ev[64];
    while (poll(fds.data(), fds.size(), -1) > 0) {
        for (size_t i = 0; i < fds.size(); i++) {
            if (!(fds[i].revents & POLLIN)) continue;
            ssize_t n = read(fds[i].fd, ev, sizeof(ev));
            for (ssize_t j = 0; j < n / (ssize_t)sizeof(ev[0]); j++) {
                if (touches[i].fd >= 0) touchEvent(touches[i], ev[j]);
                else keyEvent(ev[j]);
            }
        }
    }
    return NULL;
}

void start(SDL_Window *w)
{
    if (fbfd < 0) return;
    window = w;
    window_id = SDL_GetWindowID(window);
    // the game's font, for the sleep screen (play.sh runs in the game's folder)
    int h;
    SDL_GetWindowSize(window, NULL, &h);
    if (TTF_Init() == 0) {
        title_font = TTF_OpenFont("default.ttf", h / 20);
        status_font = TTF_OpenFont("default.ttf", h / 32);
    }
    pthread_t thread;
    pthread_create(&thread, NULL, inputLoop, NULL);
    pthread_detach(thread);
}

}
