/**
 * High Performance JPG Previewer
 * * Dependencies:
 * 1. SDL2 (Simple DirectMedia Layer) - For windowing and hardware accelerated rendering.
 * 2. stb_image.h - For image decoding (Single header library).
 * * Setup:
 * Ensure 'stb_image.h' is in the same directory or include path.
 * Download it here: https://github.com/nothings/stb/blob/master/stb_image.h
 */

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <future>
#include <algorithm>
#include <chrono>

// SDL2
#include <SDL2/SDL.h>

// STB Image Implementation
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------
// Structures
// ---------------------------------------------------------

struct RawImage {
    std::string filename;
    int width;
    int height;
    int channels;
    unsigned char* data; // Raw pixel data in RAM
    bool loaded;
};

// ---------------------------------------------------------
// Global State
// ---------------------------------------------------------

std::vector<RawImage> g_images;
size_t g_currentIndex = 0;
SDL_Texture* g_displayTexture = nullptr;
int g_texWidth = 0;
int g_texHeight = 0;

// ---------------------------------------------------------
// Helper Functions
// ---------------------------------------------------------

bool isImageFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" || ext == ".tga");
}

// Loads a single image into raw memory (CPU side)
// This is designed to be thread-safe for parallel loading
void loadImageIntoMemory(RawImage& img, const std::string& filepath) {
    // Force 4 channels (RGBA) for consistency with SDL textures
    img.data = stbi_load(filepath.c_str(), &img.width, &img.height, &img.channels, 4);
    if (img.data) {
        img.loaded = true;
        // printf("Loaded: %s (%dx%d)\n", fs::path(filepath).filename().string().c_str(), img.width, img.height);
    } else {
        img.loaded = false;
        fprintf(stderr, "Failed to load: %s\n", filepath.c_str());
    }
}

// Updates the GPU texture with specific image data
void updateTexture(SDL_Renderer* renderer) {
    if (g_images.empty()) return;
    
    RawImage& current = g_images[g_currentIndex];
    if (!current.loaded || !current.data) return;

    // If texture exists but dimensions are different, destroy it
    if (g_displayTexture && (g_texWidth != current.width || g_texHeight != current.height)) {
        SDL_DestroyTexture(g_displayTexture);
        g_displayTexture = nullptr;
    }

    // Create texture if needed
    if (!g_displayTexture) {
        g_displayTexture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING, // Streaming allows fast CPU->GPU updates
            current.width,
            current.height
        );
        g_texWidth = current.width;
        g_texHeight = current.height;
    }

    // Upload pixels to GPU
    // This is generally very fast (PCIe bandwidth) even for 4k images
    SDL_UpdateTexture(g_displayTexture, nullptr, current.data, current.width * 4);
    
    // Set linear filtering for smooth scaling
    SDL_SetTextureScaleMode(g_displayTexture, SDL_ScaleModeLinear);
    
    std::cout << "[" << (g_currentIndex + 1) << "/" << g_images.size() << "] Viewing: " << current.filename << std::endl;
}

// ---------------------------------------------------------
// Main Application
// ---------------------------------------------------------

int main(int argc, char* argv[]) {
    // 1. Argument Parsing
    std::string inputDir = ".";
    if (argc > 1) {
        inputDir = argv[1];
    }

    if (!fs::exists(inputDir) || !fs::is_directory(inputDir)) {
        std::cerr << "Error: Directory not found -> " << inputDir << std::endl;
        return 1;
    }

    // 2. Scan Directory
    std::vector<std::string> filePaths;
    std::cout << "Scanning directory: " << inputDir << " ..." << std::endl;
    for (const auto& entry : fs::directory_iterator(inputDir)) {
        if (entry.is_regular_file() && isImageFile(entry.path())) {
            filePaths.push_back(entry.path().string());
        }
    }
    
    // Sort files alphabetically
    std::sort(filePaths.begin(), filePaths.end());

    if (filePaths.empty()) {
        std::cerr << "No images found in directory." << std::endl;
        return 1;
    }

    // 3. Pre-allocation
    size_t count = filePaths.size();
    g_images.resize(count);
    
    // 4. Parallel Loading Phase
    // We launch std::async tasks to decode JPGs on all available cores.
    std::cout << "Loading " << count << " images into System RAM... (This consumes memory to ensure instant switching)" << std::endl;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    std::vector<std::future<void>> futures;

    for (size_t i = 0; i < count; ++i) {
        g_images[i].filename = fs::path(filePaths[i]).filename().string();
        // Launch async loader
        futures.push_back(std::async(std::launch::async, loadImageIntoMemory, std::ref(g_images[i]), filePaths[i]));
    }

    // Wait for all to finish
    int loadedCount = 0;
    for (auto& f : futures) {
        f.get();
        loadedCount++;
        if (loadedCount % 10 == 0) std::cout << "\rProcessed " << loadedCount << "/" << count << "..." << std::flush;
    }
    std::cout << std::endl;

    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = endTime - startTime;
    std::cout << "Loaded " << count << " images in " << elapsed.count() << " seconds." << std::endl;

    // 5. Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL could not initialize! Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    // Create Window
    SDL_Window* window = SDL_CreateWindow(
        "High-Res Image Previewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window) return 1;

    // Create Renderer (Hardware Accelerated)
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) return 1;

    // Initial Texture Upload
    updateTexture(renderer);

    // 6. Main Loop
    bool quit = false;
    SDL_Event e;

    while (!quit) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                quit = true;
            } else if (e.type == SDL_KEYDOWN) {
                bool changed = false;
                switch (e.key.keysym.sym) {
                    case SDLK_RIGHT:
                    case SDLK_d:
                    case SDLK_SPACE:
                        g_currentIndex = (g_currentIndex + 1) % g_images.size();
                        changed = true;
                        break;
                    case SDLK_LEFT:
                    case SDLK_a:
                        g_currentIndex = (g_currentIndex == 0) ? g_images.size() - 1 : g_currentIndex - 1;
                        changed = true;
                        break;
                    case SDLK_ESCAPE:
                        quit = true;
                        break;
                }

                if (changed) {
                    updateTexture(renderer);
                }
            } else if (e.type == SDL_WINDOWEVENT) {
                 if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
                     SDL_RenderPresent(renderer); // Force redraw on resize
                 }
            }
        }

        // ------------------
        // Rendering
        // ------------------
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255); // Dark Grey Background
        SDL_RenderClear(renderer);

        if (g_displayTexture) {
            // Get Window Size
            int winW, winH;
            SDL_GetRendererOutputSize(renderer, &winW, &winH);

            // Calculate Aspect Ratio Fit
            float imgAspect = (float)g_texWidth / (float)g_texHeight;
            float winAspect = (float)winW / (float)winH;

            SDL_Rect dstRect;
            
            if (winAspect > imgAspect) {
                // Window is wider than image (fit to height)
                dstRect.h = winH;
                dstRect.w = (int)(winH * imgAspect);
                dstRect.y = 0;
                dstRect.x = (winW - dstRect.w) / 2;
            } else {
                // Window is taller than image (fit to width)
                dstRect.w = winW;
                dstRect.h = (int)(winW / imgAspect);
                dstRect.x = 0;
                dstRect.y = (winH - dstRect.h) / 2;
            }

            SDL_RenderCopy(renderer, g_displayTexture, nullptr, &dstRect);
        }

        SDL_RenderPresent(renderer);
    }

    // 7. Cleanup
    // Free raw image data
    for (auto& img : g_images) {
        if (img.data) stbi_image_free(img.data);
    }

    if (g_displayTexture) SDL_DestroyTexture(g_displayTexture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
