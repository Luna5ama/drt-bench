#pragma once

#include "rgb_cube.hpp"

#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

class GpuCubeWindow {
public:
    using RedrawCallback = std::function<void()>;

    GpuCubeWindow() = default;
    ~GpuCubeWindow();
    GpuCubeWindow(const GpuCubeWindow&) = delete;
    GpuCubeWindow& operator=(const GpuCubeWindow&) = delete;

    void create(HWND owner, CubeSpace space, std::size_t placementIndex,
                RedrawCallback redraw);
    bool visible() const;
    HWND handle() const { return window_; }
    CubeSpace space() const { return space_; }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    bool consumeResize();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam);
    void requestRedraw();

    HWND window_ = nullptr;
    CubeSpace space_ = CubeSpace::rgb;
    RedrawCallback redraw_;
    bool dragging_ = false;
    bool resizePending_ = true;
    POINTS dragPoint_{};
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
};

class GpuCubeRenderer {
public:
    GpuCubeRenderer();
    ~GpuCubeRenderer();
    GpuCubeRenderer(const GpuCubeRenderer&) = delete;
    GpuCubeRenderer& operator=(const GpuCubeRenderer&) = delete;

    void initialize(VkInstance instance, VkPhysicalDevice physicalDevice,
                    VkDevice device, VkQueue queue, uint32_t queueFamily,
                    VkSampler sampler, const std::filesystem::path& shaderDirectory);
    void shutdown();
    void add(GpuCubeWindow& window);
    void removeClosed();
    void prepareFrame(bool hdr);
    void acquire();
    std::span<const VkSemaphore> waitSemaphores() const;
    void record(VkCommandBuffer commandBuffer, VkImageView sourceView,
                VkExtent2D sourceExtent, bool hdr);
    void appendPresent(std::vector<VkSwapchainKHR>& swapchains,
                       std::vector<uint32_t>& imageIndices) const;
    bool handlePresent(std::span<const VkResult> results);
    bool hasAcquiredImages() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
