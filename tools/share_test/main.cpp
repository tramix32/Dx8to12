// x86 half of the cross-process shared-texture test. See CMakeLists.txt for
// why this exists as a standalone program rather than inside d3d8.dll.
//
// It creates one full-resolution Texture2D with D3D12_HEAP_FLAG_SHARED plus
// two shared fences, launches the x64 helper in --share-test mode, and then
// ping-pongs with it for as long as it is told to: x86 signals "input ready",
// x64 writes the texture and signals "output ready", x86 copies the result
// into a local texture the way a real composite pass would. That is the exact
// shape of the DLSS D1 round trip, with everything else stripped away.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// Representative of a DLSS output_color target rather than deliberately large:
// 2560x1440 is this machine's actual resolution, and R16G16B16A16_FLOAT is a
// format an upscaler would plausibly hand back (~28 MiB).
constexpr UINT kWidth = 2560;
constexpr UINT kHeight = 1440;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

template <typename T>
void Release(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

bool Check(HRESULT hr, const char* what) {
  if (SUCCEEDED(hr)) return true;
  std::printf("FAIL %s hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
  return false;
}

std::wstring UniqueName(const wchar_t* kind) {
  return std::wstring(L"Local\\Dx8to12ShareTest-") + kind + L"-" +
         std::to_wstring(GetCurrentProcessId());
}

// A device-removed reason is the failure this whole test is looking for, so it
// is checked explicitly rather than inferred from a later failing call.
bool DeviceAlive(ID3D12Device* device, const char* side) {
  const HRESULT reason = device->GetDeviceRemovedReason();
  if (reason == S_OK) return true;
  std::printf("DEVICE REMOVED (%s) reason=0x%08lX\n", side,
              static_cast<unsigned long>(reason));
  return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Unbuffered: this program's whole value is the last line it manages to
  // print before something goes wrong, and a full buffer loses exactly that.
  setvbuf(stdout, nullptr, _IONBF, 0);
  const int iterations = argc > 1 ? _wtoi(argv[1]) : 3000;
  std::printf("share_test: %ux%u, %d iterations\n", kWidth, kHeight,
              iterations);

  IDXGIFactory6* factory = nullptr;
  if (!Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2"))
    return 1;

  // Whichever adapter this x86 process would really render on; its LUID is
  // handed to the helper so both sides are provably on the same GPU.
  IDXGIAdapter1* adapter = nullptr;
  DXGI_ADAPTER_DESC1 adapter_desc{};
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    if (SUCCEEDED(adapter->GetDesc1(&adapter_desc)) &&
        !(adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
        SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                    __uuidof(ID3D12Device), nullptr))) {
      break;
    }
    Release(adapter);
  }
  if (!adapter) {
    std::printf("FAIL no hardware adapter\n");
    Release(factory);
    return 1;
  }
  std::wprintf(L"adapter: %s\n", adapter_desc.Description);

  ID3D12Device* device = nullptr;
  if (!Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(&device)),
             "D3D12CreateDevice"))
    return 1;

  ID3D12CommandQueue* queue = nullptr;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{.Type =
                                                D3D12_COMMAND_LIST_TYPE_DIRECT};
  if (!Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)),
             "CreateCommandQueue"))
    return 1;

  // The object under test.
  const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC tex_desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
      .Width = kWidth,
      .Height = kHeight,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .Format = kFormat,
      .SampleDesc = {.Count = 1},
      .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ID3D12Resource* shared_tex = nullptr;
  if (!Check(device->CreateCommittedResource(
                 &default_heap, D3D12_HEAP_FLAG_SHARED, &tex_desc,
                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                 IID_PPV_ARGS(&shared_tex)),
             "CreateCommittedResource(shared texture)"))
    return 1;

  // Stands in for whatever the game would composite the upscaled result into.
  D3D12_RESOURCE_DESC local_desc = tex_desc;
  local_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  ID3D12Resource* local_tex = nullptr;
  if (!Check(device->CreateCommittedResource(
                 &default_heap, D3D12_HEAP_FLAG_NONE, &local_desc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                 IID_PPV_ARGS(&local_tex)),
             "CreateCommittedResource(local texture)"))
    return 1;

  ID3D12Fence* ready_fence = nullptr;
  ID3D12Fence* done_fence = nullptr;
  if (!Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                 IID_PPV_ARGS(&ready_fence)),
             "CreateFence(ready)") ||
      !Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                 IID_PPV_ARGS(&done_fence)),
             "CreateFence(done)"))
    return 1;

  const std::wstring tex_name = UniqueName(L"tex");
  const std::wstring ready_name = UniqueName(L"ready");
  const std::wstring done_name = UniqueName(L"done");
  HANDLE tex_handle = nullptr, ready_handle = nullptr, done_handle = nullptr;
  if (!Check(device->CreateSharedHandle(shared_tex, nullptr, GENERIC_ALL,
                                        tex_name.c_str(), &tex_handle),
             "CreateSharedHandle(texture)") ||
      !Check(device->CreateSharedHandle(ready_fence, nullptr, GENERIC_ALL,
                                        ready_name.c_str(), &ready_handle),
             "CreateSharedHandle(ready)") ||
      !Check(device->CreateSharedHandle(done_fence, nullptr, GENERIC_ALL,
                                        done_name.c_str(), &done_handle),
             "CreateSharedHandle(done)"))
    return 1;
  std::printf("shared texture + fences created\n");

  wchar_t helper[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, helper, MAX_PATH);
  if (wchar_t* slash = wcsrchr(helper, L'\\')) *(slash + 1) = 0;
  std::wstring command = std::wstring(L"\"") + helper +
                         L"dx8to12_rt_helper.exe\" --share-test \"" + tex_name +
                         L"\" \"" + ready_name + L"\" \"" + done_name + L"\" " +
                         std::to_wstring(adapter_desc.AdapterLuid.LowPart) +
                         L" " +
                         std::to_wstring(adapter_desc.AdapterLuid.HighPart) +
                         L" " + std::to_wstring(iterations);
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION helper_proc{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &helper_proc)) {
    std::printf("FAIL CreateProcess helper (error %lu)\n", GetLastError());
    return 1;
  }
  std::printf("helper launched (pid %lu)\n", helper_proc.dwProcessId);

  // The helper needs its own DLLs beside it; if it fails to load them it exits
  // instantly and every later wait would block forever. Catch that here, while
  // the message can still say what actually happened.
  if (WaitForSingleObject(helper_proc.hProcess, 500) == WAIT_OBJECT_0) {
    DWORD early_exit = 0;
    GetExitCodeProcess(helper_proc.hProcess, &early_exit);
    std::printf("FAIL helper exited immediately, code=0x%08lX\n", early_exit);
    return 1;
  }

  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  if (!Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator)),
             "CreateCommandAllocator") ||
      !Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocator, nullptr,
                                       IID_PPV_ARGS(&list)),
             "CreateCommandList"))
    return 1;
  list->Close();

  HANDLE wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const auto started = std::chrono::steady_clock::now();
  bool ok = true;

  for (int i = 1; i <= iterations && ok; ++i) {
    const uint64_t value = static_cast<uint64_t>(i);
    // "Inputs are ready" -- in the real thing this is where the scene would
    // already have been rendered into the shared texture.
    if (!Check(queue->Signal(ready_fence, value), "Signal(ready)")) break;

    // Do not block the CPU on the helper: this is the GPU-side dependency the
    // handoff insists on, and a CPU wait here would hide exactly the kind of
    // stall a real frame would suffer.
    if (!Check(queue->Wait(done_fence, value), "Wait(done)")) break;

    if (!Check(allocator->Reset(), "allocator->Reset") ||
        !Check(list->Reset(allocator, nullptr), "list->Reset"))
      break;
    const D3D12_RESOURCE_BARRIER to_copy_src = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = shared_tex,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_COMMON,
                       .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE}};
    list->ResourceBarrier(1, &to_copy_src);
    const D3D12_TEXTURE_COPY_LOCATION dst{.pResource = local_tex,
                                          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                          .SubresourceIndex = 0};
    const D3D12_TEXTURE_COPY_LOCATION src{.pResource = shared_tex,
                                          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                          .SubresourceIndex = 0};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER to_common = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {.pResource = shared_tex,
                       .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                       .StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE,
                       .StateAfter = D3D12_RESOURCE_STATE_COMMON}};
    list->ResourceBarrier(1, &to_common);
    if (!Check(list->Close(), "list->Close")) break;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    // Keep the CPU from running unboundedly ahead of the helper, so a hang
    // surfaces as a timeout here rather than as an ever-growing queue of
    // pending work. done_fence is owned by the helper -- x86 only ever reads
    // it.
    if (done_fence->GetCompletedValue() < value) {
      if (!Check(done_fence->SetEventOnCompletion(value, wait_event),
                 "SetEventOnCompletion"))
        break;
      if (WaitForSingleObject(wait_event, 5000) != WAIT_OBJECT_0) {
        std::printf("FAIL timeout waiting for helper at iteration %d\n", i);
        ok = false;
        break;
      }
    }

    if (!DeviceAlive(device, "x86")) {
      ok = false;
      break;
    }
    if (i % 200 == 0) {
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        started)
              .count();
      std::printf("  %d/%d  %.1f it/s\n", i, iterations, i / seconds);
      std::fflush(stdout);
    }
  }

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();

  // Release any GPU-side Wait still queued on done_fence before tearing
  // anything down. If the helper died, that wait can never be satisfied by
  // the helper, and destroying a device whose queue is blocked on it leaves a
  // process that not even taskkill can end. A CPU-side fence signal satisfies
  // the wait; it is legal here because this process created the fence.
  if (done_fence) {
    done_fence->Signal(static_cast<uint64_t>(iterations) + 1);
  }

  DWORD helper_exit = 0;
  WaitForSingleObject(helper_proc.hProcess, 5000);
  GetExitCodeProcess(helper_proc.hProcess, &helper_exit);
  if (helper_exit == STILL_ACTIVE) TerminateProcess(helper_proc.hProcess, 1);

  std::printf("%s after %.1fs, helper exit=%lu\n",
              ok ? "SURVIVED" : "FAILED", seconds, helper_exit);

  CloseHandle(wait_event);
  CloseHandle(helper_proc.hProcess);
  CloseHandle(helper_proc.hThread);
  Release(list);
  Release(allocator);
  Release(done_fence);
  Release(ready_fence);
  Release(local_tex);
  Release(shared_tex);
  Release(queue);
  Release(device);
  Release(adapter);
  Release(factory);
  return ok ? 0 : 1;
}
