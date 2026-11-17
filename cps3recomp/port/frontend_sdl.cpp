// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of cps3recomp (sf3comp); from cps2recomp.
//
// The window: SDL3 picture, sound and controls around Port::RunFrame().
#include "game_info.h"

#include "bindings.h"
#include "port.h"
#include "png_write.h"
#include "present.h"
#include "touch.h"

#include "../runtime/game.h"
#include "../recomp-common/gamepads.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

using namespace cps3recomp;

namespace port {

#ifdef __APPLE__
void HandFrontToLauncher(); // (front_macos.mm)
#endif

namespace {

    // What each player's keys and gamepad buttons do: the player's choices (bindings.h; the launcher's
    // Controllers page), or the usual layout. A game started from the collection's launcher has the table every
    // game there shares; one with a launcher of its own, its own table.
    const BoundTable &Table() {
        static const BoundTable &table = MakeBoundTable(GamePlayers(), GameButtons(), InCollection());
        return table;
    }
    Bindings g_bindings = recomp::DefaultBindings(Table().table);
    int Players() {
        return Table().table.players;
    }

    static_assert(kBoundActions[0].bit == kUp && kBoundActions[1].bit == kDown && kBoundActions[2].bit == kLeft &&
                  kBoundActions[3].bit == kRight && kBoundActions[4].bit == kPunch1 && kBoundActions[9].bit == kKick3 &&
                  kBoundActions[10].bit == kStart && kBoundActions[11].bit == kCoin &&
                  kBoundActions[12].bit == (kPunch1 | kPunch2 | kPunch3) && kBoundActions[13].bit == (kKick1 | kKick2 | kKick3));

    constexpr int kDead = 16000; // how far a stick or a trigger must go to count

#ifndef __ANDROID__ // (a phone: its one screen, full)
    // The Display page's screen (DisplayScreen): the main display, or the others in the order SDL lists them; the main
    // one where the one chosen is not connected (issue #15)
    SDL_DisplayID ChosenDisplay() {
        const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
        SDL_DisplayID chosen = primary;
        int count = 0, other = 0;
        SDL_DisplayID *all = SDL_GetDisplays(&count);
        for (int i = 0; all && i < count; i++) {
            if (all[i] != primary && ++other == DisplayScreen()) {
                chosen = all[i];
            }
        }
        SDL_free(all);
        return chosen;
    }

    // The screen's mode of exactly the board's 384 x 224 (an arcade monitor's, from CRT Emudriver or a custom
    // resolution), at the refresh nearest the board's 59.6 Hz and no further than 5 Hz from it; false: it has none.
    // Never another size or rate: a monitor of 15 kHz can be harmed by a mode it cannot take (issue #15)
    bool ArcadeMode(SDL_DisplayID display, SDL_DisplayMode &found) {
        int count = 0;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &count);
        bool any = false;
        for (int i = 0; modes && i < count; i++) {
            const SDL_DisplayMode &mode = *modes[i];
            const float density = mode.pixel_density > 0 ? mode.pixel_density : 1.0f;
            const bool size = std::lround(mode.w * density) == Video::kWidth && std::lround(mode.h * density) == Video::kHeight;
            const float off = std::fabs(mode.refresh_rate - static_cast<float>(Machine::kFramesPerSecond));
            if (size && off <= 5.0f && (!any || off < std::fabs(found.refresh_rate - static_cast<float>(Machine::kFramesPerSecond)))) {
                found = mode;
                any = true;
            }
        }
        SDL_free(modes);
        return any;
    }

    // The window's full screen as the Arcade monitor page says (ArcadeResolution): the board's 384 x 224 on the chosen
    // screen where it has that mode; else the desktop's own. Kept by the window for F11 too
    void SetFullscreenMode(SDL_Window *window) {
        SDL_DisplayMode mode;
        if (ArcadeResolution() && ArcadeMode(ChosenDisplay(), mode)) {
            if (!SDL_SetWindowFullscreenMode(window, &mode)) {
                fprintf(stderr, "full screen %dx%d at %.2f Hz: %s\n", mode.w, mode.h, mode.refresh_rate, SDL_GetError());
            }
            return;
        }
        if (ArcadeResolution()) {
            fprintf(stderr, "full screen: the screen has no %dx%d mode near %.1f Hz; full screen stays at its own resolution\n",
                    Video::kWidth, Video::kHeight, Machine::kFramesPerSecond);
        }
        SDL_SetWindowFullscreenMode(window, nullptr);
    }
    // The window in the middle of the chosen screen
    void CentreOnScreen(SDL_Window *window) {
        const SDL_DisplayID display = ChosenDisplay();
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    }

    // Full screen as the Display page's "Full screen as" says (BorderlessFullScreen, issue #20): SDL's full screen, or
    // a window with no edges as large as the chosen screen (for what needs a window: NVIDIA RTX HDR, overlays). Where
    // and how large the window was before is kept, to put it back
    struct Borderless {
        bool on = false, maximized = false;
        int x = 0, y = 0, width = 0, height = 0;
    } g_borderless;
    bool IsFullScreen(SDL_Window *window) {
        return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) || g_borderless.on;
    }
    // (the window made edgeless and as large as the chosen screen, all of it)
    void CoverScreen(SDL_Window *window) {
        SDL_Rect bounds;
        if (!SDL_GetDisplayBounds(ChosenDisplay(), &bounds)) {
            fprintf(stderr, "borderless window: %s\n", SDL_GetError());
            return;
        }
        SDL_SetWindowBordered(window, false);
        SDL_SetWindowPosition(window, bounds.x, bounds.y);
        SDL_SetWindowSize(window, bounds.w, bounds.h);
        SDL_SyncWindow(window);
    }
    // Full screen (`full`) or a window again, either way as chosen now; a full screen of the other way becomes this one
    void GoFullScreen(SDL_Window *window, bool full) {
        if (full == IsFullScreen(window) && (!full || BorderlessFullScreen() == g_borderless.on)) {
            return;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) {
            SDL_SetWindowFullscreen(window, false);
            SDL_SyncWindow(window);
        }
        if (g_borderless.on) {
            g_borderless.on = false;
            SDL_SetWindowBordered(window, true);
            SDL_SetWindowSize(window, g_borderless.width, g_borderless.height);
            SDL_SetWindowPosition(window, g_borderless.x, g_borderless.y);
            if (g_borderless.maximized) {
                SDL_MaximizeWindow(window);
            }
            SDL_SyncWindow(window);
        }
        if (!full) {
            return;
        }
        if (BorderlessFullScreen()) {
            g_borderless.maximized = SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED;
            if (g_borderless.maximized) {
                SDL_RestoreWindow(window);
                SDL_SyncWindow(window);
            }
            SDL_GetWindowPosition(window, &g_borderless.x, &g_borderless.y);
            SDL_GetWindowSize(window, &g_borderless.width, &g_borderless.height);
            g_borderless.on = true;
            CoverScreen(window);
        } else {
            SetFullscreenMode(window);
            SDL_SetWindowFullscreen(window, true);
        }
    }
#else
    bool IsFullScreen(SDL_Window *window) {
        return SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
    }
