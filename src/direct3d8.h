#pragma once

#include <dxgi.h>

#include <unordered_map>
#include <vector>

#include "d3d8.h"
#include "util.h"

interface IDXGIAdapter;
interface IDXGIFactory2;
interface IDXGIOutput;
interface ID3D12Device;

namespace Dx8to12 {
class Direct3D8 : public IDirect3D8, RefCounted {
 public:
  Direct3D8();
  virtual ~Direct3D8();

  virtual __declspec(nothrow) HRESULT STDMETHODCALLTYPE
      QueryInterface(REFIID riid, void **ppvObj) override;

  virtual __declspec(nothrow) ULONG STDMETHODCALLTYPE AddRef() override {
    return RefCounted::AddRef();
  }

  virtual __declspec(nothrow) ULONG STDMETHODCALLTYPE Release() override {
    return RefCounted::Release();
  }

  virtual __declspec(nothrow) HRESULT STDMETHODCALLTYPE
      RegisterSoftwareDevice(void *pInitializeFunction) override {
    NOT_IMPLEMENTED();
  }

  virtual __declspec(nothrow) UINT STDMETHODCALLTYPE GetAdapterCount() override;

  virtual __declspec(nothrow) HRESULT __stdcall GetAdapterIdentifier(
      UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8 *pIdentifier) override;

  virtual __declspec(nothrow) UINT
      __stdcall GetAdapterModeCount(UINT Adapter) override;

  virtual __declspec(nothrow) HRESULT
      __stdcall EnumAdapterModes(UINT Adapter, UINT Mode,
                                 D3DDISPLAYMODE *pMode) override;

  virtual __declspec(nothrow) HRESULT
      __stdcall GetAdapterDisplayMode(UINT Adapter,
                                      D3DDISPLAYMODE *pMode) override;

  virtual __declspec(nothrow) HRESULT
      __stdcall CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType,
                                D3DFORMAT DisplayFormat,
                                D3DFORMAT BackBufferFormat,
                                BOOL Windowed) override;

  virtual __declspec(nothrow) HRESULT
      __stdcall CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType,
                                  D3DFORMAT AdapterFormat, DWORD Usage,
                                  D3DRESOURCETYPE RType,
                                  D3DFORMAT CheckFormat) override;

  virtual __declspec(nothrow) HRESULT __stdcall CheckDeviceMultiSampleType(
      UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat,
      BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType) override;

  virtual __declspec(nothrow) HRESULT
      __stdcall CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType,
                                       D3DFORMAT AdapterFormat,
                                       D3DFORMAT RenderTargetFormat,
                                       D3DFORMAT DepthStencilFormat) override {
    return S_OK;
  }

  virtual __declspec(nothrow) HRESULT
      __stdcall GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType,
                              D3DCAPS8 *pCaps) override;

  virtual __declspec(nothrow) HMONITOR
      __stdcall GetAdapterMonitor(UINT Adapter) override;

  virtual __declspec(nothrow) HRESULT __stdcall CreateDevice(
      UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
      DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters,
      IDirect3DDevice8 **ppReturnedDeviceInterface) override;

 private:
  IDXGIOutput *GetDefaultOutputFor(UINT Adapter) {
    // TODO: Support more than one output.
    return adapter_outputs_.at(Adapter).at(0);
  }

  // CheckDeviceType/CheckDeviceFormat only need a device to ask
  // CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT) -- they don't need a
  // *fresh* one. D3D12CreateDevice is one of the most expensive calls in the
  // whole API (real driver/runtime initialization), and games/mods commonly
  // call these capability queries in a loop (once per format they care
  // about) -- profiled via WPA CPU sampling, this was the single largest
  // cost in this codebase's own code, dwarfing every actual per-frame
  // rendering function. Lazily create one device per adapter and reuse it
  // for every subsequent probe on that adapter.
  ID3D12Device *GetProbeDeviceFor(UINT Adapter);
  std::unordered_map<UINT, ComPtr<ID3D12Device>> cached_probe_devices_;

  ComPtr<IDXGIFactory2> dxgi_factory_;

  std::vector<IDXGIAdapter *> adapters_;
  std::vector<std::vector<IDXGIOutput *>> adapter_outputs_;

  // EnumAdapterModes cache, keyed by Adapter -- games/mods typically call
  // this in a loop (Mode = 0, 1, 2, ...) to enumerate every supported
  // resolution, e.g. to populate an options menu. GetDisplayModeList is a
  // genuinely slow, driver-level query; querying it fresh on every single
  // Mode index (as this used to do) turns an O(1)-per-call enumeration loop
  // into O(n) redundant full-list re-fetches from the driver.
  std::unordered_map<UINT, std::vector<DXGI_MODE_DESC>> cached_adapter_modes_;
};
}  // namespace Dx8to12