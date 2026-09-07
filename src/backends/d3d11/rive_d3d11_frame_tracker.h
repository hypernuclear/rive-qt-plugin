#pragma once

#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

// One tracker per shared RenderContext, used only on its render thread.
// A single outstanding event bounds query storage. While it is pending,
// later flushes remain unretired; the next event covers their entire prefix.
class RiveD3D11FrameTracker
{
public:
    HRESULT initialize(ID3D11Device* device)
    {
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_EVENT;
        return device->CreateQuery(&desc, m_event.GetAddressOf());
    }

    HRESULT poll(ID3D11DeviceContext* context)
    {
        if (!m_pendingFrame)
            return S_OK;
        BOOL complete = FALSE;
        const HRESULT hr = context->GetData(m_event.Get(), &complete,
                                            sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && complete) {
            m_safeFrame = m_pendingFrame;
            m_pendingFrame = 0;
        }
        return hr;
    }

    uint64_t nextFrame() { return ++m_currentFrame; }
    uint64_t safeFrame() const { return m_safeFrame; }

    void submitted(ID3D11DeviceContext* context)
    {
        if (!m_pendingFrame) {
            // End follows Rive's flush on Qt's immediate context. Never
            // reissue an event until its previous result has been consumed.
            context->End(m_event.Get());
            m_pendingFrame = m_currentFrame;
        }
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Query> m_event;
    uint64_t m_currentFrame = 0;
    uint64_t m_safeFrame = 0;
    uint64_t m_pendingFrame = 0;
};