#endif

    bool KeyBound(SDL_Scancode key) {
        for (int player = 0; player < Players(); player++) {
            for (int action = 0; action < Table().table.count; action++) {
                if (g_bindings.key[player][action] == key) {
                    return true;
                }
            }
        }
        return false;
    }
    bool ButtonBound(SDL_GamepadButton button) {
        return PadInputBound(g_bindings, Table(), {button, -1});
    }

    // The camera's zoom (zoom.h) is the left stick's click held and a shoulder pressed: left for player 1, right for
    // player 2, the right stick's click for both fighters at once; the d-pad (or the stick) moves the camera. While the
    // click is held (and is not the game's), the shoulders and the directions are not the game's. On the keyboard,
    // Left Shift held, Q, E and W, and the arrows.
    bool ZoomHeld(SDL_Gamepad *gp) {
        return Zoom::Possible() && gp && !ButtonBound(SDL_GAMEPAD_BUTTON_LEFT_STICK) && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    }
    bool Shoulder(int button) {
        return button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    }
    bool DPad(int button) {
        return button == SDL_GAMEPAD_BUTTON_DPAD_UP || button == SDL_GAMEPAD_BUTTON_DPAD_DOWN || button == SDL_GAMEPAD_BUTTON_DPAD_LEFT ||
               button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    }
    bool KeyZoomHeld(const bool *keys) {
        return Zoom::Possible() && keys[SDL_SCANCODE_LSHIFT] && !KeyBound(SDL_SCANCODE_LSHIFT);
    }
    bool KeyZoom(SDL_Scancode key) {
        return key == SDL_SCANCODE_Q || key == SDL_SCANCODE_E || key == SDL_SCANCODE_W || key == SDL_SCANCODE_UP || key == SDL_SCANCODE_DOWN ||
               key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_RIGHT;
    }

    uint16_t ReadKeyboard(const bool *keys, int player) {
        uint16_t pad = 0;
        const bool zoomHeld = KeyZoomHeld(keys);
        for (int action = 0; action < Table().table.count; action++) {
            const SDL_Scancode key = g_bindings.key[player][action];
            if (zoomHeld && KeyZoom(key)) {
                continue;
            }
            if (key != SDL_SCANCODE_UNKNOWN && keys[key]) {
                pad |= Table().bits[action];
            }
        }
        // (Return starts player 1's game too, unless it has been given something to do)
        if (player == 0 && keys[SDL_SCANCODE_RETURN] && !KeyBound(SDL_SCANCODE_RETURN)) {
            pad |= kStart;
        }
        return pad;
    }

    uint16_t ReadGamepad(SDL_Gamepad *gp, int player) {
        uint16_t pad = 0;
        const bool zoomHeld = ZoomHeld(gp);
        for (int action = 0; action < Table().table.count; action++) {
            const PadInput input = g_bindings.pad[player][action];
            if (zoomHeld && (Shoulder(input.button) || DPad(input.button))) {
                continue;
            }
            if ((input.button >= 0 && SDL_GetGamepadButton(gp, static_cast<SDL_GamepadButton>(input.button))) ||
                (input.axis >= 0 && SDL_GetGamepadAxis(gp, static_cast<SDL_GamepadAxis>(input.axis)) > kDead)) {
                pad |= Table().bits[action];
            }
        }
        const int x = zoomHeld ? 0 : SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX);
        const int y = zoomHeld ? 0 : SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
        if (x < -kDead) pad |= kLeft;
        if (x > kDead) pad |= kRight;
        if (y < -kDead) pad |= kUp;
        if (y > kDead) pad |= kDown;
        return pad;
    }

    // A Multi game run's window: where the game before left it (each game is a program of its own, and its window
    // would open where the system puts a new one). The game ending for the next writes it down; the next game reads
    // it, if it was written moments ago (not a run before), and opens its window there, as tall.
    struct Placement {
        int x = 0, y = 0, height = 0;
        bool maximized = false;
    };
    std::filesystem::path PlacementFile() {
        std::error_code error;
        const std::filesystem::path folder = std::filesystem::temp_directory_path(error);
        return error ? std::filesystem::path() : folder / "sf3comp-multi-window.txt";
    }
    void KeepPlacement(SDL_Window *window) {
        const std::filesystem::path file = PlacementFile();
        if (file.empty() || IsFullScreen(window)) {
            return;
        }
        Placement at;
        int width = 0;
        SDL_GetWindowPosition(window, &at.x, &at.y);
        SDL_GetWindowSize(window, &width, &at.height);
        at.maximized = SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED;
        std::ofstream(file) << at.x << ' ' << at.y << ' ' << at.height << ' ' << at.maximized << '\n';
    }
    bool TakePlacement(Placement &at) {
        const std::filesystem::path file = PlacementFile();
        std::error_code error;
        if (file.empty() || !std::filesystem::exists(file, error)) {
            return false;
        }
        const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(file, error);
        bool fresh = !error && age < std::chrono::minutes(1);
        {
            std::ifstream in(file);
            fresh = fresh && (in >> at.x >> at.y >> at.height >> at.maximized) && at.height > 0;
        }
        std::filesystem::remove(file, error);
        return fresh;
    }

    // (ForkAde) The menu bar along the top of the window, as the CPS games have it (recomp-common's menubar.h): the
    // in-game menu's rows, features and commands in its Game, Video, Input, Features and Help menus (Menu::
    // AddBarEntries), with the window's own entries: quick save and load, the screenshot, Game > Replay, full screen
    enum { kBarQuickSave = 1, kBarQuickLoad, kBarFullscreen, kBarAbout, kBarKey, kBarScreenshot, kBarRecordInput, kBarPlayInput, kBarStopReplay };
    const char *const kBarKeys[][2] = {
        {"In-game menu", "Esc, F1"}, {"Menu bar", "F10, Alt"}, {"Quick save", "F5"}, {"Quick load", "F9"}, {"Slot before, next", "F6, F7"},
        {"Screenshot", "F12"}, {"Fullscreen", "F11"}, {"Test switch", "F2"}, {"Pause", "P"}, {"One frame", "O"},
        {"Slower, faster", "-, ="}, {"Restart", "Backspace"},
    };
    recomp::BarEntry BarItem(const char *label, int id, const char *shortcut = "", bool enabled = true) {
        recomp::BarEntry e;
        e.label = label;
        e.id = id;
        e.shortcut = shortcut;
        e.enabled = enabled;
        return e;
    }
    // (SDL's file dialogs answer on their own thread: the file chosen waits here for the window's loop)
    struct ReplayDialog {
        std::mutex lock;
        int kind = 0;
        std::string path;
        bool answered = false;
    };
    ReplayDialog replayDialog;
    void SDLCALL ReplayDialogDone(void *userdata, const char *const *files, int) {
        std::lock_guard<std::mutex> hold(replayDialog.lock);
        replayDialog.kind = static_cast<int>(reinterpret_cast<intptr_t>(userdata));
        replayDialog.path = files && files[0] ? files[0] : "";
        replayDialog.answered = true;
    }
    const SDL_DialogFileFilter kReplayFilter[] = {{"Replays (.s3r)", "s3r"}};

    void BarMenus(Port &port, recomp::MenuBar &bar, bool fullscreen, std::vector<recomp::BarMenu> *copy = nullptr) {
        std::vector<recomp::BarMenu> menus = {{"Game", {}}, {"Video", {}}, {"Audio", {}}, {"Input", {}}, {"Features", {}}, {"Help", {}}};
        std::error_code error;
        recomp::BarOptions options;
        options.slotUsed = [&](int slot) { return std::filesystem::exists(TextPath(port.StatePath(slot)), error); };
        port.menu.AddBarEntries(menus, true, options);
        const bool online = port.net != nullptr || port.gekkoOnline;
        std::vector<recomp::BarEntry> &game = menus[0].entries;
        auto slot = std::find_if(game.begin(), game.end(), [](const recomp::BarEntry &e) { return e.label == "Slot"; });
        const size_t after = slot == game.end() ? 0 : static_cast<size_t>(slot - game.begin()) + 1;
        game.insert(game.begin() + static_cast<std::ptrdiff_t>(after),
                    {recomp::BarEntry::Line(), BarItem("Quick save", kBarQuickSave, "F5"),
                     BarItem("Quick load", kBarQuickLoad, "F9", options.slotUsed(port.menu.Slot())), BarItem("Screenshot", kBarScreenshot, "F12")});
        // Game > Replay: Record input..., Play input..., Stop (dim online, as the save states are)
        {
            const bool recording = !port.replay.manualPath.empty();
            recomp::BarEntry replay = BarItem("Replay", 0);
            if (port.replay.playing) replay.children.push_back(BarItem("Playing a replay", 0, "", false));
            else if (recording) replay.children.push_back(BarItem("Recording input", 0, "", false));
            replay.children.push_back(BarItem("Record input...", kBarRecordInput, "", !port.replay.playing && !recording));
            replay.children.push_back(BarItem("Play input...", kBarPlayInput));
            replay.children.push_back(BarItem("Stop replay/record", kBarStopReplay, "", port.replay.playing || recording));
            replay.enabled = !online;
            auto shot = std::find_if(game.begin(), game.end(), [](const recomp::BarEntry &e) { return e.id == kBarScreenshot; });
            game.insert(shot == game.end() ? game.end() : shot + 1, std::move(replay));
        }
        std::vector<recomp::BarEntry> &video = menus[1].entries;
        if (!video.empty()) {
            video.push_back(recomp::BarEntry::Line());
        }
        video.push_back(BarItem("Fullscreen", kBarFullscreen, "F11"));
        video.back().mark = fullscreen ? recomp::BarEntry::Check : recomp::BarEntry::NoMark;
        menus[3].entries.push_back(recomp::BarEntry::Line());
        menus[3].entries.push_back(BarItem("Controls: in the launcher", 0, "", false));
        recomp::BarEntry keys = BarItem("Keys", 0);
        for (const auto &key : kBarKeys) {
            keys.children.push_back(BarItem(key[0], kBarKey, key[1]));
        }
        menus[5].entries.push_back(std::move(keys));
        menus[5].entries.push_back(BarItem("About", kBarAbout));
        // Online, what would change the match is dim (as the in-game menu says): the save states, Reset, the features
        if (online) {
            for (recomp::BarMenu &menu : menus) {
                for (recomp::BarEntry &entry : menu.entries) {
                    const std::string &l = entry.label;
                    if (menu.title == "Features" || l == "Save state" || l == "Load state" || l == "Quick save" || l == "Quick load" ||
                        l == "Reset game" || l == "Player 2 joins") {
                        entry.enabled = false;
                    }
                }
            }
        }
        menus.erase(std::remove_if(menus.begin(), menus.end(), [](const recomp::BarMenu &m) { return m.entries.empty(); }), menus.end());
        if (copy) *copy = menus;
        bar.Configure(std::move(menus));
    }
    int BarDot(int windowHeight) {
        // (a window opens the board's lines and the bar's dots tall, so many times: a dot is as many pixels)
        return std::max(1, windowHeight / (Video::kHeight + recomp::MenuBar::kHeightDots));
    }
    int BarStrip(int windowHeight) {
        return recomp::MenuBar::kHeightDots * BarDot(windowHeight);
    }
    // (the bar and its open menus over the whole window, transparent elsewhere: the presenter's overlay)
    void BarPicture(const recomp::MenuBar &bar, int width, int height, std::vector<uint32_t> &pixels) {
        pixels.assign(static_cast<size_t>(std::max(width, 0)) * std::max(height, 0), 0xFF000000u);
        if (width <= 0 || height <= 0) {
            return;
        }
        bar.Draw(pixels.data(), width, height);
        std::vector<SDL_Rect> regions;
        if (bar.BarShown()) {
            regions.push_back({0, 0, width, bar.Height()});
        }
        for (const recomp::MenuBar::Box &box : bar.Boxes()) {
            regions.push_back({static_cast<int>(box.x), static_cast<int>(box.y), static_cast<int>(std::ceil(box.w)), static_cast<int>(std::ceil(box.h))});
        }
        std::vector<uint32_t> drawn(pixels.size(), 0);
        for (const SDL_Rect &r : regions) {
            for (int y = std::max(r.y, 0); y < std::min(r.y + r.h, height); y++) {
                for (int x = std::max(r.x, 0); x < std::min(r.x + r.w, width); x++) {
                    drawn[static_cast<size_t>(y) * width + x] = pixels[static_cast<size_t>(y) * width + x];
                }
            }
        }
        pixels.swap(drawn);
    }

} // namespace

