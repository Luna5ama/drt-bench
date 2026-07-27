#include <windows.h>
#include <tinyexr.h>
#include <webp/decode.h>
#include <webp/encode.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "opendrt.hpp"

namespace fs = std::filesystem;

namespace {

struct ExrPixels {
    std::unique_ptr<float, decltype(&std::free)> rgba{nullptr, &std::free};
    EXRBox2i data{};
    EXRBox2i display{};
    int dataWidth = 0;
    int dataHeight = 0;
};

struct OutputInfo {
    fs::path path;
    int width;
    int height;
};

std::string takeExrError(const char* error) {
    const std::string message = error ? error : "unknown TinyEXR error";
    if (error) FreeEXRErrorMessage(error);
    return message;
}

std::wstring lowercase(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

bool isExr(const fs::path& path) {
    return lowercase(path.extension().wstring()) == L".exr";
}

void addFile(const fs::path& path, std::vector<fs::path>& files) {
    if (!fs::is_regular_file(path)) throw std::runtime_error("not a file: " + path.string());
    if (!isExr(path)) throw std::runtime_error("unsupported input: " + path.string());
    files.push_back(fs::canonical(path));
}

void expandSpec(const fs::path& spec, std::vector<fs::path>& files) {
    const size_t before = files.size();
    if (fs::is_directory(spec)) {
        for (const auto& entry : fs::directory_iterator(spec)) {
            if (entry.is_regular_file() && isExr(entry.path())) files.push_back(fs::canonical(entry.path()));
        }
    } else if (spec.wstring().find_first_of(L"*?") != std::wstring::npos) {
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileW(spec.c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            const fs::path parent = spec.parent_path().empty() ? fs::path(L".") : spec.parent_path();
            do {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    const fs::path path = parent / data.cFileName;
                    if (isExr(path)) files.push_back(fs::canonical(path));
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    } else {
        addFile(spec, files);
    }
    if (files.size() == before) throw std::runtime_error("matched no EXR files: " + spec.string());
}

std::vector<fs::path> uniqueFiles(std::vector<fs::path> files) {
    const auto key = [](const fs::path& path) { return lowercase(path.wstring()); };
    std::ranges::sort(files, {}, key);
    files.erase(std::unique(files.begin(), files.end(), [&](const auto& a, const auto& b) {
        return key(a) == key(b);
    }), files.end());
    return files;
}

ExrPixels loadExr(const fs::path& path) {
    EXRVersion version{};
    if (ParseEXRVersionFromFile(&version, path.string().c_str()) != TINYEXR_SUCCESS) {
        throw std::runtime_error("invalid EXR: " + path.string());
    }
    EXRHeader header{};
    InitEXRHeader(&header);
    const char* error = nullptr;
    if (ParseEXRHeaderFromFile(&header, &version, path.string().c_str(), &error) != TINYEXR_SUCCESS) {
        const std::string message = takeExrError(error);
        FreeEXRHeader(&header);
        throw std::runtime_error(message);
    }
    ExrPixels image;
    image.data = header.data_window;
    image.display = header.display_window;
    FreeEXRHeader(&header);

    float* rgba = nullptr;
    if (LoadEXR(&rgba, &image.dataWidth, &image.dataHeight, path.string().c_str(), &error) != TINYEXR_SUCCESS) {
        throw std::runtime_error(takeExrError(error));
    }
    image.rgba.reset(rgba);
    if (image.dataWidth != image.data.max_x - image.data.min_x + 1 ||
        image.dataHeight != image.data.max_y - image.data.min_y + 1) {
        throw std::runtime_error("EXR data window mismatch: " + path.string());
    }
    return image;
}

OutputInfo convertFile(const fs::path& input) {
    const ExrPixels source = loadExr(input);
    const int width = source.display.max_x - source.display.min_x + 1;
    const int height = source.display.max_y - source.display.min_y + 1;
    if (width <= 0 || height <= 0 || width > WEBP_MAX_DIMENSION || height > WEBP_MAX_DIMENSION) {
        throw std::runtime_error("unsupported display window: " + input.string());
    }

    const glm::mat3 ap0ToXyz(
        0.9525523959f, 0.0f, 9.36786e-05f,
        0.3439664498f, 0.7281660966f, -0.0721325464f,
        0.0f, 0.0f, 1.0088251844f);
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int globalX = source.display.min_x + x;
            const int globalY = source.display.min_y + y;
            glm::vec3 ap0(0.0f);
            if (globalX >= source.data.min_x && globalX <= source.data.max_x &&
                globalY >= source.data.min_y && globalY <= source.data.max_y) {
                const size_t pixel = static_cast<size_t>(globalY - source.data.min_y) * source.dataWidth
                                   + static_cast<size_t>(globalX - source.data.min_x);
                const float* rgba = source.rgba.get();
                ap0 = {rgba[pixel * 4], rgba[pixel * 4 + 1], rgba[pixel * 4 + 2]};
                for (int channel = 0; channel < 3; ++channel) {
                    if (!std::isfinite(ap0[channel])) ap0[channel] = 0.0f;
                }
            }
            const glm::vec3 encoded = openDRT(ap0 * ap0ToXyz);
            for (int channel = 0; channel < 3; ++channel) {
                const float value = std::isfinite(encoded[channel]) ? encoded[channel] : 0.0f;
                rgb[(static_cast<size_t>(y) * width + x) * 3 + static_cast<size_t>(channel)] =
                    static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            }
        }
    }

    uint8_t* encoded = nullptr;
    const size_t size = WebPEncodeLosslessRGB(rgb.data(), width, height, width * 3, &encoded);
    std::unique_ptr<uint8_t, decltype(&WebPFree)> webp(encoded, &WebPFree);
    if (!size) throw std::runtime_error("WebP lossless encoding failed: " + input.string());

    fs::path output = input;
    output.replace_extension(L".webp");
    fs::path temporary = output;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary);
        if (!file.write(reinterpret_cast<const char*>(webp.get()), static_cast<std::streamsize>(size))) {
            file.close();
            fs::remove(temporary);
            throw std::runtime_error("cannot write " + output.string());
        }
    }
    if (!MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temporary);
        throw std::runtime_error("cannot replace " + output.string());
    }
    return {output, width, height};
}

