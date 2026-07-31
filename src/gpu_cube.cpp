#include "gpu_cube.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " +
                                 std::to_string(result));
    }
}

std::vector<uint32_t> readSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    const std::streamoff byteCount = file.tellg();
    if (byteCount <= 0 || byteCount % static_cast<std::streamoff>(sizeof(uint32_t)) != 0) {
        throw std::runtime_error("invalid SPIR-V file " + path.string());
    }
    std::vector<uint32_t> words(static_cast<std::size_t>(byteCount) / sizeof(uint32_t));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(words.data()), byteCount)) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return words;
}

VkShaderModule createModule(VkDevice device, const std::filesystem::path& path) {
    const std::vector<uint32_t> words = readSpirv(path);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = words.size() * sizeof(uint32_t);
    info.pCode = words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule(cube)");
    return module;
}

std::string axisName(const wchar_t* wide) {
    std::string result;
    while (*wide) result.push_back(static_cast<char>(*wide++));
    return result;
}

}

GpuCubeWindow::~GpuCubeWindow() {
    if (window_) DestroyWindow(window_);
}

void GpuCubeWindow::create(HWND owner, CubeSpace space, std::size_t placementIndex,
                           RedrawCallback redraw) {
    space_ = space;
    redraw_ = std::move(redraw);
    const HINSTANCE module = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = module;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"drt-bench-gpu-color-cube";
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    constexpr int width = 500;
    constexpr int height = 500;
    RECT ownerBounds{};
    RECT work{};
    GetWindowRect(owner, &ownerBounds);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const bool onRight = placementIndex % 2 == 0;
    const int cascade = static_cast<int>(placementIndex / 2) * 28;
    int x = onRight ? ownerBounds.right + 12 + cascade
                    : ownerBounds.left - width - 12 - cascade;
    if (onRight && x + width > work.right) x = ownerBounds.left - width - 12 - cascade;
    if (!onRight && x < work.left) x = ownerBounds.right + 12 + cascade;
    x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
    const int y = std::clamp(static_cast<int>(ownerBounds.top) + cascade,
                             static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName,
                              cubeDescription(space).title,
                              WS_CAPTION | WS_THICKFRAME | WS_SYSMENU,
                              x, y, width, height, owner, nullptr, module, this);
    if (window_) ShowWindow(window_, SW_SHOWNOACTIVATE);
}

bool GpuCubeWindow::visible() const {
    return window_ && IsWindowVisible(window_);
}

bool GpuCubeWindow::consumeResize() {
    return std::exchange(resizePending_, false);
}

void GpuCubeWindow::requestRedraw() {
    if (redraw_) redraw_();
}