int RunWindowed(Port &port) {
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    if (!port.opt.settingsPath.empty()) {
        g_bindings = LoadBindings(port.opt.settingsPath, Table());
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // (controllers SDL doesn't know, issue #213: gamecontrollerdb.txt beside the program, in the launcher's folder or in
    // the game's own; recomp-common/gamepads.h)
    {
        const char *base = SDL_GetBasePath();
        const std::string mine = port.PlayerFolder();
        recomp::AddGamepadMappings({base ? std::filesystem::u8path(base) : std::filesystem::path(), LauncherFolder(),
                                    mine.empty() ? std::filesystem::path() : std::filesystem::u8path(mine)});
    }
    // (the window: so many times the board's 224 lines, --scale's or the Display page's)
    const int scale = port.opt.scale > 0 ? port.opt.scale : WindowSize();
    // (the window's name: game.cfg's "title" where a game has one, else its name)
#ifdef GAME_TITLE
    const char *title = GAME_TITLE;
#else
    const char *title = GAME_NAME;
#endif
    // (the picture's shape: the board's 384 x 224 shown 4:3 unless the Display page says otherwise, and as
    // much wider as the widest picture the game's sources may ask for; 4:3 for a picture that fills it)
    const int columns = TheGame().widestPicture ? std::clamp(TheGame().widestPicture(), +Video::kWidth, +Video::kMaxWidth) : Video::kWidth;
    // (with the Display page's bezels, 16:9 at least: the picture in the middle and the bezels beside it, issue #8)
    auto windowShape = [](int across) {
        const float picture = PictureShape(across, Video::kHeight, DisplayShape(), 4.0f / 3.0f * across / Video::kWidth);
        return DisplayBezel() != Bezel::Off && DisplayShape() != Shape::Fill ? std::max(picture, 16.0f / 9.0f) : picture;
    };
    // (in a Multi game run, a window where the game before's was: a full screen one is full screen as asked)
    Placement placed;
    const bool carried = !MultiGame().empty() && TakePlacement(placed) && !port.opt.fullscreen;
    // (ForkAde: and the menu bar's strip above the picture)
    const int windowHeight = carried ? placed.height : Video::kHeight * scale + recomp::MenuBar::kHeightDots * scale;
    // The window, as an OpenGL window for a shader (shading.h: nothing else draws to it), `at` a place for it (`width`
    // 0: as wide as the picture's shape makes it)
    auto makeWindow = [&](bool opengl, const Placement *at, int width, int height, bool fullscreen) {
        const SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width > 0 ? width : std::lround(height * windowShape(columns)));
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
        if (at) {
            SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER, at->x);
            SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, at->y);
        } else {
#ifndef __ANDROID__
            // (on the Display page's screen)
            const SDL_DisplayID display = ChosenDisplay();
            SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
            SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
#endif
        }
#ifdef __ANDROID__
        fullscreen = true; // (a phone's whole screen, its bars hidden)
        const bool fullscreenNow = true;
#else
        // (the arcade's resolution: made a window, given its mode, then made full screen; a borderless window: made a
        // window, then made to cover the screen)
        const bool borderless = fullscreen && BorderlessFullScreen();
        const bool fullscreenNow = fullscreen && !ArcadeResolution() && !borderless;
#endif
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                              SDL_WINDOW_RESIZABLE | (fullscreenNow ? SDL_WINDOW_FULLSCREEN : 0) | (at && at->maximized ? SDL_WINDOW_MAXIMIZED : 0) |
                                  (opengl ? SDL_WINDOW_OPENGL : 0) | (getenv("CPS3RECOMP_HIDDEN_WINDOW") ? SDL_WINDOW_HIDDEN : 0)); // (hidden: for tests)
        SDL_Window *made = SDL_CreateWindowWithProperties(properties);
        SDL_DestroyProperties(properties);
#ifndef __ANDROID__
        if (made) {
            SetFullscreenMode(made);
            if (borderless) {
                // (made again, for or without a shader: as large as the screen again, where it was before kept)
                if (g_borderless.on) {
                    CoverScreen(made);
                } else {
                    GoFullScreen(made, true);
                }
            } else if (fullscreen && !fullscreenNow) {
                SDL_SetWindowFullscreen(made, true);
            }
        }
