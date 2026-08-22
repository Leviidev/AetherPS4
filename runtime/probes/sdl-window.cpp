#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <initializer_list>
#include <thread>

int main() {
    for (const char* library : {
             "libX11.so.6",
             "libX11-xcb.so.1",
             "libXcursor.so.1",
             "libXext.so.6",
             "libXfixes.so.3",
             "libXi.so.6",
             "libXrandr.so.2",
             "libXss.so.1",
             "libxkbcommon.so.0",
         }) {
        void* handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
        std::fprintf(stderr, "BACHATA_DLOPEN %s %s\n", library, handle != nullptr ? "ok" : dlerror());
        if (handle != nullptr) dlclose(handle);
    }

    const int drivers = SDL_GetNumVideoDrivers();
    std::fprintf(stderr, "BACHATA_SDL_DRIVERS count=%d", drivers);
    for (int index = 0; index < drivers; ++index) {
        std::fprintf(stderr, " %s", SDL_GetVideoDriver(index));
    }
    std::fputc('\n', stderr);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "BACHATA_SDL_ERROR init=%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(
        "Bachata S4 SDL probe",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        std::fprintf(stderr, "BACHATA_SDL_ERROR create_window=%s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool quit_requested = false;
    while (!quit_requested && std::chrono::steady_clock::now() < deadline) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) quit_requested = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "resized to %dx%d", event.window.data1, event.window.data2);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    if (quit_requested) return 3;
    std::puts("BACHATA_SDL_OK");
    return 0;
}