LRESULT CALLBACK GpuCubeWindow::windowProc(HWND window, UINT message,
                                           WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<GpuCubeWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<GpuCubeWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            self->resizePending_ = true;
            self->requestRedraw();
        }
        return 0;
    case WM_LBUTTONDOWN:
        self->dragging_ = true;
        self->dragPoint_ = MAKEPOINTS(lparam);
        SetCapture(window);
        return 0;
    case WM_MOUSEMOVE:
        if (self->dragging_ && (wparam & MK_LBUTTON)) {
            const POINTS point = MAKEPOINTS(lparam);
            self->yaw_ += (point.x - self->dragPoint_.x) * 0.01f;
            self->pitch_ = std::clamp(
                self->pitch_ + (point.y - self->dragPoint_.y) * 0.01f, -1.4f, 1.4f);
            self->dragPoint_ = point;
            self->requestRedraw();
        }
        return 0;
    case WM_LBUTTONUP:
        if (self->dragging_) {
            self->dragging_ = false;
            if (GetCapture() == window) ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        self->dragging_ = false;
        return 0;
    case WM_RBUTTONUP:
        self->yaw_ = 0.0f;
        self->pitch_ = 0.0f;
        self->requestRedraw();
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        self->requestRedraw();
        return 0;
    case WM_NCDESTROY:
        self->window_ = nullptr;
        return DefWindowProcW(window, message, wparam, lparam);
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

struct GpuCubeRenderer::Impl {
    struct alignas(16) CubeUniform {
        std::array<float, 4> view{};
        std::array<float, 4> source{};
        std::array<float, 4> rangeMinimum{};
        std::array<float, 4> rangeMaximum{};
        std::array<uint32_t, 4> meta{};
        std::array<uint32_t, 4> labelLengths0{};
        std::array<uint32_t, 4> labelLengths1{};
        std::array<std::array<uint32_t, 4>, 36> labelCharacters{};
    };

    struct UniformFrame {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };

    struct PointFrame {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct Cube {
        GpuCubeWindow* window = nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<bool> initialized;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets;
        std::vector<UniformFrame> uniforms;
        std::vector<PointFrame> points;
        bool recreate = false;
        uint32_t acquiredImage = 0;
        bool acquired = false;
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout peakDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout cubeDescriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout peakPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout cubePipelineLayout = VK_NULL_HANDLE;
    VkPipeline peakPipeline = VK_NULL_HANDLE;
    VkPipeline cubePipeline = VK_NULL_HANDLE;
    VkDescriptorPool peakDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet peakDescriptorSet = VK_NULL_HANDLE;
    VkBuffer peakBuffer = VK_NULL_HANDLE;
    VkDeviceMemory peakMemory = VK_NULL_HANDLE;
    uint32_t* peakMapped = nullptr;
    float peakRelative = 1.0f;
    std::vector<Cube> cubes;
    std::vector<Cube*> acquired;
    std::vector<VkSemaphore> waits;

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags required) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & required) == required) return i;
        }
        throw std::runtime_error("no compatible Vulkan memory type for GPU cube");
    }

    void createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer(cube)");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &allocation, nullptr, &memory), "vkAllocateMemory(cube)");
        check(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory(cube)");
        check(vkMapMemory(device, memory, 0, size, 0, &mapped), "vkMapMemory(cube)");
    }

    void createDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            VkBuffer& buffer, VkDeviceMemory& memory) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer),
              "vkCreateBuffer(cube points)");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &allocation, nullptr, &memory),
              "vkAllocateMemory(cube points)");
        check(vkBindBufferMemory(device, buffer, memory, 0),
              "vkBindBufferMemory(cube points)");
    }

    void createPipelines(const std::filesystem::path& shaderDirectory) {
        VkDescriptorSetLayoutBinding peakBindings[2]{};
        peakBindings[0].binding = 0;
        peakBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        peakBindings[0].descriptorCount = 1;
        peakBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        peakBindings[1].binding = 1;
        peakBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        peakBindings[1].descriptorCount = 1;
        peakBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo peakLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        peakLayoutInfo.bindingCount = 2;
        peakLayoutInfo.pBindings = peakBindings;
        check(vkCreateDescriptorSetLayout(device, &peakLayoutInfo, nullptr,
                                          &peakDescriptorLayout),
              "vkCreateDescriptorSetLayout(cube peak)");

        VkDescriptorSetLayoutBinding cubeBindings[4]{};
        cubeBindings[0] = peakBindings[0];
        cubeBindings[1].binding = 1;
        cubeBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        cubeBindings[1].descriptorCount = 1;
        cubeBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        cubeBindings[2].binding = 2;
        cubeBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        cubeBindings[2].descriptorCount = 1;
        cubeBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        cubeBindings[3].binding = 3;
        cubeBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        cubeBindings[3].descriptorCount = 1;
        cubeBindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo cubeLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        cubeLayoutInfo.bindingCount = 4;
        cubeLayoutInfo.pBindings = cubeBindings;
        check(vkCreateDescriptorSetLayout(device, &cubeLayoutInfo, nullptr,
                                          &cubeDescriptorLayout),
              "vkCreateDescriptorSetLayout(cube render)");

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = sizeof(uint32_t);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &peakDescriptorLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr,
                                     &peakPipelineLayout),
              "vkCreatePipelineLayout(cube peak)");
        pipelineLayoutInfo.pSetLayouts = &cubeDescriptorLayout;
        check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr,
                                     &cubePipelineLayout),
              "vkCreatePipelineLayout(cube render)");

        const VkShaderModule peakModule = createModule(device, shaderDirectory / "cube_peak.comp.spv");
        const VkShaderModule cubeModule = createModule(device, shaderDirectory / "cube_render.comp.spv");
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.pName = "main";
        info.stage.module = peakModule;
        info.layout = peakPipelineLayout;
        const VkResult peakResult = vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1, &info, nullptr, &peakPipeline);
        info.stage.module = cubeModule;
        info.layout = cubePipelineLayout;
        const VkResult cubeResult = vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1, &info, nullptr, &cubePipeline);
        vkDestroyShaderModule(device, peakModule, nullptr);
        vkDestroyShaderModule(device, cubeModule, nullptr);
        check(peakResult, "vkCreateComputePipelines(cube peak)");
        check(cubeResult, "vkCreateComputePipelines(cube render)");
    }

    void createPeakResources() {
        void* mapped = nullptr;
        createHostBuffer(sizeof(uint32_t),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         peakBuffer, peakMemory, mapped);
        peakMapped = static_cast<uint32_t*>(mapped);
        *peakMapped = std::bit_cast<uint32_t>(1.0f);

        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &peakDescriptorPool),
              "vkCreateDescriptorPool(cube peak)");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = peakDescriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &peakDescriptorLayout;
        check(vkAllocateDescriptorSets(device, &allocate, &peakDescriptorSet),
              "vkAllocateDescriptorSets(cube peak)");

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = peakBuffer;
        bufferInfo.range = sizeof(uint32_t);
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = peakDescriptorSet;
        write.dstBinding = 1;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    void destroySwapchainResources(Cube& cube) {
        if (cube.descriptorPool) vkDestroyDescriptorPool(device, cube.descriptorPool, nullptr);
        cube.descriptorPool = VK_NULL_HANDLE;
        cube.descriptorSets.clear();
        for (UniformFrame& uniform : cube.uniforms) {
            if (uniform.mapped) vkUnmapMemory(device, uniform.memory);
            if (uniform.buffer) vkDestroyBuffer(device, uniform.buffer, nullptr);
            if (uniform.memory) vkFreeMemory(device, uniform.memory, nullptr);
        }
        cube.uniforms.clear();
        for (PointFrame& points : cube.points) {
            if (points.buffer) vkDestroyBuffer(device, points.buffer, nullptr);
            if (points.memory) vkFreeMemory(device, points.memory, nullptr);
        }
        cube.points.clear();
        for (VkImageView view : cube.views) vkDestroyImageView(device, view, nullptr);
        cube.views.clear();
        cube.images.clear();
        cube.initialized.clear();
        if (cube.swapchain) vkDestroySwapchainKHR(device, cube.swapchain, nullptr);
        cube.swapchain = VK_NULL_HANDLE;
        cube.extent = {};
    }

    void destroyCube(Cube& cube) {
        destroySwapchainResources(cube);
        if (cube.imageAvailable) vkDestroySemaphore(device, cube.imageAvailable, nullptr);
        if (cube.surface) vkDestroySurfaceKHR(instance, cube.surface, nullptr);
        cube = {};
    }

    VkSurfaceFormatKHR chooseCubeFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
        constexpr VkFormat preferred[] = {
            VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
        for (VkFormat wanted : preferred) {
            for (const VkSurfaceFormatKHR format : formats) {
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(physicalDevice, format.format, &properties);
                if (format.format == wanted &&
                    format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                    (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
                    return format;
                }
            }
        }
        throw std::runtime_error("cube window has no storage-capable SDR surface format");
    }

    void createCubeDescriptors(Cube& cube) {
        const uint32_t count = static_cast<uint32_t>(cube.images.size());
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count},
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = count;
        poolInfo.poolSizeCount = 4;
        poolInfo.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &cube.descriptorPool),
              "vkCreateDescriptorPool(cube render)");
        cube.descriptorSets.resize(count);
        std::vector<VkDescriptorSetLayout> layouts(count, cubeDescriptorLayout);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = cube.descriptorPool;
        allocate.descriptorSetCount = count;
        allocate.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(device, &allocate, cube.descriptorSets.data()),
              "vkAllocateDescriptorSets(cube render)");
        cube.uniforms.resize(count);
        cube.points.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            UniformFrame& uniform = cube.uniforms[i];
            createHostBuffer(sizeof(CubeUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             uniform.buffer, uniform.memory, uniform.mapped);
            PointFrame& points = cube.points[i];
            const VkDeviceSize pointBytes = static_cast<VkDeviceSize>(cube.extent.width) *
                                            cube.extent.height * sizeof(uint32_t);
            createDeviceBuffer(pointBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               points.buffer, points.memory);
            VkDescriptorImageInfo outputInfo{};
            outputInfo.imageView = cube.views[i];
            outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorBufferInfo uniformInfo{};
            uniformInfo.buffer = uniform.buffer;
            uniformInfo.range = sizeof(CubeUniform);
            VkDescriptorBufferInfo pointInfo{};
            pointInfo.buffer = points.buffer;
            pointInfo.range = pointBytes;
            VkWriteDescriptorSet writes[3]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = cube.descriptorSets[i];
            writes[0].dstBinding = 1;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[0].pImageInfo = &outputInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = cube.descriptorSets[i];
            writes[1].dstBinding = 2;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].pBufferInfo = &uniformInfo;
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = cube.descriptorSets[i];
            writes[2].dstBinding = 3;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo = &pointInfo;
            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }
    }

    void recreateCubeSwapchain(Cube& cube) {
        check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(cube resize)");
        destroySwapchainResources(cube);
        RECT client{};
        GetClientRect(cube.window->handle(), &client);
        if (client.right <= client.left || client.bottom <= client.top) return;

        VkBool32 present = VK_FALSE;
        check(vkGetPhysicalDeviceSurfaceSupportKHR(
                  physicalDevice, queueFamily, cube.surface, &present),
              "vkGetPhysicalDeviceSurfaceSupportKHR(cube)");
        if (!present) throw std::runtime_error("selected Vulkan queue cannot present a cube window");
        VkSurfaceCapabilitiesKHR capabilities{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                  physicalDevice, cube.surface, &capabilities),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR(cube)");
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT)) {
            throw std::runtime_error("cube swapchain does not support storage images");
        }
        uint32_t formatCount = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(
                  physicalDevice, cube.surface, &formatCount, nullptr),
              "vkGetPhysicalDeviceSurfaceFormatsKHR(cube)");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(
                  physicalDevice, cube.surface, &formatCount, formats.data()),
              "vkGetPhysicalDeviceSurfaceFormatsKHR(cube)");
        const VkSurfaceFormatKHR selected = chooseCubeFormat(formats);

        VkExtent2D extent{};
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            extent = capabilities.currentExtent;
        } else {
            extent.width = std::clamp<uint32_t>(client.right - client.left,
                                                capabilities.minImageExtent.width,
                                                capabilities.maxImageExtent.width);
            extent.height = std::clamp<uint32_t>(client.bottom - client.top,
                                                 capabilities.minImageExtent.height,
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
        for (const VkCompositeAlphaFlagBitsKHR candidate : alphaCandidates) {
            if (capabilities.supportedCompositeAlpha & candidate) {
                alpha = candidate;
                break;
            }
        }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = cube.surface;
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
        check(vkCreateSwapchainKHR(device, &info, nullptr, &cube.swapchain),
              "vkCreateSwapchainKHR(cube)");
        cube.format = selected.format;
        cube.extent = extent;
        check(vkGetSwapchainImagesKHR(device, cube.swapchain, &imageCount, nullptr),
              "vkGetSwapchainImagesKHR(cube count)");
        cube.images.resize(imageCount);
        check(vkGetSwapchainImagesKHR(device, cube.swapchain, &imageCount, cube.images.data()),
              "vkGetSwapchainImagesKHR(cube)");
        cube.views.resize(imageCount);
        cube.initialized.assign(imageCount, false);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = cube.images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = cube.format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            check(vkCreateImageView(device, &viewInfo, nullptr, &cube.views[i]),
                  "vkCreateImageView(cube)");
        }
        createCubeDescriptors(cube);
        cube.recreate = false;
    }

    void add(GpuCubeWindow& window) {
        Cube cube{};
        cube.window = &window;
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = GetModuleHandleW(nullptr);
        surfaceInfo.hwnd = window.handle();
        check(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &cube.surface),
              "vkCreateWin32SurfaceKHR(cube)");
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &cube.imageAvailable),
              "vkCreateSemaphore(cube)");
        cubes.push_back(std::move(cube));
        recreateCubeSwapchain(cubes.back());
    }

    void removeClosed() {
        const bool anyClosed = std::any_of(cubes.begin(), cubes.end(),
            [](const Cube& cube) { return !cube.window->visible(); });
        if (!anyClosed) return;
        check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(remove cube)");
        std::erase_if(cubes, [this](Cube& cube) {
            if (cube.window->visible()) return false;
            destroyCube(cube);
            return true;
        });
    }

    static void copyLabel(CubeUniform& uniform, uint32_t label, const std::string& text) {
        const uint32_t length = static_cast<uint32_t>(std::min<std::size_t>(text.size(), 24));
        if (label < 4) uniform.labelLengths0[label] = length;
        else uniform.labelLengths1[label - 4] = length;
        for (uint32_t i = 0; i < length; ++i) {
            const uint32_t index = label * 24 + i;
            uniform.labelCharacters[index / 4][index % 4] =
                static_cast<unsigned char>(text[i]);
        }
    }

    CubeUniform makeUniform(const Cube& cube, VkExtent2D sourceExtent, bool hdr) const {
        CubeUniform uniform{};
        uniform.view = {cube.window->yaw(), cube.window->pitch(),
                        static_cast<float>(cube.extent.width),
                        static_cast<float>(cube.extent.height)};
        const double pixelCount = static_cast<double>(sourceExtent.width) * sourceExtent.height;
        const float step = static_cast<float>(std::max(1.0,
            std::ceil(std::sqrt(pixelCount / 100000.0))));
        uniform.source = {static_cast<float>(sourceExtent.width),
                          static_cast<float>(sourceExtent.height), step, hdr ? 1.0f : 0.0f};
        const CubeRanges ranges = cubeRanges(cube.window->space(), hdr, peakRelative);
        uniform.rangeMinimum = {ranges.first.minimum, ranges.second.minimum,
                                ranges.vertical.minimum, 0.0f};
        uniform.rangeMaximum = {ranges.first.maximum, ranges.second.maximum,
                                ranges.vertical.maximum, 0.0f};
        uniform.meta[0] = static_cast<uint32_t>(cube.window->space());

        const CubeDescription& description = cubeDescription(cube.window->space());
        const float minima[3] = {
            ranges.first.minimum, ranges.second.minimum, ranges.vertical.minimum};
        const float maxima[3] = {
            ranges.first.maximum, ranges.second.maximum, ranges.vertical.maximum};
        std::array<std::string, 3> ownedNames = {
            axisName(description.first), axisName(description.second), axisName(description.vertical)};
        for (uint32_t axis = 0; axis < 3; ++axis) {
            char minimum[24]{};
            char maximum[24]{};
            std::snprintf(minimum, sizeof(minimum), "%s %.4g", ownedNames[axis].c_str(), minima[axis]);
            std::snprintf(maximum, sizeof(maximum), "%s %.4g", ownedNames[axis].c_str(), maxima[axis]);
            copyLabel(uniform, axis, minimum);
            copyLabel(uniform, axis + 3, maximum);
        }
        return uniform;
    }

    void acquire() {
        waits.clear();
        acquired.clear();
        for (Cube& cube : cubes) {
            cube.acquired = false;
            if (!cube.window->visible()) continue;
            if (cube.window->consumeResize() || cube.recreate || !cube.swapchain) {
                recreateCubeSwapchain(cube);
            }
            if (!cube.swapchain || !cube.extent.width || !cube.extent.height) continue;
            VkResult result = vkAcquireNextImageKHR(
                device, cube.swapchain, UINT64_MAX, cube.imageAvailable,
                VK_NULL_HANDLE, &cube.acquiredImage);
            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                recreateCubeSwapchain(cube);
                if (!cube.swapchain) continue;
                result = vkAcquireNextImageKHR(
                    device, cube.swapchain, UINT64_MAX, cube.imageAvailable,
                    VK_NULL_HANDLE, &cube.acquiredImage);
            }
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                check(result, "vkAcquireNextImageKHR(cube)");
            }
            cube.recreate = result == VK_SUBOPTIMAL_KHR;
            cube.acquired = true;
            waits.push_back(cube.imageAvailable);
            acquired.push_back(&cube);
        }
    }

    void updateSourceDescriptor(VkDescriptorSet set, VkImageView sourceView) const {
        VkDescriptorImageInfo sourceInfo{};
        sourceInfo.sampler = sampler;
        sourceInfo.imageView = sourceView;
        sourceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &sourceInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    void imageWriteBarrier(VkCommandBuffer commandBuffer, VkImage image) const {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
    }

    void pointWriteBarrier(VkCommandBuffer commandBuffer, VkBuffer buffer) const {
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 1, &barrier, 0, nullptr);
    }

    void record(VkCommandBuffer commandBuffer, VkImageView sourceView,
                VkExtent2D sourceExtent, bool hdr) {
        if (acquired.empty()) return;
        updateSourceDescriptor(peakDescriptorSet, sourceView);
        vkCmdFillBuffer(commandBuffer, peakBuffer, 0, sizeof(uint32_t), 0);
        VkBufferMemoryBarrier peakReady{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        peakReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        peakReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        peakReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        peakReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        peakReady.buffer = peakBuffer;
        peakReady.size = sizeof(uint32_t);
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 1, &peakReady, 0, nullptr);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, peakPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                peakPipelineLayout, 0, 1, &peakDescriptorSet, 0, nullptr);
        const uint32_t hdrPush = hdr ? 1u : 0u;
        vkCmdPushConstants(commandBuffer, peakPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(hdrPush), &hdrPush);
        vkCmdDispatch(commandBuffer, (sourceExtent.width + 7) / 8,
                      (sourceExtent.height + 7) / 8, 1);

        for (Cube* cube : acquired) {
            const uint32_t imageIndex = cube->acquiredImage;
            const CubeUniform uniform = makeUniform(*cube, sourceExtent, hdr);
            std::memcpy(cube->uniforms[imageIndex].mapped, &uniform, sizeof(uniform));
            updateSourceDescriptor(cube->descriptorSets[imageIndex], sourceView);

            VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toGeneral.oldLayout = cube->initialized[imageIndex]
                ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
            toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.image = cube->images[imageIndex];
            toGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toGeneral.subresourceRange.levelCount = 1;
            toGeneral.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &toGeneral);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, cubePipeline);
            VkDescriptorSet set = cube->descriptorSets[imageIndex];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    cubePipelineLayout, 0, 1, &set, 0, nullptr);
            uint32_t pass = 0;
            vkCmdPushConstants(commandBuffer, cubePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(pass), &pass);
            vkCmdDispatch(commandBuffer, (cube->extent.width + 7) / 8,
                          (cube->extent.height + 7) / 8, 1);
            imageWriteBarrier(commandBuffer, cube->images[imageIndex]);
            pointWriteBarrier(commandBuffer, cube->points[imageIndex].buffer);
            pass = 1;
            vkCmdPushConstants(commandBuffer, cubePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(pass), &pass);
            const uint32_t step = std::max(1u, static_cast<uint32_t>(uniform.source[2]));
            const uint32_t sampleWidth = (sourceExtent.width + step - 1) / step;
            const uint32_t sampleHeight = (sourceExtent.height + step - 1) / step;
            vkCmdDispatch(commandBuffer, (sampleWidth + 7) / 8,
                          (sampleHeight + 7) / 8, 1);
            pointWriteBarrier(commandBuffer, cube->points[imageIndex].buffer);
            pass = 2;
            vkCmdPushConstants(commandBuffer, cubePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(pass), &pass);
            vkCmdDispatch(commandBuffer, (cube->extent.width + 7) / 8,
                          (cube->extent.height + 7) / 8, 1);

            VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toPresent.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toPresent.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toPresent.image = cube->images[imageIndex];
            toPresent.subresourceRange = toGeneral.subresourceRange;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &toPresent);
            cube->initialized[imageIndex] = true;
        }

        VkBufferMemoryBarrier peakToHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        peakToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        peakToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        peakToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        peakToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        peakToHost.buffer = peakBuffer;
        peakToHost.size = sizeof(uint32_t);
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &peakToHost, 0, nullptr);
    }

    void shutdown() {
        if (!device) return;
        vkDeviceWaitIdle(device);
        for (Cube& cube : cubes) destroyCube(cube);
        cubes.clear();
        acquired.clear();
        waits.clear();
        if (peakMapped) vkUnmapMemory(device, peakMemory);
        if (peakBuffer) vkDestroyBuffer(device, peakBuffer, nullptr);
        if (peakMemory) vkFreeMemory(device, peakMemory, nullptr);
        if (peakDescriptorPool) vkDestroyDescriptorPool(device, peakDescriptorPool, nullptr);
        if (cubePipeline) vkDestroyPipeline(device, cubePipeline, nullptr);
        if (peakPipeline) vkDestroyPipeline(device, peakPipeline, nullptr);
        if (cubePipelineLayout) vkDestroyPipelineLayout(device, cubePipelineLayout, nullptr);
        if (peakPipelineLayout) vkDestroyPipelineLayout(device, peakPipelineLayout, nullptr);
        if (cubeDescriptorLayout) vkDestroyDescriptorSetLayout(device, cubeDescriptorLayout, nullptr);
        if (peakDescriptorLayout) vkDestroyDescriptorSetLayout(device, peakDescriptorLayout, nullptr);
        *this = {};
    }
};

