#include "../../src/backends/d3d11/rive_d3d11_frame_tracker.h"

#include <chrono>
#include <iostream>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " #condition "\n"; return 1; \
} } while (false)

int main()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    RiveD3D11FrameTracker tracker;
    CHECK(SUCCEEDED(tracker.initialize(device.Get())));
    CHECK(tracker.safeFrame() == 0);

    auto waitFor = [&](uint64_t frame) {
        context->Flush(); // Test submission; production uses Qt's normal present.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (tracker.safeFrame() < frame && std::chrono::steady_clock::now() < deadline) {
            if (FAILED(tracker.poll(context.Get()))) return false;
            std::this_thread::yield();
        }
        return tracker.safeFrame() == frame;
    };

    CHECK(tracker.nextFrame() == 1);
    tracker.submitted(context.Get());
    for (uint64_t frame = 2; frame <= 20; ++frame) {
        CHECK(tracker.nextFrame() == frame);
        tracker.submitted(context.Get());
    }
    // Submission alone never retires anything. The outstanding query covers
    // only frame 1, even though many views have submitted subsequent flushes.
    CHECK(tracker.safeFrame() == 0);
    CHECK(waitFor(1));
    tracker.submitted(context.Get());
    CHECK(waitFor(20));

    uint64_t current = 20;
    ComPtr<ID3D11Buffer> buffer;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 16;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CHECK(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &buffer)));
    for (int views : {1, 8, 2, 13, 1, 4}) {
        for (int frame = 0; frame < 20; ++frame) {
            for (int view = 0; view < views; ++view) {
                const auto previousSafe = tracker.safeFrame();
                CHECK(SUCCEEDED(tracker.poll(context.Get())));
                CHECK(tracker.safeFrame() >= previousSafe);
                CHECK(tracker.safeFrame() <= current);
                CHECK(tracker.nextFrame() == ++current);
                const uint32_t data[4] = {uint32_t(current), 0, 0, 0};
                context->UpdateSubresource(buffer.Get(), 0, nullptr, data, 0, 0);
                tracker.submitted(context.Get());
            }
            context->Flush();
        }
    }
    // Drain the old marker, then mark the entire submitted prefix.
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tracker.safeFrame() < current && std::chrono::steady_clock::now() < deadline) {
        CHECK(SUCCEEDED(tracker.poll(context.Get())));
        tracker.submitted(context.Get());
        context->Flush();
        std::this_thread::yield();
    }
    CHECK(tracker.safeFrame() == current);
    std::cout << "D3D11 completion tracking passed with changing view counts.\n";
}
