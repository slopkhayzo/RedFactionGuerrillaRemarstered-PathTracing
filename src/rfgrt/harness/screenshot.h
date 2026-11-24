#pragma once
#include <cstdint>
#include <string>

struct IDXGISwapChain;

// Screenshots of the presented frame (PNG, at most 1280 pixels wide), taken in keen::graphics::endFrame on the render
// thread just before Present. Ported from the PhysX project.
namespace screenshot {

bool install();
// The next presented frame is saved to `png_path`.
void request(const std::wstring& png_path);
// Called in every endFrame before the capture (render thread): draws over the frame about to be presented.
void set_overlay(void (*draw)(uint8_t* graphics, IDXGISwapChain* swap_chain));
// Called in every endFrame first (render thread) with the game's swap chain: the presenter's (present/presenter.h).
void set_end_frame(void (*fn)(IDXGISwapChain* swap_chain));

}  // namespace screenshot