GpuCubeRenderer::GpuCubeRenderer() : impl_(std::make_unique<Impl>()) {}
GpuCubeRenderer::~GpuCubeRenderer() { shutdown(); }

void GpuCubeRenderer::initialize(VkInstance instance, VkPhysicalDevice physicalDevice,
                                 VkDevice device, VkQueue queue, uint32_t queueFamily,
                                 VkSampler sampler,
                                 const std::filesystem::path& shaderDirectory) {
    impl_->instance = instance;
    impl_->physicalDevice = physicalDevice;
    impl_->device = device;
    impl_->queue = queue;
    impl_->queueFamily = queueFamily;
    impl_->sampler = sampler;
    impl_->createPipelines(shaderDirectory);
    impl_->createPeakResources();
}

void GpuCubeRenderer::shutdown() {
    if (impl_) impl_->shutdown();
}

void GpuCubeRenderer::add(GpuCubeWindow& window) { impl_->add(window); }
void GpuCubeRenderer::removeClosed() { impl_->removeClosed(); }

void GpuCubeRenderer::prepareFrame(bool) {
    if (!impl_->peakMapped) return;
    const float peak = std::bit_cast<float>(*impl_->peakMapped);
    if (std::isfinite(peak)) impl_->peakRelative = std::clamp(peak, 0.0f, 1.0f);
}