#endif
        // (in front: on macOS a program started by one that is not, as the launcher is behind a Multi game run's
        // last game, opens behind the other windows)
        if (made && !getenv("CPS3RECOMP_HIDDEN_WINDOW")) {
            SDL_RaiseWindow(made);
        }
        return made;
    };
    // The picture goes to the window through SDL's GPU API, or through SDL's renderer where there is no
    // device for that (and the dummy video driver of the tests has none) or when asked: --renderer sdl. With a
    // shader preset chosen (the Display page's Shader, or --shader) it goes through the preset with OpenGL instead,
    // where librashader is beside the game and makes the preset; else as without one.
    Presenter presenter;
    std::string presentError;
    const bool wantGpu = port.opt.renderer != "sdl" && strcmp(SDL_GetCurrentVideoDriver(), "dummy") != 0;
    // (the checks, CPS3RECOMP_WINDOW_OFFSCREEN=1 with SDL's offscreen video driver: no window at all (that driver makes none
    // without OpenGL); the GPU's picture drawn as for one of CPS3RECOMP_WINDOW_SIZE, into a texture that size, read back
    // by CPS3RECOMP_WINDOW_SHOT)
    const bool offscreen = wantGpu && getenv("CPS3RECOMP_WINDOW_OFFSCREEN") != nullptr;
    int checkWidth = 1280, checkHeight = 720;
    if (const char *size = getenv("CPS3RECOMP_WINDOW_SIZE")) {
        sscanf(size, "%dx%d", &checkWidth, &checkHeight);
    }
    const bool canShade = strcmp(SDL_GetCurrentVideoDriver(), "dummy") != 0;
    std::string preset = canShade ? port.ShaderPreset() : "";
    int shaderValue = FeatureValue("display_shader");
    std::string shaderError;
    if (!preset.empty() && !Shading::Available(shaderError)) {
        fprintf(stderr, "shader: %s; the picture is drawn without it\n", shaderError.c_str());
        preset.clear();
    }
    if (offscreen) {
        preset.clear();
    }
    SDL_Window *window = offscreen ? nullptr : makeWindow(!preset.empty(), carried ? &placed : nullptr,
                                                          carried ? 0 : static_cast<int>(std::lround(Video::kHeight * scale * windowShape(columns))),
                                                          windowHeight, port.opt.fullscreen);
    // (the checks, CPS3RECOMP_WINDOW_SIZE=WxH: the window that size, as a full screen of that shape would be)
    if (getenv("CPS3RECOMP_WINDOW_SIZE") && window && checkWidth > 0 && checkHeight > 0) {
        SDL_SetWindowSize(window, checkWidth, checkHeight);
        SDL_SyncWindow(window);
    }
    // (opened as the preset says, or as without one when that fails)
    auto openPresenter = [&]() {
        if (!preset.empty()) {
            if (presenter.OpenShaded(window, preset, shaderError)) {
                return true;
            }
            fprintf(stderr, "shader %s: %s; the picture is drawn without it\n", preset.c_str(), shaderError.c_str());
            preset.clear();
            // (an OpenGL window is drawn to by nothing else: one that is not, where this one is)
            Placement at;
            int width = 0;
            SDL_GetWindowPosition(window, &at.x, &at.y);
            SDL_GetWindowSize(window, &width, &at.height);
            at.maximized = SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED;
            const bool fullscreen = IsFullScreen(window);
            SDL_DestroyWindow(window);
            window = makeWindow(false, &at, width, at.height, fullscreen);
            if (!window) {
                presentError = SDL_GetError();
                return false;
            }
        }
        if (offscreen) {
            return presenter.OpenOffscreen(std::max(checkWidth, 1), std::max(checkHeight, 1), false, presentError);
        }
        return presenter.Open(window, wantGpu, presentError);
    };
    presenter.SetVsync(FeatureOn("display_vsync")); // (kept when it is opened again)
    if ((!window && !offscreen) || !openPresenter()) {
        fprintf(stderr, "SDL window: %s\n", window || offscreen ? presentError.c_str() : SDL_GetError());
        return 1;
    }
    if (wantGpu && !presenter.UsesGpu() && !presenter.Shaded()) {
        fprintf(stderr, "no GPU device (%s): the picture goes through SDL's renderer\n", presentError.c_str());
    }
    auto sayPresenter = [&]() {
        if (presenter.Shaded() && getenv("CPS3RECOMP_SHADER_SHOT")) {
            presenter.Shader()->keepLast = true;
        }
        if (port.opt.stats) {
            fprintf(stderr, "picture: %s\n", presenter.Shaded()  ? ("OpenGL, shader " + preset).c_str()
                                             : presenter.UsesGpu() ? SDL_GetGPUDeviceDriver(presenter.Device())
                                                                   : SDL_GetRendererName(presenter.Renderer()));
        }
    };
    sayPresenter();
    // The Display page's Shader changed in the menu: another preset in the same window, or the window made again
    // for or without OpenGL, where it is and as large
    auto followShader = [&]() {
        const int value = FeatureValue("display_shader");
        if (value == shaderValue || !port.opt.shader.empty() || !canShade) {
            return;
        }
        shaderValue = value;
        std::string next = port.ShaderPreset();
        if (!next.empty() && !Shading::Available(shaderError)) {
            fprintf(stderr, "shader: %s; the picture is drawn without it\n", shaderError.c_str());
            next.clear();
        }
        if (next == preset) {
            return;
        }
        if (!next.empty() && presenter.Shaded()) {
            if (presenter.SetPreset(next, shaderError)) {
                preset = next;
                sayPresenter();
            } else {
                fprintf(stderr, "shader %s: %s; the one before is kept\n", next.c_str(), shaderError.c_str());
            }
            return;
        }
        Placement at;
        int width = 0;
        SDL_GetWindowPosition(window, &at.x, &at.y);
        SDL_GetWindowSize(window, &width, &at.height);
        at.maximized = SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED;
        const bool fullscreen = IsFullScreen(window);
        presenter.Close();
        SDL_DestroyWindow(window);
        preset = next;
        window = makeWindow(!preset.empty(), &at, width, at.height, fullscreen);
        if (!window || !openPresenter()) {
            fprintf(stderr, "SDL window: %s\n", window ? presentError.c_str() : SDL_GetError());
            port.quit = true;
            return;
        }
        sayPresenter();
    };

    // Sound: how much is kept waiting to be played (seconds), and how much is too much. The device is asked
    // for small buffers, so that little waits there either. (CPS3RECOMP_AUDIO_LOG: what is waiting, printed
    // once a second, and nothing heard.)
    constexpr double kAudioWaiting = 0.028, kAudioMost = 0.11;
    const bool audioLog = getenv("CPS3RECOMP_AUDIO_LOG") != nullptr;
    double audioLeast = 1;
    int audioDropped = 0;
    // (--mute starts with no sound; the menu's Sound row switches it on, so the device is opened all the same)
    SDL_AudioStream *audio = nullptr;
    port.menu.muted = port.opt.mute;
    bool silent = false;
    // (for the checks, CPS3RECOMP_AUDIO_SINK=FILE:RATE[:QUANTUM]: no sound device; the stream is taken by the clock as a
    // device at RATE would take it, QUANTUM frames at a time (as PipeWire's quantum), and what it plays written to FILE,
    // 16-bit stereo; a quantum the stream could not fill is counted, and silence written for what was missing.
    // games/sfiii3/checkko.sh hears the knock-out through it at the Steam Machine's 48 kHz and the Mac's 44.1 kHz)
    struct Sink {
        FILE *file = nullptr;
        int rate = 0, quantum = 0;
        uint64_t start = 0, taken = 0, short_ = 0;
        std::vector<int16_t> buffer;
    } sink;
    if (const char *spec = getenv("CPS3RECOMP_AUDIO_SINK")) {
        const std::string s = spec;
        const size_t colon = s.find(':');
        if (colon != std::string::npos) {
            sink.file = fopen(s.substr(0, colon).c_str(), "wb");
            sink.rate = atoi(s.c_str() + colon + 1);
            const size_t second = s.find(':', colon + 1);
            sink.quantum = second == std::string::npos ? 512 : std::max(1, atoi(s.c_str() + second + 1));
        }
        if (!sink.file || sink.rate <= 0) {
            fprintf(stderr, "CPS3RECOMP_AUDIO_SINK: FILE:RATE[:QUANTUM]\n");
            return 1;
        }
    }
    const auto drainSink = [&]() {
        if (!sink.file || !audio) {
            return;
        }
        const uint64_t now = SDL_GetTicksNS();
        if (!sink.start) {
            sink.start = now;
        }
        const uint64_t due = (now - sink.start) * static_cast<uint64_t>(sink.rate) / 1000000000ull / static_cast<uint64_t>(sink.quantum) * static_cast<uint64_t>(sink.quantum);
        sink.buffer.resize(static_cast<size_t>(sink.quantum) * 2);
        for (; sink.taken < due; sink.taken += static_cast<uint64_t>(sink.quantum)) {
            const int want = sink.quantum * 4;
            const int got = std::max(0, SDL_GetAudioStreamData(audio, sink.buffer.data(), want));
            if (got < want) {
                sink.short_++;
                memset(reinterpret_cast<uint8_t *>(sink.buffer.data()) + got, 0, static_cast<size_t>(want - got));
            }
            fwrite(sink.buffer.data(), 1, static_cast<size_t>(want), sink.file);
        }
    };
    {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(Machine::kSampleRate + 0.5)};
        if (sink.file) {
            const SDL_AudioSpec to{SDL_AUDIO_S16, 2, sink.rate};
            audio = SDL_CreateAudioStream(&spec, &to);
        } else {
            audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        }
        if (audio && sink.file) {
            if (audioLog) {
                fprintf(stderr, "audio: a sink of %d Hz, %d frames a quantum\n", sink.rate, sink.quantum);
            }
        } else if (audio) {
            SDL_ResumeAudioStreamDevice(audio);
            if (audioLog) {
                SDL_AudioSpec device{};
                int frames = 0;
                SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(audio), &device, &frames);
                fprintf(stderr, "audio: device %d Hz, %d channels, %d frames a buffer (%s)\n", device.freq, device.channels, frames, SDL_GetCurrentAudioDriver());
            }
        } else {
            fprintf(stderr, "Audio: %s\n", SDL_GetError());
        }
    }

    // The custom soundtrack (soundtrack.h): a stream of its own, at its files' rate, opened when it first plays
    SDL_AudioStream *music = nullptr;
    SDL_AudioSpec musicSpec{SDL_AUDIO_F32, 2, 0};
    std::vector<float> musicMade;
    bool musicSilent = false;

    std::vector<SDL_Gamepad *> gamepads;
    // The in-game menu: opened and closed by Esc, F1, a gamepad stick's click or its Start and Select
    // together (the game is left from its "Quit game"). While it is open the game stands still and the
    // controls work the menu; buttons held as it closes do not reach the game until let go.
    std::vector<uint32_t> menuBase, menuPicture, shown, overlay;
    uint64_t menuVersion = 0, overlayVersion = 0, menuShotVersion = 0; // (NativeGpu::Overlay's)
    uint32_t menuWidth = 0, menuHeight = 0;
    // (the menu's picture's size where it is drawn on the screen's own pixels: Port::MenuPicture; 0: as usual)
    int menuWantWidth = 0, menuWantHeight = 0;
    bool menuDirty = false;
    uint16_t previousPad = 0, heldAtClose = 0;
    recomp::MenuRepeat menuRepeat; // (the arrows held go on through the rows and the choices)
    int triggerHeld = 0;
    bool bothHeld = false; // a gamepad's Start and Select (Back) together: the menu
    // (with a zoom to work, the left stick's click opens the menu when it is let go, unless a shoulder was pressed
    // while it was held: then it was the zoom's)
    bool stickWaiting = false;
    auto toggleMenu = [&]() {
        if (port.gekkoOnline) {
            // (ForkAde's session: no menu, the match goes on; Esc asks whether to leave it, as with Tommy's netplay)
            port.leaving = !port.leaving;
            return;
        }
        if (port.net) {
            // (online: no menu, the game goes on for both players; Esc gives up a wait, and in a match asks whether to
            // leave it: Port::leaving)
            if (port.connecting && !port.net->Arcade()) {
                port.LeaveOnline();
            } else {
                port.leaving = !port.leaving;
            }
            return;
        }
        if (port.menu.IsOpen()) {
            port.menu.Close();
        } else {
            // (the game's picture under the menu: the GPU's, with the menu over it on the GPU, where the last frame was
            // made for it; else the CPU's)
            menuBase.clear();
            if (!port.machine->GpuFrame() || !presenter.CanShowNative()) {
                port.MenuPicture(menuBase, menuWidth, menuHeight, menuWantWidth, menuWantHeight);
            }
            port.menu.Open(true);
            menuDirty = true;
        }
    };
    // Online, the chat (Port::typing): T on the keyboard, the right stick's click or the touch screen's Chat button
    // opens a line to type, and the system's text input with it (on a Steam Deck or a phone, its own on-screen
    // keyboard); Enter sends it, Esc (or the click again) puts it away. Meanwhile the keyboard is the chat's, and after
    // it, not the game's until every key is let go (the Enter that sent it is not the game's Start).
    uint64_t openedByT = 0; // (when T opened it: its own letter, if the system sends it as text too, is not the line's)
    bool keysAfterChat = false;
    // (a match to chat in: Tommy's netplay's once found, or ForkAde's session's, its players only)
    auto chatting = [&]() {
        return (port.net && !port.connecting) ||
               (port.gekkoChat && port.gekkoChat->ChatReady() && !port.gekkoChat->ChatSpectating());
    };
    auto chatOpen = [&](bool open) {
        if (open == port.typing || (open && (!chatting() || port.leaving))) {
            return;
        }
        port.typing = open;
        port.typed.clear();
        if (open) {
            int w = 0, h = 0;
            SDL_GetWindowSize(window, &w, &h);
            const SDL_Rect area{0, h * 3 / 4, w / 2, h / 12};
            SDL_SetTextInputArea(window, &area, 0);
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
            keysAfterChat = true;
        }
    };
    auto quickState = [&](bool save) {
        if (port.net || port.gekkoOnline) {
            return; // (online: no save states)
        }
        std::string error;
        const std::string path = port.StatePath(port.menu.Slot());
        if (!(save ? port.SaveState(path, error) : port.LoadState(path, error))) {
            fprintf(stderr, "state %d: %s\n", port.menu.Slot(), error.c_str());
        }
    };
    // (ForkAde) The menu bar: F10 or Alt, a click on its strip, or a right click on the picture (its menus as a popup).
    // While one of its menus is open the game stands still, as with the in-game menu
    recomp::MenuBar bar;
    std::vector<uint32_t> barPixels;
    bool barDirty = true, barRevealed = false, barPopup = false, barWasOpen = false, barHeldPause = false;
    int barWidth = 0, barHeight = 0;
    auto showBar = [&]() {
        const bool shown = !window || !IsFullScreen(window) || barRevealed || (bar.IsOpen() && !barPopup);
        if (shown != bar.BarShown()) {
            bar.ShowBar(shown);
            barDirty = true;
        }
    };
    auto barOpened = [&](bool popup = false) {
        BarMenus(port, bar, window && IsFullScreen(window));
        barPopup = popup;
        barDirty = true;
    };
    auto barChose = [&](const recomp::MenuBar::Chosen &chosen) {
        barDirty = true;
        heldAtClose = previousPad;
        if (!chosen.chosen) {
            return;
        }
        if (recomp::IsMenuEntry(chosen.id)) {
            port.MenuAct(port.menu.Choose(chosen.id));
        } else if (chosen.id == kBarQuickSave || chosen.id == kBarQuickLoad) {
            quickState(chosen.id == kBarQuickSave);
        } else if (chosen.id == kBarFullscreen && window) {
            GoFullScreen(window, !IsFullScreen(window));
            barRevealed = false;
        } else if (chosen.id == kBarScreenshot) {
            port.Screenshot();
        } else if (chosen.id == kBarAbout) {
            port.menu.Note("Street Fighter III by sf3comp (Tommy McLeroy), with ForkAde's menu bar.");
        } else if ((chosen.id == kBarRecordInput || chosen.id == kBarPlayInput) && window) {
            const std::string folder = port.ReplayFolder();
            if (!folder.empty()) SDL_CreateDirectory(folder.c_str());
            const std::string where = chosen.id == kBarRecordInput ? port.ProposedReplayPath() : folder;
            void *kind = reinterpret_cast<void *>(static_cast<intptr_t>(chosen.id));
            if (chosen.id == kBarRecordInput) SDL_ShowSaveFileDialog(ReplayDialogDone, kind, window, kReplayFilter, 1, where.empty() ? nullptr : where.c_str());
            else SDL_ShowOpenFileDialog(ReplayDialogDone, kind, window, kReplayFilter, 1, where.empty() ? nullptr : where.c_str(), false);
        } else if (chosen.id == kBarStopReplay) {
            port.StopReplayOrRecord();
        }
        BarMenus(port, bar, window && IsFullScreen(window));
    };
    // (a file chosen in a replay dialog: recorded to, or played)
    auto replayDialogs = [&] {
        int kind = 0;
        std::string path;
        {
            std::lock_guard<std::mutex> hold(replayDialog.lock);
            if (!replayDialog.answered) return;
            replayDialog.answered = false;
            kind = replayDialog.kind;
            path = replayDialog.path;
        }
        if (path.empty()) return; // (cancelled)
        if (kind == kBarRecordInput && (path.size() < 4 || path.substr(path.size() - 4) != ".s3r")) path += ".s3r";
        std::string error;
        if (!(kind == kBarRecordInput ? port.RecordInputTo(path, error) : port.PlayInput(path, error))) {
            port.menu.Note(error);
            fprintf(stderr, "replay: %s\n", error.c_str());
        }
        barDirty = true;
    };
    // (the pointer's place in the window's pixels: events give it in the window's points)
    auto pixelsOf = [&](float x, float y, float &px, float &py) {
        const float density = window && SDL_GetWindowPixelDensity(window) > 0 ? SDL_GetWindowPixelDensity(window) : 1.0f;
        px = x * density;
        py = y * density;
    };
    bool altAlone = false;
    // (the checks, CPS3RECOMP_BAR_SHOT=TICK:FILE: at that loop's pass the bar's Game menu is opened at its Replay
    // submenu, drawn to FILE (PNG) and its entries listed)
    const char *barShot = getenv("CPS3RECOMP_BAR_SHOT");
    const uint64_t barShotAt = barShot ? strtoull(barShot, nullptr, 10) : 0;
    uint64_t barLoops = 0;
    auto barShotNow = [&] {
        if (!barShot || ++barLoops != barShotAt || !window) return;
        const char *file = strchr(barShot, ':');
        std::vector<recomp::BarMenu> menus;
        BarMenus(port, bar, IsFullScreen(window), &menus);
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        bar.SetArea(width, height, BarDot(height));
        bar.OpenMenu(0);
        int downs = 0;
        bool found = false;
        for (const recomp::BarEntry &e : menus[0].entries) {
            if (e.label == "Replay") { found = e.enabled; break; }
            if (!e.IsLine() && e.enabled) downs++;
        }
        for (int i = 0; found && i < downs; i++) bar.Buttons(recomp::kMenuDown);
        if (found) bar.Buttons(recomp::kMenuRight);
        for (const recomp::BarMenu &menu : menus) fprintf(stderr, "bar: %s (%zu entries)\n", menu.title.c_str(), menu.entries.size());
        for (const recomp::BarEntry &e : menus[0].entries)
            if (e.label == "Replay")
                for (const recomp::BarEntry &c : e.children) fprintf(stderr, "bar: Replay > %s%s\n", c.label.c_str(), c.enabled ? "" : " (dim)");
        std::vector<uint32_t> pixels;
        BarPicture(bar, width, height, pixels);
        if (file && WritePng(file + 1, pixels.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height)))
            fprintf(stderr, "bar: drawn to %s\n", file + 1);
        bar.Close();
        barDirty = true;
    };
    // A Multi game run (the launcher's Multi game page): N or a gamepad's left shoulder for the next game, F or
    // the left trigger held to fast-forward (each when the player has not given it to the game)
    const bool multi = !MultiGame().empty();
    // (a touch screen's controls: touch.h)
    TouchControls touch;
    std::vector<uint32_t> touchPicture;
    // (the menu's Zoom row, as last seen: a change there is the camera's, and a zoom's press is the row's)
    int zoomRow = port.zoom.Choice();
    const double kFrameNs = 1e9 / Machine::kFramesPerSecond;
    uint64_t nextFrame = SDL_GetTicksNS();
    // (ForkAde) Its GekkoNet session (online.h): it runs the frames, with this side's controls as its player's
    Online session;
    const bool sessionOn = port.opt.gekko.mode != OnlineOptions::Mode::Off;
    if (sessionOn) {
        std::string error;
        if (!session.Start(port, port.opt.gekko, error)) {
            fprintf(stderr, "online: %s\n", error.c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, ("Online: " + error).c_str(), window);
            return 1;
        }
        const OnlineOptions::Mode mode = port.opt.gekko.mode;
        const char *who = mode == OnlineOptions::Mode::Host ? "player 1" : mode == OnlineOptions::Mode::Join ? "player 2"
                        : mode == OnlineOptions::Mode::Watch ? "watching" : "stress test";
        SDL_SetWindowTitle(window, (std::string(title) + " - online, " + who).c_str());
    }
    bool running = true;
    while (running && !port.quit && (port.opt.frames == 0 || port.frame < port.opt.frames)) {
        SDL_Event ev;
        if (cps3recomp::TakeMenuAsk() && !port.menu.IsOpen() && !port.net) {
            toggleMenu(); // (the game's sources asked for it: a mode over)
        }
        // (the checks, CPS3RECOMP_MENU_SCRIPT=FRAME:KEYS[;FRAME:KEYS...]: from that frame on, the in-game menu worked as a
        // player works it, a key a pass of this loop: m opens or closes it, u d l r up, down, left, right, a confirms, b
        // goes back: games/sfiii3/checkwide.sh, widescreen switched in the menu)
        if (static const char *script = getenv("CPS3RECOMP_MENU_SCRIPT"); script && !port.net) {
            static std::string left = script;
            static size_t at = std::string::npos;
            const uint64_t from = strtoull(left.c_str(), nullptr, 10);
            const size_t colon = left.find(':'), end = std::min(left.find(';'), left.size());
            if (!left.empty() && colon != std::string::npos && port.frame >= from) {
                if (at == std::string::npos) {
                    at = colon + 1;
                }
                if (at < end) {
                    static const std::string kKeys = "udlrab";
                    const char key = left[at++];
                    if (key == 'm') {
                        toggleMenu();
                    } else if (port.menu.IsOpen() && kKeys.find(key) != std::string::npos) {
                        port.MenuAct(port.menu.Press(1u << kKeys.find(key)));
                        menuDirty = true;
                    }
                }
                if (at >= end) {
                    left = end < left.size() ? left.substr(end + 1) : "";
                    at = std::string::npos;
                }
            }
        }
        if (port.typing && !chatting()) {
            chatOpen(false); // (the match over, or waited for again)
        }
        replayDialogs();
        barShotNow();
        if (bar.IsOpen() != barWasOpen) {
            // (ForkAde: the game stands still while the bar's menu is open, and goes on as it was after; online it goes on)
            barWasOpen = bar.IsOpen();
            if (!port.net && !port.gekkoOnline) {
                if (barWasOpen) {
                    barHeldPause = port.paused;
                    port.paused = true;
                } else {
                    port.paused = barHeldPause;
                }
            }
        }
        while (SDL_PollEvent(&ev)) {
            if (touch.Event(ev)) {
                continue;
            }
            if (port.typing && ev.type == SDL_EVENT_TEXT_INPUT) {
                if (openedByT && SDL_GetTicks() - openedByT < 250 && (!strcmp(ev.text.text, "t") || !strcmp(ev.text.text, "T"))) {
                    openedByT = 0;
                    continue;
                }
                openedByT = 0;
                for (const char *c = ev.text.text; *c; c++) {
                    if (*c >= 32 && *c < 127 && port.typed.size() < Netplay::kChatMost) {
                        port.typed += *c;
                    }
                }
                continue;
            }
            if (port.typing && ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.scancode == SDL_SCANCODE_RETURN || ev.key.scancode == SDL_SCANCODE_KP_ENTER) {
                    port.Say();
                    chatOpen(false);
                } else if (ev.key.scancode == SDL_SCANCODE_ESCAPE || ev.key.scancode == SDL_SCANCODE_AC_BACK) {
                    chatOpen(false);
                } else if (ev.key.scancode == SDL_SCANCODE_BACKSPACE && !port.typed.empty()) {
                    port.typed.pop_back();
                }
                continue; // (every other key the line's, as text)
            }
            if ((ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) || ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                port.LagPressed(ev.common.timestamp); // (the input lag readout: Port::Lag)
            }
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_MOUSE_MOTION: {
                float x, y;
                pixelsOf(ev.motion.x, ev.motion.y, x, y);
                if (window && IsFullScreen(window)) {
                    // (at the top edge the bar comes; below it, with none of its menus open, it goes)
                    if (y < 4) {
                        barRevealed = true;
                    } else if (y >= bar.Height() && !bar.IsOpen()) {
                        barRevealed = false;
                    }
                    showBar();
                }
                if (bar.IsOpen() && !port.menu.IsOpen()) {
                    barDirty = bar.Pointer(x, y) || barDirty;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (!port.menu.IsOpen()) {
                    float x, y;
                    pixelsOf(ev.button.x, ev.button.y, x, y);
                    if (ev.button.button == SDL_BUTTON_LEFT && (bar.IsOpen() || bar.Hit(x, y))) {
                        if (!bar.IsOpen()) {
                            barOpened();
                        }
                        barChose(bar.Click(x, y));
                    } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                        barOpened(true);
                        bar.OpenPopup(x, y);
                    } else if (bar.IsOpen()) {
                        bar.Close();
                        barDirty = true;
                    }
                }
                break;
            case SDL_EVENT_KEY_UP:
                if ((ev.key.scancode == SDL_SCANCODE_LALT || ev.key.scancode == SDL_SCANCODE_RALT) && altAlone &&
                    !KeyBound(ev.key.scancode) && !port.menu.IsOpen()) {
                    if (bar.IsOpen()) {
                        bar.Close();
                        barDirty = true;
                    } else {
                        barOpened();
                        bar.OpenMenu(0);
                    }
                }
                altAlone = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!ev.key.repeat) {
                    altAlone = ev.key.scancode == SDL_SCANCODE_LALT || ev.key.scancode == SDL_SCANCODE_RALT;
                }
                if (bar.IsOpen() && !port.menu.IsOpen()) {
                    // (the bar's menu open: Esc closes a level of it, F10 the bar; the arrows, Enter and Backspace work
                    // it below, as the in-game menu's do; nothing reaches the game)
                    if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                        barChose(bar.Buttons(recomp::kMenuBack));
                    } else if (ev.key.scancode == SDL_SCANCODE_F10) {
                        bar.Close();
                        barDirty = true;
                    }
                    break;
                }
                if (ev.key.scancode == SDL_SCANCODE_F10 && !KeyBound(SDL_SCANCODE_F10) && !port.menu.IsOpen() && !ev.key.repeat) {
                    barOpened();
                    bar.OpenMenu(0);
                    break;
                }
                if (!port.menu.IsOpen() && !ev.key.repeat &&
                    (ev.key.scancode == SDL_SCANCODE_Q || ev.key.scancode == SDL_SCANCODE_E || ev.key.scancode == SDL_SCANCODE_W) &&
                    KeyZoomHeld(SDL_GetKeyboardState(nullptr))) {
                    if (ev.key.scancode == SDL_SCANCODE_W) {
                        port.zoom.Both();
                    } else {
                        port.zoom.Press(ev.key.scancode == SDL_SCANCODE_E ? 1 : 0);
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_ESCAPE || ev.key.scancode == SDL_SCANCODE_F1 || ev.key.scancode == SDL_SCANCODE_AC_BACK ||
                           (ev.key.scancode == SDL_SCANCODE_TAB && !KeyBound(SDL_SCANCODE_TAB))) {
                    // (AC_BACK: a phone's Back)
                    toggleMenu();
                } else if (KeyBound(ev.key.scancode)) {
                    // (a key the player has given to the game does only that)
                } else if (chatting() && ev.key.scancode == SDL_SCANCODE_T && !ev.key.repeat) {
                    chatOpen(true);
                    openedByT = port.typing ? SDL_GetTicks() : 0;
                } else if (multi && !port.menu.IsOpen() && ev.key.scancode == SDL_SCANCODE_N && !ev.key.repeat) {
                    port.Key('n');
                } else if (ev.key.scancode == SDL_SCANCODE_F5 || ev.key.scancode == SDL_SCANCODE_F9) {
                    // F5 keeps the game in the slot, F9 goes back to it; F6 / F7: the slot before, the next
                    if (!port.menu.IsOpen()) {
                        quickState(ev.key.scancode == SDL_SCANCODE_F5);
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_F6 || ev.key.scancode == SDL_SCANCODE_F7) {
                    port.menu.SetSlot(port.menu.Slot() + (ev.key.scancode == SDL_SCANCODE_F7 ? 1 : -1));
                } else if (!port.menu.IsOpen() && ev.key.scancode == SDL_SCANCODE_P && !ev.key.repeat) {
                    port.Key('p'); // stops the game, and starts it again
                } else if (!port.menu.IsOpen() && ev.key.scancode == SDL_SCANCODE_O) {
                    port.Key('o'); // on by one frame (held: frame after frame)
                } else if (ev.key.scancode == SDL_SCANCODE_MINUS || ev.key.scancode == SDL_SCANCODE_EQUALS) {
                    port.Key(ev.key.scancode == SDL_SCANCODE_MINUS ? '-' : '='); // slower, faster
                } else if (!port.menu.IsOpen() && ev.key.scancode == SDL_SCANCODE_BACKSPACE && !ev.key.repeat) {
                    port.Key('k', kKeyRestart);
                } else if (!port.menu.IsOpen() && ev.key.scancode == SDL_SCANCODE_BACKSLASH && !ev.key.repeat) {
                    port.Key('k', kKeyExtra);
                } else if (ev.key.scancode == SDL_SCANCODE_F12 && !ev.key.repeat) {
                    port.Screenshot();
                } else if (ev.key.scancode == SDL_SCANCODE_F2) {
                    port.test = !port.test; // the operator's test switch
                } else if (ev.key.scancode == SDL_SCANCODE_F11 ||
                           (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT))) {
#ifdef __ANDROID__
                    SDL_SetWindowFullscreen(window, !IsFullScreen(window));
#else
                    GoFullScreen(window, !IsFullScreen(window));
#endif
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (!port.menu.IsOpen() && Shoulder(ev.gbutton.button) && ZoomHeld(SDL_GetGamepadFromID(ev.gbutton.which))) {
                    port.zoom.Press(ev.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER ? 1 : 0);
                    stickWaiting = false;
                } else if (!port.menu.IsOpen() && ev.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK &&
                           !ButtonBound(SDL_GAMEPAD_BUTTON_RIGHT_STICK) && ZoomHeld(SDL_GetGamepadFromID(ev.gbutton.which))) {
                    port.zoom.Both(); // (not the right stick's own pause)
                    stickWaiting = false;
                } else if (ButtonBound(static_cast<SDL_GamepadButton>(ev.gbutton.button))) {
                    // (a button the player has given to the game does only that)
                } else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK && Zoom::Possible() && !port.menu.IsOpen()) {
                    stickWaiting = true;
                } else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK || ev.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
                    toggleMenu();
                } else if (port.menu.IsOpen()) {
                    break;
                } else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
                    if (port.net || port.gekkoOnline) {
                        chatOpen(!port.typing);
                    } else {
                        port.Key('p');
                    }
                } else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
                    port.Key(multi ? 'n' : 'k', kKeyRestart);
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK && stickWaiting) {
                    stickWaiting = false;
                    if (!port.menu.IsOpen()) {
                        toggleMenu();
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad *gp = SDL_OpenGamepad(ev.gdevice.which)) {
                    gamepads.push_back(gp);
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (auto it = gamepads.begin(); it != gamepads.end(); ++it) {
                    if (SDL_GetGamepadID(*it) == ev.gdevice.which) {
                        SDL_CloseGamepad(*it);
                        gamepads.erase(it);
                        break;
                    }
                }
                break;
            default: break;
            }
        }

        if (touch.TakeMenuPress()) {
            toggleMenu();
        }
        port.PumpHost(); // (a match hosted from the menu, listened for, the menu open or not)
        touch.Chat(port.net != nullptr || port.gekkoOnline); // (online, from the start or a match hosted from the menu)
        if (touch.TakeZoomPress()) {
            if (port.net || port.gekkoOnline) {
                chatOpen(!port.typing);
            } else {
                port.zoom.Both();
            }
        }
        if (Zoom::Possible()) {
            const int asked = FeatureValue("zoom");
            if (asked != zoomRow) {
                port.zoom.Choose(asked);
            }
            zoomRow = port.zoom.Choice();
            if (asked != zoomRow) {
                SetFeatureForNow("zoom", zoomRow);
                menuDirty = true;
            }
        }
        if (port.opt.moviePlay.empty() && !port.opt.autoPlay) {
            // The keyboard is every player's (each has the keys chosen for it); the first gamepad is player
            // 1's, a second player 2's, and so on
            uint16_t pads[Machine::kMaxPlayers] = {};
            int keyCount = 0;
            const bool *keys = SDL_GetKeyboardState(&keyCount);
            if (keysAfterChat) {
                keysAfterChat = std::any_of(keys, keys + keyCount, [](bool down) { return down; });
            }
            if (!(SDL_GetModState() & SDL_KMOD_ALT) && !port.typing && !keysAfterChat) {
                for (int player = 0; player < Players(); player++) {
                    pads[player] |= ReadKeyboard(keys, player);
                }
                port.service = keys[SDL_SCANCODE_9] && !KeyBound(SDL_SCANCODE_9);
            }
            bool hurry = multi && keys[SDL_SCANCODE_F] && !KeyBound(SDL_SCANCODE_F);
            // (the camera moved by the arrows with Left Shift held, and by each gamepad's d-pad or stick with its click held)
            if (!port.menu.IsOpen()) {
                int across = 0, down = 0;
                if (KeyZoomHeld(keys) && !(SDL_GetModState() & SDL_KMOD_ALT)) {
                    across += keys[SDL_SCANCODE_RIGHT] - keys[SDL_SCANCODE_LEFT];
                    down += keys[SDL_SCANCODE_DOWN] - keys[SDL_SCANCODE_UP];
                }
                for (SDL_Gamepad *gp : gamepads) {
                    if (ZoomHeld(gp)) {
                        const int x = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
                        const int right = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > kDead;
                        const int left = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -kDead;
                        const int lower = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > kDead;
                        const int higher = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -kDead;
                        across += right - left;
                        down += lower - higher;
                        // (the click was the camera's, not the menu's, once it has been moved with it)
                        if (right || left || lower || higher) {
                            stickWaiting = false;
                        }
                    }
                }
                port.zoom.Pan(std::clamp(across, -1, 1), std::clamp(down, -1, 1));
            }
            pads[0] |= touch.Read(port.menu.IsOpen());
            bool trigger = false, both = false;
            const bool stepOnTrigger = !PadInputBound(g_bindings, Table(), {-1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER});
            for (size_t i = 0; i < gamepads.size() && static_cast<int>(i) < Players(); i++) {
                pads[i] |= ReadGamepad(gamepads[i], static_cast<int>(i));
                const bool left = stepOnTrigger && SDL_GetGamepadAxis(gamepads[i], SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kDead;
                trigger = trigger || left;
                hurry = hurry || (multi && left);
                // (while the game runs, the left trigger is its player's Tag: kTag; in a Multi game run, fast forward)
                if (left && !port.paused && !multi) {
                    pads[i] |= kTag;
                }
                both = both || (SDL_GetGamepadButton(gamepads[i], SDL_GAMEPAD_BUTTON_START) &&
                                SDL_GetGamepadButton(gamepads[i], SDL_GAMEPAD_BUTTON_BACK));
            }
            // The left trigger goes on by one frame (held: frame after frame, after a moment), once the game
            // has been stopped: it does not stop it, so a touch of it while playing does nothing
            port.hurry = hurry && !port.menu.IsOpen();
            triggerHeld = trigger && port.paused ? triggerHeld + 1 : 0;
            if (!port.menu.IsOpen() && (triggerHeld == 1 || (triggerHeld > 24 && triggerHeld % 4 == 0))) {
                port.Key('o');
            }
            uint16_t all = 0;
            for (const uint16_t one : pads) {
                all |= one;
            }
            uint16_t pressed = all & ~previousPad;
            previousPad = all;
            // Start and Select held together open and close the menu, whatever they are bound to (the second
            // of them to go down is not also a press in the menu; neither reaches the game until let go)
            if (both && !bothHeld) {
                toggleMenu();
                pressed = 0;
                heldAtClose = previousPad;
            }
            bothHeld = both;
            // The menu's buttons are the same whatever the game's are set to: the arrows, the d-pad and the
            // left stick (and the game's own directions); Space, Enter and A choose; Backspace and B go back
            unsigned menuNow = 0;
            menuNow |= (previousPad & kUp) || keys[SDL_SCANCODE_UP] ? recomp::kMenuUp : 0;
            menuNow |= (previousPad & kDown) || keys[SDL_SCANCODE_DOWN] ? recomp::kMenuDown : 0;
            menuNow |= (previousPad & kLeft) || keys[SDL_SCANCODE_LEFT] ? recomp::kMenuLeft : 0;
            menuNow |= (previousPad & kRight) || keys[SDL_SCANCODE_RIGHT] ? recomp::kMenuRight : 0;
            menuNow |= keys[SDL_SCANCODE_SPACE] || (keys[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & SDL_KMOD_ALT)) ? recomp::kMenuConfirm : 0;
            menuNow |= keys[SDL_SCANCODE_BACKSPACE] ? recomp::kMenuBack : 0;
            for (SDL_Gamepad *gp : gamepads) {
                menuNow |= SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP) ? recomp::kMenuUp : 0;
                menuNow |= SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? recomp::kMenuDown : 0;
                menuNow |= SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? recomp::kMenuLeft : 0;
                menuNow |= SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? recomp::kMenuRight : 0;
                menuNow |= SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH) ? recomp::kMenuConfirm : 0;
                menuNow |= SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST) ? recomp::kMenuBack : 0;
            }
            menuNow |= touch.Menu();
            unsigned menuPressed = menuRepeat.Step(menuNow);
            if (both) {
                menuPressed = 0;
            }
            if (bar.IsOpen() && !port.menu.IsOpen()) {
                // (ForkAde: the bar's menu, worked as the in-game menu is; the game's controls let go meanwhile)
                if (menuPressed) {
                    barChose(bar.Buttons(menuPressed));
                }
                heldAtClose = previousPad;
            } else if (port.menu.IsOpen()) {
                if (menuPressed) {
                    port.MenuAct(port.menu.Press(menuPressed));
                    menuDirty = true;
                } else if (pressed) {
                    port.menu.Note(""); // (a press of something else still takes the note away)
                    menuDirty = true;
                }
                if (!port.menu.IsOpen()) {
                    heldAtClose = previousPad;
                }
            } else if (port.leaving) {
                // (online, "Leave the match?": Confirm leaves it, Back stays; this side's player stands still meanwhile)
                if (menuPressed & recomp::kMenuConfirm) {
                    if (port.gekkoOnline) {
                        session.Stop(); // (ForkAde's session: leaving the match ends the game, as the launcher started it)
                        port.ChatTick();
                        port.quit = true;
                    } else {
                        port.LeaveOnline();
                    }
                } else if (menuPressed & recomp::kMenuBack) {
                    port.leaving = false;
                }
                heldAtClose = previousPad;
            }
            heldAtClose &= previousPad;
            for (int player = 0; player < Machine::kMaxPlayers; player++) {
                port.pad[player] = pads[player] & ~heldAtClose;
            }
        }

        // The window takes the picture's shape again when that changes (widescreen, or the Display page's
        // shape), and its size when the Display page's size does: a window only, not full screen or maximized
        {
            static int shownColumns = columns, shownSize = scale;
            static Shape shownShape = DisplayShape();
            static bool shownBezels = DisplayBezel() != Bezel::Off;
            const int now = TheGame().widestPicture ? std::clamp(TheGame().widestPicture(), +Video::kWidth, +Video::kMaxWidth) : columns;
            const int size = port.opt.scale > 0 ? port.opt.scale : WindowSize();
            const bool bezels = DisplayBezel() != Bezel::Off;
            if (now != shownColumns || DisplayShape() != shownShape || size != shownSize || bezels != shownBezels) {
                if (!IsFullScreen(window) && !(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)) {
                    int windowWidth = 0, windowHeight = 0;
                    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
                    // (the picture's part: below the menu bar's strip)
                    int pictureHeight = windowHeight - recomp::MenuBar::kHeightDots * std::max(1, windowHeight / (Video::kHeight + recomp::MenuBar::kHeightDots));
                    if (size != shownSize) {
                        pictureHeight = Video::kHeight * size;
                        windowHeight = pictureHeight + recomp::MenuBar::kHeightDots * size;
                    }
                    SDL_SetWindowSize(window, static_cast<int>(std::lround(pictureHeight * windowShape(now))), windowHeight);
                }
                shownColumns = now;
                shownShape = DisplayShape();
                shownSize = size;
                shownBezels = bezels;
            }
        }
        followShader();