int selfTest() {
    const fs::path root = fs::temp_directory_path() /
        (L"opendrt-convert-self-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    const fs::path first = root / L"a.exr";
    const fs::path second = root / L"b.exr";
    const float pixels[] = {0.0f, 0.18f, 1.0f, 1.0f, 2.0f, 0.5f, 0.1f, 1.0f};
    const char* error = nullptr;
    if (SaveEXR(pixels, 2, 1, 4, 1, first.string().c_str(), &error) != TINYEXR_SUCCESS) {
        std::cerr << takeExrError(error) << '\n';
        return 1;
    }
    fs::copy_file(first, second, fs::copy_options::overwrite_existing);
    std::vector<fs::path> directoryFiles;
    std::vector<fs::path> wildcardFiles;
    expandSpec(root, directoryFiles);
    expandSpec(root / L"*.exr", wildcardFiles);
    const OutputInfo output = convertFile(first);
    int width = 0;
    int height = 0;
    std::ifstream file(output.path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    const bool valid = directoryFiles.size() == 2 && wildcardFiles.size() == 2 &&
        WebPGetInfo(bytes.data(), bytes.size(), &width, &height) && width == 2 && height == 1 &&
        bytes.size() >= 16 && std::string_view(reinterpret_cast<const char*>(bytes.data() + 12), 4) == "VP8L";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    std::cout << (valid ? "self-test passed\n" : "self-test failed\n");
    return valid ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--self-test") return selfTest();
    if (argc < 2) {
        std::cerr << "usage: opendrt-convert <file|wildcard|directory> [...]\n";
        return 2;
    }

    std::vector<fs::path> inputs;
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        try {
            expandSpec(argv[i], inputs);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            ++failures;
        }
    }
    inputs = uniqueFiles(std::move(inputs));
    if (inputs.empty()) return 1;

    std::atomic_size_t next = 0;
    std::atomic_int conversionFailures = 0;
    std::mutex outputMutex;
    const unsigned workers = std::min({8u, std::max(1u, std::thread::hardware_concurrency()),
                                       static_cast<unsigned>(inputs.size())});
    std::vector<std::jthread> threads;
    threads.reserve(workers);
    for (unsigned worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&] {
            while (true) {
                const size_t index = next.fetch_add(1);
                if (index >= inputs.size()) return;
                try {
                    const OutputInfo output = convertFile(inputs[index]);
                    std::lock_guard lock(outputMutex);
                    std::wcout << inputs[index] << L" -> " << output.path
                               << L" (" << output.width << L'x' << output.height << L")\n";
                } catch (const std::exception& error) {
                    std::lock_guard lock(outputMutex);
                    std::cerr << inputs[index].string() << ": " << error.what() << '\n';
                    ++conversionFailures;
                }
            }
        });
    }
    threads.clear();
    return failures || conversionFailures ? 1 : 0;
}