void GpuCubeRenderer::acquire() { impl_->acquire(); }

std::span<const VkSemaphore> GpuCubeRenderer::waitSemaphores() const {
    return impl_->waits;
}

void GpuCubeRenderer::record(VkCommandBuffer commandBuffer, VkImageView sourceView,
                             VkExtent2D sourceExtent, bool hdr) {
    impl_->record(commandBuffer, sourceView, sourceExtent, hdr);
}

void GpuCubeRenderer::appendPresent(std::vector<VkSwapchainKHR>& swapchains,
                                    std::vector<uint32_t>& imageIndices) const {
    for (const Impl::Cube* cube : impl_->acquired) {
        swapchains.push_back(cube->swapchain);
        imageIndices.push_back(cube->acquiredImage);
    }
}

bool GpuCubeRenderer::handlePresent(std::span<const VkResult> results) {
    if (results.size() != impl_->acquired.size()) {
        throw std::runtime_error("cube present result count mismatch");
    }
    bool redraw = false;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const VkResult result = results[i];
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            impl_->acquired[i]->recreate = true;
            redraw = true;
        } else {
            check(result, "vkQueuePresentKHR(cube)");
        }
    }
    return redraw;
}

bool GpuCubeRenderer::hasAcquiredImages() const { return !impl_->acquired.empty(); }