#ifndef __ANDROID__
        // The Display page's screen or full screen mode changed: the window to that screen, full screen in that mode
        if (window) {
            static int shownScreen = DisplayScreen();
            static bool shownArcade = ArcadeResolution(), shownBorderless = BorderlessFullScreen();
            if (DisplayScreen() != shownScreen || ArcadeResolution() != shownArcade || BorderlessFullScreen() != shownBorderless) {
                const bool full = IsFullScreen(window);
                GoFullScreen(window, false);
                if (DisplayScreen() != shownScreen) {
                    CentreOnScreen(window);
                }
                SetFullscreenMode(window);
                GoFullScreen(window, full);
                SDL_SyncWindow(window);
                shownScreen = DisplayScreen();
                shownArcade = ArcadeResolution();
                shownBorderless = BorderlessFullScreen();
            }
        }
#endif
        presenter.SetVsync(FeatureOn("display_vsync")); // (the Display page's VSync, as changed in the menu)
        port.gpuPresent = presenter.CanShowNative(); // (the frames made for the GPU from here: Machine::gpuPicture)
        if (port.quit) {
            break;
        }
        // The picture in the window as the Display page says (present.h)
        int width = 0, height = 0;
        presenter.Size(width, height);
        presenter.SetScaling(DisplayScaling());
        // (full screen in the arcade's resolution: the picture fills the screen's mode, whose shape the monitor gives it)
        const bool arcadeScreen = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) && SDL_GetWindowFullscreenMode(window);
        // (ForkAde: in a window, below the menu bar's strip; full screen, the whole screen)
        const int barStrip = (window && IsFullScreen(window)) || arcadeScreen ? 0 : BarStrip(height);
        SDL_FRect where = FitPicture(width, height - barStrip, port.ShownWidth(), Video::kHeight, arcadeScreen ? Shape::Fill : DisplayShape(), DisplayScaling());
        where.y += barStrip;
        // (the Display page's bezels under the picture: made again when they, the window's size or the picture's place
        // change; none on the arcade monitor, which the picture fills)
        {
            static Bezel madeBezel = Bezel::Off;
            static int madeWidth = 0, madeHeight = 0;
            static SDL_FRect madeWhere = {};
            const Bezel bezel = arcadeScreen ? Bezel::Off : DisplayBezel();
            if (bezel != madeBezel || width != madeWidth || height != madeHeight || where.x != madeWhere.x || where.y != madeWhere.y ||
                where.w != madeWhere.w || where.h != madeWhere.h) {
                std::vector<uint32_t> bezelPixels;
                int bezelWidth = 0, bezelHeight = 0;
                if (MakeBezel(bezel, width, height, where, port.BezelFolder(), bezelPixels, bezelWidth, bezelHeight)) {
                    presenter.SetBackdrop(bezelPixels.data(), bezelWidth, bezelHeight);
                } else {
                    presenter.SetBackdrop(nullptr, 0, 0);
                }
                madeBezel = bezel;
                madeWidth = width;
                madeHeight = height;
                madeWhere = where;
            }
        }
        // (the menu compact where the Arcade monitor page asks for it, or on the arcade monitor itself; drawn again when
        // that changes)
        if (const bool compact = CompactMenuChosen() || arcadeScreen; compact != port.menu.Compact()) {
            port.menu.SetCompact(compact);
            menuBase.clear();
            menuDirty = true;
        }
        // (the picture shown smaller than the menu's usual 672 lines (a small window, an arcade monitor): the menu drawn
        // on the screen's own pixels there, so that its letters are not shrunk; drawn again when that changes)
        {
            const int wantHeight = static_cast<int>(std::lround(where.h)), wantWidth = static_cast<int>(std::lround(where.w));
            const bool own = wantHeight >= Video::kHeight && wantHeight < 3 * Video::kHeight && wantWidth < NativeGpu::kOverlayWidth - 1;
            if ((own ? wantWidth : 0) != menuWantWidth || (own ? wantHeight : 0) != menuWantHeight) {
                menuWantWidth = own ? wantWidth : 0;
                menuWantHeight = own ? wantHeight : 0;
                menuBase.clear(); // (made again at the new size where the CPU's picture is the menu's)
                menuDirty = true;
            }
        }
        touch.Layout(width, height, where);
        int touchWidth = 0, touchHeight = 0;
        if (touch.TakePicture(touchPicture, touchWidth, touchHeight)) {
            presenter.SetOverlay(touchPicture.empty() ? nullptr : touchPicture.data(), touchWidth, touchHeight);
            barDirty = true; // (a touch screen's controls over it: the bar's picture again when they go)
        }
        showBar();
        if (touchPicture.empty() && (barDirty || width != barWidth || height != barHeight)) {
            bar.SetArea(width, height, BarDot(height));
            BarPicture(bar, width, height, barPixels);
            presenter.SetOverlay(barPixels.data(), width, height);
            barDirty = false;
            barWidth = width;
            barHeight = height;
        }

        if (port.menu.IsOpen()) {
            // The game's picture as it was, with the menu over it: on the GPU (the menu, and what Shown puts over the
            // picture, an overlay of the menu's picture's size over the frame's picture made as large: NativeGpu::Overlay),
            // or the CPU's picture (not through a shader: the menu is drawn three times the board's size, and is not the
            // game's picture)
            const native::Frame *gpuFrame = port.machine->GpuFrame();
            if (gpuFrame && presenter.CanShowNative()) {
                if (menuDirty) {
                    menuDirty = false;
                    port.MenuOverlay(menuPicture, menuWidth, menuHeight, menuWantWidth, menuWantHeight);
                    menuVersion++;
                }
                const NativeGpu::Overlay over = {menuPicture.data(), static_cast<int>(menuWidth), static_cast<int>(menuHeight), false, menuVersion};
                presenter.ShowNative(*gpuFrame, port.machine->GpuVideo(), port.machine->GpuText(), where, &over);
            } else {
                if (menuBase.empty()) {
                    port.MenuPicture(menuBase, menuWidth, menuHeight, menuWantWidth, menuWantHeight); // (the GPU's frame gone meanwhile, or the size changed)
                    menuDirty = true;
                }
                if (menuDirty) {
                    menuDirty = false;
                    menuPicture = menuBase;
                    port.menu.Draw(menuPicture.data(), static_cast<int>(menuWidth), static_cast<int>(menuHeight));
                    menuVersion++;
                }
                presenter.Show(menuPicture.data(), static_cast<int>(menuWidth), static_cast<int>(menuHeight), where, nullptr, false);
            }
            // (the checks, CPS3RECOMP_WINDOW_SHOT: the menu as the window shows it, to PREFIXmenu<frame>.png)
            if (static const char *menuShot = getenv("CPS3RECOMP_WINDOW_SHOT"); menuShot && menuVersion != menuShotVersion) {
                std::vector<uint32_t> pixels;
                int w = 0, h = 0;
                presenter.keepShown = true;
                if (presenter.ReadShown(pixels, w, h)) {
                    const std::string spec = menuShot;
                    char path[600];
                    snprintf(path, sizeof(path), "%smenu%06llu.png", spec.substr(0, spec.rfind(':')).c_str(), static_cast<unsigned long long>(port.frame));
                    WritePng(path, pixels.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                    menuShotVersion = menuVersion;
                }
            }
            SDL_Delay(8);
            nextFrame = SDL_GetTicksNS();
            continue;
        }

        // (the game's time to the screen's: under 1 slowed, over 1 faster than the board)
        const auto rate = port.menu.GameRate();
        const double pace = port.paused || sessionOn ? 1.0 : static_cast<double>(rate.frames) / rate.ticks;
        bool ran;
        if (sessionOn) {
            // (this side's player: the keyboard's players' keys and the gamepads together)
            const uint16_t local[4] = {static_cast<uint16_t>(port.pad[0] | port.pad[1]), 0, 0, 0};
            ran = session.Tick(local) > 0;
            port.tick++;
            if (session.GetStatus() == Online::Status::Ended && port.leaving) {
                port.quit = true;
            }
        } else {
            ran = port.Tick();
        }

        drainSink(); // (the checks' sink: what a device would have taken by now)
        if (audio && silent != (port.menu.muted || audioLog)) {
            silent = port.menu.muted || audioLog;
            SDL_SetAudioStreamGain(audio, silent ? 0.0f : 1.0f);
        }
        std::vector<int16_t> &made = port.machine->audio;
        if (audio && ran && !made.empty()) {
            // The game makes sound at its own pace and the sound device takes it at its own, and the two
            // clocks never quite agree. So the sound is played a touch faster when it is piling up and a
            // touch slower when it is running out (a fraction of a percent, far too little to hear), which
            // keeps a frame or two of it waiting: no gaps, no growing delay.
            // (Slowed, the sound is played slower by as much; faster, faster.)
            double waiting = SDL_GetAudioStreamQueued(audio) / (4.0 * static_cast<int>(Machine::kSampleRate + 0.5)) / pace;
            // (When much more than that is waiting (the game caught up after being held up, say) what waits
            // is thrown away: a click once, rather than sound that stays late for the seconds it would take
            // to play it off.)
            if (waiting > kAudioMost) {
                SDL_ClearAudioStream(audio);
                waiting = 0;
                audioDropped++;
            }
            const double ratio = (1.0 + std::clamp((waiting - kAudioWaiting) * 0.5, -0.02, 0.02)) * pace;
            SDL_SetAudioStreamFrequencyRatio(audio, static_cast<float>(ratio));
            audioLeast = std::min(audioLeast, waiting);
            if (audioLog && port.frame % 60 == 0) {
                fprintf(stderr, "audio: frame %llu, %.0f ms waiting (least %.0f), ratio %.4f, thrown away %d times\n",
                        static_cast<unsigned long long>(port.frame), waiting * 1000, audioLeast * 1000, ratio, audioDropped);
                audioLeast = 1;
            }
            SDL_PutAudioStreamData(audio, made.data(), static_cast<int>(made.size() * sizeof(int16_t)));
        }
        made.clear();
        if (ran && port.soundtrackOn && port.soundtrack.Any()) {
            // (fed as the game's sound is: the game's time's worth each frame, kept a frame or two ahead of the device)
            port.soundtrack.Make(port.machine->soundtrack.state, !FastForward() && !port.hurry, musicMade);
            if (!musicMade.empty()) {
                const int rate = port.soundtrack.Rate();
                if (!music) {
                    musicSpec.freq = rate;
                    music = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &musicSpec, nullptr, nullptr);
                    if (music) {
                        SDL_ResumeAudioStreamDevice(music);
                    } else {
                        fprintf(stderr, "Audio (soundtrack): %s\n", SDL_GetError());
                    }
                } else if (rate != musicSpec.freq) {
                    musicSpec.freq = rate;
                    SDL_SetAudioStreamFormat(music, &musicSpec, nullptr);
                }
            }
            if (music && !musicMade.empty()) {
                if (musicSilent != (port.menu.muted || audioLog)) {
                    musicSilent = port.menu.muted || audioLog;
                    SDL_SetAudioStreamGain(music, musicSilent ? 0.0f : 1.0f);
                }
                double waiting = SDL_GetAudioStreamQueued(music) / (8.0 * musicSpec.freq) / pace;
                if (waiting > kAudioMost) {
                    SDL_ClearAudioStream(music);
                    waiting = 0;
                }
                const double ratio = (1.0 + std::clamp((waiting - kAudioWaiting) * 0.5, -0.02, 0.02)) * pace;
                SDL_SetAudioStreamFrequencyRatio(music, static_cast<float>(ratio));
                SDL_PutAudioStreamData(music, musicMade.data(), static_cast<int>(musicMade.size() * sizeof(float)));
            }
        }

        // The frame drawn by the GPU from the native video interface (Presenter::ShowNative), where the machine made it
        // for the GPU, what Shown puts over it put over it there (an overlay: NativeGpu::Overlay); else the CPU's
        // picture as Shown makes it. Zoomed: the part of the picture the camera looks at, the readout's panels as they
        // are. Either way one command buffer, submitted here
        port.WindowedStep();
        const native::Frame *gpuFrame = port.machine->GpuFrame();
        Zoomed zoomed;
        bool shownNative = false;
        if (gpuFrame && presenter.CanShowNative()) {
            const bool isZoomed = port.WindowedOverlay(overlay, zoomed);
            const NativeGpu::Overlay over = {overlay.data(), port.ShownWidth(), Video::kHeight, isZoomed, ++overlayVersion};
            shownNative = presenter.ShowNative(*gpuFrame, port.machine->GpuVideo(), port.machine->GpuText(), where, overlay.empty() ? nullptr : &over,
                                               isZoomed ? &zoomed : nullptr);
        }
        if (!shownNative) {
            const bool isZoomed = port.WindowedPicture(shown, zoomed);
            presenter.Show(shown.data(), port.ShownWidth(), Video::kHeight, where, isZoomed ? &zoomed : nullptr);
        }
        port.LagShown(ran, SDL_GetTicksNS());
        // (the checks, CPS3RECOMP_WINDOW_SHOT=PREFIX:EVERY: every so many frames what the window was shown, read back, to
        // PREFIX<frame>.png: games/sfiii3/checkwide.sh)
        static const char *windowShot = getenv("CPS3RECOMP_WINDOW_SHOT");
        if (windowShot && ran) {
            const std::string spec = windowShot;
            const size_t colon = spec.rfind(':');
            const uint64_t every = colon == std::string::npos ? 60 : std::max<uint64_t>(1, strtoull(spec.c_str() + colon + 1, nullptr, 10));
            presenter.keepShown = true;
            std::vector<uint32_t> pixels;
            int w = 0, h = 0;
            if (port.frame % every == 0 && presenter.ReadShown(pixels, w, h)) {
                char path[600];
                snprintf(path, sizeof(path), "%s%06llu.png", spec.substr(0, colon).c_str(), static_cast<unsigned long long>(port.frame));
                WritePng(path, pixels.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            }
        }

        if (!port.opt.unthrottled && !FastForward()) {
            nextFrame += static_cast<uint64_t>(kFrameNs);
            const uint64_t now = SDL_GetTicksNS();
            if (nextFrame > now) {
                SDL_DelayPrecise(nextFrame - now);
            } else if (now - nextFrame > 100000000ull) {
                nextFrame = now; // (fell far behind: start counting again)
            }
        }
    }
    // (for the checks: what a shaded window showed last, CPS3RECOMP_SHADER_SHOT=FILE.png)
    if (const char *shot = getenv("CPS3RECOMP_SHADER_SHOT"); shot && presenter.Shaded()) {
        std::vector<uint32_t> pixels;
        int width = 0, height = 0;
        if (presenter.Shader()->ReadBack(pixels, width, height)) {
            WritePng(shot, pixels.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        }
    }
    for (SDL_Gamepad *gp : gamepads) {
        SDL_CloseGamepad(gp);
    }
    if (sink.file) {
        fclose(sink.file);
        fprintf(stderr, "cps3recomp: audio sink: %llu frames at %d Hz, %llu quanta short\n", static_cast<unsigned long long>(sink.taken), sink.rate,
                static_cast<unsigned long long>(sink.short_));
    }
    if (audio) {
        SDL_DestroyAudioStream(audio);
    }
    if (music) {
        SDL_DestroyAudioStream(music);
    }
    // (on to a Multi game run's next game: its window where this one is; on macOS the launcher starts it, and must
    // be in front for it to come forward)
    if (ProgramEnded() == kNextGame && !MultiGame().empty()) {
        KeepPlacement(window);
#ifdef __APPLE__
        HandFrontToLauncher();
#endif
    }
    presenter.Close();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace port
