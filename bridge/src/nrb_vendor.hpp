#pragma once

// nr-bridge [NRB3]: which GPU vendor sits behind a D3D12 device.
//
// Upstream assumes the game renders on NVIDIA and calls NvAPI (Reflex) and
// patches NGX import slots on the game's device. With the game on an AMD card
// those calls would land on an AMD ID3D12Device, so the call sites ask this
// first. Uses CreateDXGIFactory2 like adapter.cpp (T2), the in-tree precedent.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

namespace nrb
{
    constexpr UINT kVendorNvidia = 0x10DE;

    inline UINT vendor_of_luid(LUID luid)
    {
        IDXGIFactory4 *f = nullptr;
        if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void **>(&f))) || f == nullptr)
            return 0;
        UINT vendor = 0;
        IDXGIAdapter1 *a = nullptr;
        if (SUCCEEDED(f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&a))) && a != nullptr)
        {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(a->GetDesc1(&d))) vendor = d.VendorId;
            a->Release();
        }
        f->Release();
        return vendor;
    }

    // 0 when unknown (no device, or the LUID did not enumerate).
    inline UINT vendor_of_device(ID3D12Device *dev)
    {
        return dev != nullptr ? vendor_of_luid(dev->GetAdapterLuid()) : 0;
    }
}
