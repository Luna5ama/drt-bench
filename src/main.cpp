#include <windows.h>
#include <vulkan/vulkan.h>
#include <tinyexr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr UINT kCommandMessage = WM_APP + 1;
using Clock = std::chrono::steady_clock;

bool shaderReloadReady(bool pending, Clock::time_point lastAttempt,
                       Clock::time_point lastChange, Clock::time_point now) {
    return pending && now - lastAttempt >= 1s && now - lastChange >= 1s;
}

void vkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
}

std::vector<std::byte> readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    const auto end = file.tellg();
    if (end < 0) throw std::runtime_error("cannot size " + path.string());
    std::vector<std::byte> bytes(static_cast<size_t>(end));
    file.seekg(0);
    if (!bytes.empty() &&
        !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return bytes;
}

std::string expandShaderIncludes(const fs::path& path, const fs::path& root,
                                 std::vector<fs::path> stack = {}) {
    const fs::path canonical = fs::weakly_canonical(path);
    if (std::find(stack.begin(), stack.end(), canonical) != stack.end()) {
        throw std::runtime_error("cyclic shader include: " + canonical.string());
    }
    stack.push_back(canonical);

    std::ifstream file(canonical, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open shader " + canonical.string());

    std::string expanded;
    std::string line;
    while (std::getline(file, line)) {
        const size_t start = line.find_first_not_of(" \t");
        const std::string_view directive =
            start == std::string::npos ? std::string_view{} : std::string_view(line).substr(start);
        if (!directive.starts_with("#include")) {
            expanded += line + '\n';
            continue;
        }

        const size_t open = line.find_first_of("\"<", start + 8);
        const char closeChar = open != std::string::npos && line[open] == '<' ? '>' : '"';
        const size_t close = open == std::string::npos ? std::string::npos : line.find(closeChar, open + 1);
        if (close == std::string::npos) {
            throw std::runtime_error("invalid shader include in " + canonical.string());
        }

        const fs::path includeName = line.substr(open + 1, close - open - 1);
        const fs::path includePath = includeName.has_root_directory()
            ? root / includeName.relative_path()
            : canonical.parent_path() / includeName;
        expanded += expandShaderIncludes(includePath, root, stack);
    }
    return expanded;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

fs::path pathFromUtf8(const std::string& text) {
    return fs::path(reinterpret_cast<const char8_t*>(text.data()),
                    reinterpret_cast<const char8_t*>(text.data() + text.size()));
}

std::wstring quote(const fs::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

struct CommandState {
    std::mutex mutex;
    std::deque<std::string> lines;
    std::atomic<HWND> window = nullptr;
    std::atomic_bool running = true;
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
};

class App {
public:
    explicit App(const fs::path& executable) : executableDir_(executable.parent_path()) {
        createWindow();
        initVulkan();
        startStdin();
        std::cout << "commands: /loadexr /loadfp16 /loadfp32 /loaddrt /sdr /hdr\n";
    }

    ~App() {
        commandState_->running = false;
        commandState_->window = nullptr;
        if (device_) vkDeviceWaitIdle(device_);
        destroyTexture(input_);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        if (descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
        destroySwapchain();
        if (renderFence_) vkDestroyFence(device_, renderFence_, nullptr);
        if (renderFinished_) vkDestroySemaphore(device_, renderFinished_, nullptr);
        if (imageAvailable_) vkDestroySemaphore(device_, imageAvailable_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (device_) vkDestroyDevice(device_, nullptr);
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        if (window_) DestroyWindow(window_);
        std::error_code ignored;
        fs::remove(tempSource_, ignored);
        fs::remove(tempSpirv_, ignored);
        fs::remove(tempLog_, ignored);
    }

    int run() {
        while (running_) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) running_ = false;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running_) break;

            processCommands();
            pollShader();
            if (resizePending_ && !minimized_) {
                resizePending_ = false;
                recreateSwapchain();
                dirty_ = true;
            }
            if (dirty_ && !minimized_ && pipeline_) draw();

            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        return 0;
    }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            app->dirty_ = true;
            return 0;
        }
        case WM_SIZE:
            app->minimized_ = wparam == SIZE_MINIMIZED;
            app->resizePending_ = !app->minimized_;
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) DestroyWindow(window);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            app->window_ = nullptr;
            app->running_ = false;
            PostQuitMessage(0);
            return 0;
        case kCommandMessage:
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    void createWindow() {
        const HINSTANCE module = GetModuleHandleW(nullptr);
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = module;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"drt-bench-window";
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("RegisterClassW failed");
        }

        constexpr int width = 1280;
        constexpr int height = 720;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int x = work.left + (work.right - work.left - width) / 2;
        const int y = work.top + (work.bottom - work.top - height) / 2;
        window_ = CreateWindowExW(0, windowClass.lpszClassName, L"drt-bench [SDR]", WS_POPUP,
                                  x, y, width, height, nullptr, nullptr, module, nullptr);
        if (!window_) throw std::runtime_error("CreateWindowExW failed");
        SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ShowWindow(window_, SW_SHOW);
    }

    void initVulkan() {
        VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.pApplicationName = "drt-bench";
        appInfo.apiVersion = VK_API_VERSION_1_2;
        const char* instanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
            VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
        };
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &appInfo;
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(std::size(instanceExtensions));
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;
        vkCheck(vkCreateInstance(&instanceInfo, nullptr, &instance_), "vkCreateInstance");

        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = GetModuleHandleW(nullptr);
        surfaceInfo.hwnd = window_;
        vkCheck(vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_), "vkCreateWin32SurfaceKHR");

        uint32_t deviceCount = 0;
        vkCheck(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkCheck(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");
        for (VkPhysicalDevice candidate : devices) {
            uint32_t queueCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queueCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            if (!features.shaderStorageImageWriteWithoutFormat) continue;
            for (uint32_t i = 0; i < queueCount; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present);
                if ((queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) {
                    physicalDevice_ = candidate;
                    queueFamily_ = i;
                    break;
                }
            }
            if (physicalDevice_) break;
        }
        if (!physicalDevice_) {
            throw std::runtime_error("no Vulkan device supports compute presentation and formatless storage writes");
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabledFeatures{};
        enabledFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;
        deviceInfo.pEnabledFeatures = &enabledFeatures;
        vkCheck(vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        vkCheck(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = commandPool_;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(device_, &commandInfo, &commandBuffer_), "vkAllocateCommandBuffers");

        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_), "vkCreateSemaphore");
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished_), "vkCreateSemaphore");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCheck(vkCreateFence(device_, &fenceInfo, nullptr, &renderFence_), "vkCreateFence");

        createDescriptors();
        createSampler();
        recreateSwapchain();
        const float gray[] = {0.18f, 0.18f, 0.18f, 1.0f};
        uploadTexture(gray, sizeof(gray), 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT);

        const fs::path defaultShader = executableDir_ / "passthrough.glsl";
        if (!loadShader(defaultShader, true)) {
            throw std::runtime_error("failed to compile bundled passthrough.glsl");
        }
    }

    void createDescriptors() {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        vkCheck(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorLayout_),
                "vkCreateDescriptorSetLayout");

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorLayout_;
        vkCheck(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_),
                "vkCreatePipelineLayout");

        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        vkCheck(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = descriptorPool_;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &descriptorLayout_;
        vkCheck(vkAllocateDescriptorSets(device_, &setInfo, &descriptorSet_), "vkAllocateDescriptorSets");
    }

    void createSampler() {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 0.0f;
        vkCheck(vkCreateSampler(device_, &info, nullptr, &sampler_), "vkCreateSampler");
    }

    static int colorSpaceRank(VkColorSpaceKHR space) {
        switch (space) {
        case VK_COLOR_SPACE_HDR10_ST2084_EXT: return 100;
        case VK_COLOR_SPACE_DOLBYVISION_EXT: return 95;
        case VK_COLOR_SPACE_BT2020_LINEAR_EXT: return 90;
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return 80;
        case VK_COLOR_SPACE_HDR10_HLG_EXT: return 70;
        case VK_COLOR_SPACE_DISPLAY_NATIVE_AMD: return 60;
        default: return 0;
        }
    }

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
        auto storageCapable = [this](const VkSurfaceFormatKHR& format) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice_, format.format, &properties);
            return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        };
        if (hdr_) {
            for (const auto& format : formats) {
                if (format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
                    format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && storageCapable(format)) {
                    return format;
                }
            }
            std::optional<VkSurfaceFormatKHR> best;
            for (const auto& format : formats) {
                if (!storageCapable(format)) continue;
                if (!best || colorSpaceRank(format.colorSpace) > colorSpaceRank(best->colorSpace)) best = format;
            }
            if (best && colorSpaceRank(best->colorSpace) > 0) return *best;
            throw std::runtime_error("active display reports no storage-capable HDR surface format");
        }
        constexpr VkFormat preferred[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
        for (VkFormat wanted : preferred) {
            for (const auto& format : formats) {
                if (format.format == wanted &&
                    format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && storageCapable(format)) {
                    return format;
                }
            }
        }
        for (const auto& format : formats) {
            if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && storageCapable(format)) return format;
        }
        throw std::runtime_error("active display reports no storage-capable SDR surface format");
    }

    void recreateSwapchain() {
        vkDeviceWaitIdle(device_);
        VkSurfaceCapabilitiesKHR capabilities{};
        vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities),
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT)) {
            throw std::runtime_error("surface swapchain images do not support storage usage");
        }
        uint32_t formatCount = 0;
        vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr),
                "vkGetPhysicalDeviceSurfaceFormatsKHR");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data()),
                "vkGetPhysicalDeviceSurfaceFormatsKHR");
        const VkSurfaceFormatKHR selected = chooseSurfaceFormat(formats);

        RECT client{};
        GetClientRect(window_, &client);
        VkExtent2D extent{};
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            extent = capabilities.currentExtent;
        } else {
            extent.width = std::clamp<uint32_t>(client.right - client.left, capabilities.minImageExtent.width,
                                                capabilities.maxImageExtent.width);
            extent.height = std::clamp<uint32_t>(client.bottom - client.top, capabilities.minImageExtent.height,
                                                 capabilities.maxImageExtent.height);
        }
        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount && imageCount > capabilities.maxImageCount) {
            imageCount = capabilities.maxImageCount;
        }
        const VkCompositeAlphaFlagBitsKHR alphaCandidates[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (const auto candidate : alphaCandidates) {
            if (capabilities.supportedCompositeAlpha & candidate) {
                alpha = candidate;
                break;
            }
        }

        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = imageCount;
        info.imageFormat = selected.format;
        info.imageColorSpace = selected.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain_;
        VkSwapchainKHR replacement = VK_NULL_HANDLE;
        vkCheck(vkCreateSwapchainKHR(device_, &info, nullptr, &replacement), "vkCreateSwapchainKHR");

        for (VkImageView view : swapchainViews_) vkDestroyImageView(device_, view, nullptr);
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = replacement;
        swapchainFormat_ = selected.format;
        colorSpace_ = selected.colorSpace;
        extent_ = extent;
        vkCheck(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr), "vkGetSwapchainImagesKHR");
        swapchainImages_.resize(imageCount);
        vkCheck(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data()),
                "vkGetSwapchainImagesKHR");
        swapchainViews_.resize(imageCount);
        imageInitialized_.assign(imageCount, false);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = swapchainImages_[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = swapchainFormat_;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            vkCheck(vkCreateImageView(device_, &viewInfo, nullptr, &swapchainViews_[i]), "vkCreateImageView");
        }
        SetWindowTextW(window_, hdr_ ? L"drt-bench [HDR]" : L"drt-bench [SDR]");
        std::cout << (hdr_ ? "HDR" : "SDR") << " swapchain: format=" << swapchainFormat_
                  << " colorspace=" << colorSpace_ << " extent=" << extent_.width << 'x' << extent_.height << '\n';
    }

    void destroySwapchain() {
        if (!device_) return;
        for (VkImageView view : swapchainViews_) vkDestroyImageView(device_, view, nullptr);
        swapchainViews_.clear();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags required) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & required) == required) return i;
        }
        throw std::runtime_error("no compatible Vulkan memory type");
    }

    void uploadTexture(const void* pixels, size_t byteCount, uint32_t width, uint32_t height, VkFormat format) {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &formatProperties);
        const auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((formatProperties.optimalTilingFeatures & required) != required) {
            throw std::runtime_error("input texture format lacks linear sampling support");
        }
        vkDeviceWaitIdle(device_);

        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = byteCount;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device_, &bufferInfo, nullptr, &staging), "vkCreateBuffer");
        VkMemoryRequirements bufferRequirements{};
        vkGetBufferMemoryRequirements(device_, staging, &bufferRequirements);
        VkMemoryAllocateInfo bufferAllocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bufferAllocation.allocationSize = bufferRequirements.size;
        bufferAllocation.memoryTypeIndex =
            memoryType(bufferRequirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(device_, &bufferAllocation, nullptr, &stagingMemory), "vkAllocateMemory");
        vkCheck(vkBindBufferMemory(device_, staging, stagingMemory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        vkCheck(vkMapMemory(device_, stagingMemory, 0, byteCount, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, pixels, byteCount);
        vkUnmapMemory(device_, stagingMemory);

        Texture replacement{};
        replacement.width = width;
        replacement.height = height;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(device_, &imageInfo, nullptr, &replacement.image), "vkCreateImage");
        VkMemoryRequirements imageRequirements{};
        vkGetImageMemoryRequirements(device_, replacement.image, &imageRequirements);
        VkMemoryAllocateInfo imageAllocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        imageAllocation.allocationSize = imageRequirements.size;
        imageAllocation.memoryTypeIndex = memoryType(imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(device_, &imageAllocation, nullptr, &replacement.memory), "vkAllocateMemory");
        vkCheck(vkBindImageMemory(device_, replacement.image, replacement.memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = replacement.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        vkCheck(vkCreateImageView(device_, &viewInfo, nullptr, &replacement.view), "vkCreateImageView");

        vkCheck(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = replacement.image;
        toTransfer.subresourceRange = viewInfo.subresourceRange;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toTransfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(commandBuffer_, staging, replacement.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier toSample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSample.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSample.image = replacement.image;
        toSample.subresourceRange = viewInfo.subresourceRange;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toSample);
        vkCheck(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        vkCheck(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        vkCheck(vkQueueWaitIdle(queue_), "vkQueueWaitIdle");

        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMemory, nullptr);
        destroyTexture(input_);
        input_ = replacement;
        dirty_ = true;
    }

    void destroyTexture(Texture& texture) {
        if (!device_) return;
        if (texture.view) vkDestroyImageView(device_, texture.view, nullptr);
        if (texture.image) vkDestroyImage(device_, texture.image, nullptr);
        if (texture.memory) vkFreeMemory(device_, texture.memory, nullptr);
        texture = {};
    }

    fs::path glslcPath() const {
        wchar_t sdk[32768]{};
        const DWORD sdkLength =
            GetEnvironmentVariableW(L"VULKAN_SDK", sdk, static_cast<DWORD>(std::size(sdk)));
        if (sdkLength && sdkLength < std::size(sdk)) {
            fs::path candidate = fs::path(sdk) / "Bin" / "glslc.exe";
            if (fs::exists(candidate)) return candidate;
        }
        wchar_t found[32768]{};
        if (SearchPathW(nullptr, L"glslc.exe", nullptr,
                        static_cast<DWORD>(std::size(found)), found, nullptr)) {
            return found;
        }
        throw std::runtime_error("glslc.exe not found; install the Vulkan SDK or add glslc to PATH");
    }

    bool compilePipeline(const fs::path& path, bool hdr, VkPipeline& result) {
        try {
            const std::string source = expandShaderIncludes(path, path.parent_path());
            std::ofstream composite(tempSource_, std::ios::binary | std::ios::trunc);
            composite << "#version 460\n";
            composite << (hdr ? "#define DRT_BENCH_HDR 1\n" : "#define DRT_BENCH_SDR 1\n");
            composite << "layout(local_size_x = 8, local_size_y = 8) in;\n"
                         "layout(set = 0, binding = 0) uniform sampler2D usam_inputTex;\n"
                         "layout(set = 0, binding = 1) uniform writeonly image2D uimg_outputTex;\n"
                         "#line 1\n";
            composite << source;
            composite.close();

            const fs::path compiler = glslcPath();
            std::wstring command = quote(compiler) +
                L" --target-env=vulkan1.2 -fshader-stage=compute -I" +
                quote(path.parent_path()) + L" -o " + quote(tempSpirv_) + L" " + quote(tempSource_);
            SECURITY_ATTRIBUTES securityAttributes{sizeof(securityAttributes), nullptr, TRUE};
            HANDLE log = CreateFileW(tempLog_.c_str(), GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, &securityAttributes,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("cannot create shader compiler log");
            }
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = log;
            startup.hStdError = log;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION process{};
            std::vector<wchar_t> mutableCommand(command.begin(), command.end());
            mutableCommand.push_back(L'\0');
            const BOOL created =
                CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, path.parent_path().c_str(),
                               &startup, &process);
            CloseHandle(log);
            if (!created) throw std::runtime_error("CreateProcessW(glslc) failed");
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD exitCode = 1;
            GetExitCodeProcess(process.hProcess, &exitCode);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            if (exitCode != 0) {
                const auto bytes = readFile(tempLog_);
                std::cerr << "shader compile failed:\n"
                          << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()) << '\n';
                return false;
            }

            const auto spirvBytes = readFile(tempSpirv_);
            if (spirvBytes.empty() || spirvBytes.size() % sizeof(uint32_t)) {
                throw std::runtime_error("glslc produced invalid SPIR-V");
            }
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = spirvBytes.size();
            moduleInfo.pCode = reinterpret_cast<const uint32_t*>(spirvBytes.data());
            VkShaderModule module = VK_NULL_HANDLE;
            vkCheck(vkCreateShaderModule(device_, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = module;
            stage.pName = "main";
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = stage;
            pipelineInfo.layout = pipelineLayout_;
            const VkResult pipelineResult =
                vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &result);
            vkDestroyShaderModule(device_, module, nullptr);
            vkCheck(pipelineResult, "vkCreateComputePipelines");
            return true;
        } catch (const std::exception& error) {
            std::cerr << "shader reload failed: " << error.what() << '\n';
            return false;
        }
    }

    bool loadShader(const fs::path& path, bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && pipeline_ && now - lastReloadAttempt_ < 1s) {
            shaderPath_ = fs::absolute(path);
            shaderReloadPending_ = true;
            lastShaderChange_ = now;
            std::cout << "shader reload queued (1 second cooldown)\n";
            return true;
        }
        VkPipeline replacement = VK_NULL_HANDLE;
        lastReloadAttempt_ = now;
        shaderPath_ = fs::absolute(path);
        std::error_code ignored;
        observedShaderWrite_ = fs::last_write_time(shaderPath_, ignored);
        if (!compilePipeline(shaderPath_, hdr_, replacement)) return false;
        vkDeviceWaitIdle(device_);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = replacement;
        shaderReloadPending_ = false;
        dirty_ = true;
        std::cout << "shader loaded: " << shaderPath_.string() << '\n';
        return true;
    }

    void pollShader() {
        if (shaderPath_.empty()) return;
        std::error_code error;
        const auto write = fs::last_write_time(shaderPath_, error);
        if (!error && write != observedShaderWrite_) {
            observedShaderWrite_ = write;
            shaderReloadPending_ = true;
            lastShaderChange_ = std::chrono::steady_clock::now();
        }
        const auto now = std::chrono::steady_clock::now();
        if (shaderReloadReady(shaderReloadPending_, lastReloadAttempt_, lastShaderChange_, now)) {
            loadShader(shaderPath_, true);
        }
    }

    void draw() {
        vkCheck(vkWaitForFences(device_, 1, &renderFence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        uint32_t imageIndex = 0;
        const VkResult acquire =
            vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_,
                                  VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            vkCheck(acquire, "vkAcquireNextImageKHR");
        }
        vkCheck(vkResetFences(device_, 1, &renderFence_), "vkResetFences");
        vkCheck(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer");

        VkDescriptorImageInfo inputInfo{};
        inputInfo.sampler = sampler_;
        inputInfo.imageView = input_.view;
        inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo outputInfo{};
        outputInfo.imageView = swapchainViews_[imageIndex];
        outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet_;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &inputInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSet_;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &outputInfo;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(commandBuffer_, &begin), "vkBeginCommandBuffer");
        VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout =
            imageInitialized_[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = swapchainImages_[imageIndex];
        toGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toGeneral.subresourceRange.levelCount = 1;
        toGeneral.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toGeneral);
        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
        vkCmdDispatch(commandBuffer_, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
        VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = swapchainImages_[imageIndex];
        toPresent.subresourceRange = toGeneral.subresourceRange;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toPresent);
        vkCheck(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer");

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailable_;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished_;
        vkCheck(vkQueueSubmit(queue_, 1, &submit, renderFence_), "vkQueueSubmit");
        imageInitialized_[imageIndex] = true;

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinished_;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &imageIndex;
        const VkResult presented = vkQueuePresentKHR(queue_, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            recreateSwapchain();
            dirty_ = true;
        } else {
            vkCheck(presented, "vkQueuePresentKHR");
            dirty_ = false;
        }
    }

    void startStdin() {
        commandState_->window = window_;
        const auto state = commandState_;
        std::thread([state] {
            std::string line;
            while (state->running && std::getline(std::cin, line)) {
                {
                    std::lock_guard lock(state->mutex);
                    state->lines.push_back(std::move(line));
                }
                if (const HWND window = state->window.load()) {
                    PostMessageW(window, kCommandMessage, 0, 0);
                }
            }
        }).detach();
    }

    void processCommands() {
        std::deque<std::string> lines;
        {
            std::lock_guard lock(commandState_->mutex);
            lines.swap(commandState_->lines);
        }
        for (const auto& line : lines) {
            try {
                const auto split = line.find_first_of(" \t");
                const std::string command = line.substr(0, split);
                const std::string argument =
                    split == std::string::npos ? "" : trim(line.substr(split + 1));
                if (command == "/loadexr") {
                    loadExr(argument);
                } else if (command == "/loadfp16") {
                    loadRaw(argument, 2, VK_FORMAT_R16G16B16A16_SFLOAT);
                } else if (command == "/loadfp32") {
                    loadRaw(argument, 4, VK_FORMAT_R32G32B32A32_SFLOAT);
                } else if (command == "/loaddrt") {
                    if (argument.empty()) throw std::runtime_error("usage: /loaddrt <path>");
                    loadShader(pathFromUtf8(argument), false);
                } else if (command == "/sdr") {
                    setHdr(false);
                } else if (command == "/hdr") {
                    setHdr(true);
                } else if (!trim(line).empty()) {
                    std::cerr << "unknown command: " << command << '\n';
                }
            } catch (const std::exception& error) {
                std::cerr << "command failed: " << error.what() << '\n';
            }
        }
    }

    void loadExr(const std::string& argument) {
        if (argument.empty()) throw std::runtime_error("usage: /loadexr <path>");
        const fs::path path = pathFromUtf8(argument);
        const auto bytes = readFile(path);
        float* pixels = nullptr;
        int width = 0;
        int height = 0;
        const char* error = nullptr;
        const int result =
            LoadEXRFromMemory(&pixels, &width, &height,
                              reinterpret_cast<const unsigned char*>(bytes.data()),
                              bytes.size(), &error);
        if (result != TINYEXR_SUCCESS) {
            const std::string message = error ? error : "unknown TinyEXR error";
            if (error) FreeEXRErrorMessage(error);
            throw std::runtime_error(message);
        }
        try {
            uploadTexture(pixels, static_cast<size_t>(width) * height * 4 * sizeof(float),
                          static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                          VK_FORMAT_R32G32B32A32_SFLOAT);
        } catch (...) {
            free(pixels);
            throw;
        }
        free(pixels);
        std::cout << "EXR loaded: " << path.string() << " (" << width << 'x' << height << ")\n";
    }

    void loadRaw(const std::string& argument, size_t componentBytes, VkFormat format) {
        if (argument.empty()) {
            throw std::runtime_error("usage: /loadfp16 <path> or /loadfp32 <path>");
        }
        const fs::path path = pathFromUtf8(argument);
        const auto bytes = readFile(path);
        const size_t expected =
            static_cast<size_t>(extent_.width) * extent_.height * 4 * componentBytes;
        if (bytes.size() != expected) {
            throw std::runtime_error(
                "raw RGBA byte size is " + std::to_string(bytes.size()) +
                ", expected " + std::to_string(expected) + " for " +
                std::to_string(extent_.width) + "x" + std::to_string(extent_.height));
        }
        uploadTexture(bytes.data(), bytes.size(), extent_.width, extent_.height, format);
        std::cout << "raw input loaded: " << path.string() << '\n';
    }

    void setHdr(bool enabled) {
        if (hdr_ == enabled) return;
        VkPipeline replacement = VK_NULL_HANDLE;
        if (!compilePipeline(shaderPath_, enabled, replacement)) {
            std::cerr << "display mode unchanged because shader recompilation failed\n";
            return;
        }
        vkDeviceWaitIdle(device_);
        hdr_ = enabled;
        try {
            recreateSwapchain();
        } catch (...) {
            hdr_ = !enabled;
            vkDestroyPipeline(device_, replacement, nullptr);
            recreateSwapchain();
            throw;
        }
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = replacement;
        dirty_ = true;
    }

    HWND window_ = nullptr;
    bool running_ = true;
    bool minimized_ = false;
    bool resizePending_ = false;
    bool dirty_ = true;
    bool hdr_ = false;
    std::shared_ptr<CommandState> commandState_ = std::make_shared<CommandState>();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkSemaphore imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore renderFinished_ = VK_NULL_HANDLE;
    VkFence renderFence_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    Texture input_{};

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<bool> imageInitialized_;

    fs::path executableDir_;
    fs::path shaderPath_;
    fs::file_time_type observedShaderWrite_{};
    std::chrono::steady_clock::time_point lastReloadAttempt_{};
    std::chrono::steady_clock::time_point lastShaderChange_{};
    bool shaderReloadPending_ = false;
    fs::path tempSource_ =
        fs::temp_directory_path() / ("drt-bench-" + std::to_string(GetCurrentProcessId()) + ".comp");
    fs::path tempSpirv_ =
        fs::temp_directory_path() / ("drt-bench-" + std::to_string(GetCurrentProcessId()) + ".spv");
    fs::path tempLog_ =
        fs::temp_directory_path() / ("drt-bench-" + std::to_string(GetCurrentProcessId()) + ".log");
};

int selfTest() {
    if (trim("  \"a b.exr\" \t") != "a b.exr") return 1;
    constexpr uint32_t width = 1280;
    constexpr uint32_t height = 720;
    const size_t fp16Bytes = static_cast<size_t>(width) * height * 4 * 2;
    const size_t fp32Bytes = static_cast<size_t>(width) * height * 4 * 4;
    if (fp16Bytes != 7'372'800 || fp32Bytes != 14'745'600) return 1;
    const auto now = Clock::now();
    if (shaderReloadReady(true, now - 2s, now - 10ms, now)) {
        std::cerr << "self-test failed: shader reloaded before one second of file stability\n";
        return 1;
    }

    const fs::path includeRoot = fs::temp_directory_path() /
        ("drt-bench-include-test-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(includeRoot / "local");
    {
        std::ofstream(includeRoot / "root.glsl") << "float rootValue = 1.0;\n";
        std::ofstream(includeRoot / "local" / "nested.glsl") <<
            "#include \"/root.glsl\"\nfloat nestedValue = rootValue;\n";
        std::ofstream(includeRoot / "entry.glsl") << "#include \"local/nested.glsl\"\n";
    }
    const std::string expanded = expandShaderIncludes(includeRoot / "entry.glsl", includeRoot);
    std::error_code includeError;
    fs::remove_all(includeRoot, includeError);
    if (expanded.find("#include") != std::string::npos ||
        expanded.find("rootValue") == std::string::npos ||
        expanded.find("nestedValue") == std::string::npos) {
        std::cerr << "self-test failed: shader includes were not expanded\n";
        return 1;
    }

    const fs::path exrPath = fs::temp_directory_path() / "drt-bench-self-test.exr";
    const float source[] = {
        0.0f, 0.25f, 1.0f, 1.0f,
        2.0f, 0.5f, 0.125f, 1.0f,
    };
    const char* error = nullptr;
    if (SaveEXR(source, 2, 1, 4, 1, exrPath.string().c_str(), &error) != TINYEXR_SUCCESS) {
        if (error) FreeEXRErrorMessage(error);
        return 1;
    }
    const auto encoded = readFile(exrPath);
    float* decoded = nullptr;
    int decodedWidth = 0;
    int decodedHeight = 0;
    const int loaded = LoadEXRFromMemory(
        &decoded, &decodedWidth, &decodedHeight,
        reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size(), &error);
    std::error_code ignored;
    fs::remove(exrPath, ignored);
    if (loaded != TINYEXR_SUCCESS || decodedWidth != 2 || decodedHeight != 1) {
        if (error) FreeEXRErrorMessage(error);
        free(decoded);
        return 1;
    }
    free(decoded);
    std::cout << "self-test passed\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") return selfTest();
    try {
        const fs::path executable = fs::absolute(pathFromUtf8(argv[0]));
        App app(executable);
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        MessageBoxA(nullptr, error.what(), "drt-bench", MB_OK | MB_ICONERROR);
        return 1;
    }
}
