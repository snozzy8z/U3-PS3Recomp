/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU state to D3D12 rendering commands.
 *
 * Phase 1 implementation:
 *   - Win32 window + D3D12 device + swap chain
 *   - Clear render target to RSX clear color
 *   - Present with vsync
 *   - Basic vertex-colored triangle rendering
 *
 * This file is C with COM calls (D3D12 is a COM API).
 * We use the C interface (__uuidof not available in C, so we
 * define GUIDs manually).
 */

#ifdef _WIN32

#include "rsx_d3d12_backend.h"
#include "rsx_primitives.h"
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* D3D12 headers */
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

/* We need these GUIDs — define them here to avoid uuid.lib dependency */
#include <initguid.h>

/* Link libraries */
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         2   /* double buffering */
#define MAX_VERTICES      4096  /* per-frame vertex buffer */
#define MAX_DRAWS          256  /* per-frame draw records */
#define SHADER_CACHE_SIZE   64  /* max cached PSO entries */

typedef struct {
    u32 vb_byte_offset; /* offset into vb_mapped where this draw's verts live */
    u32 vertex_count;
    u32 topology;       /* D3D_PRIMITIVE_TOPOLOGY_* */
} D3D12DrawRecord;

/* Simple FNV-1a hash for shader cache keys */
static u32 fnv1a_hash(const u8* data, u32 len)
{
    u32 h = 0x811c9dc5u;
    for (u32 i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

typedef struct {
    u32  key;       /* hash of FP+VP microcode, 0 = empty slot */
    ID3D12PipelineState* pso_tri;
    ID3D12PipelineState* pso_line;
    ID3D12PipelineState* pso_point;
    ID3D12RootSignature* root_sig;
} ShaderCacheEntry;

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Window */
    HWND hwnd;
    u32  width;
    u32  height;
    int  window_closed;

    /* D3D12 core */
    ID3D12Device*              device;
    ID3D12CommandQueue*        cmd_queue;
    IDXGISwapChain3*           swap_chain;
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_descriptor_size;
    ID3D12Resource*            render_targets[FRAME_COUNT];
    ID3D12CommandAllocator*    cmd_allocators[FRAME_COUNT];
    ID3D12GraphicsCommandList* cmd_list;

    /* Synchronization */
    ID3D12Fence* fence;
    HANDLE       fence_event;
    u64          fence_values[FRAME_COUNT];
    u32          frame_index;

    /* Pipeline */
    ID3D12RootSignature*  root_signature;
    ID3D12PipelineState*  pipeline_state;         /* triangle class — default */
    ID3D12PipelineState*  pipeline_state_lines;   /* line class */
    ID3D12PipelineState*  pipeline_state_points;  /* point class */

    /* Depth/stencil */
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12Resource*       depth_buffer;

    /* Dynamic vertex buffer (upload heap) */
    ID3D12Resource*       vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vb_view;
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 if root sig + PSO created */

    /* Per-frame draw recording */
    int                   frame_recording; /* 1 if cmd list is open for recording */
    u32                   draw_count;      /* draws this frame */
    D3D12DrawRecord       draws[MAX_DRAWS];

    /* Pointer to current RSX state (set before draw calls) */
    const rsx_state*      current_rsx_state;

    /* Shader cache — maps FP+VP hash to compiled PSOs */
    ShaderCacheEntry      shader_cache[SHADER_CACHE_SIZE];
    ShaderCacheEntry*     active_shader;  /* currently bound cache entry (or NULL → fallback) */

    /* Extended root signature for decompiled shaders (CBV + SRV table) */
    ID3D12RootSignature*  extended_root_sig;

    /* Constant buffer for vertex program constants (512 vec4 = 32KB) */
    ID3D12Resource*       vp_const_buffer;
    void*                 vp_const_mapped;

    /* Current frame state */
    float clear_color[4];  /* RGBA float */

    /* Stats */
    u64 frame_count;
    u64 last_fps_time;
    u32 fps;

    int initialized;
} D3D12State;

static D3D12State s_d3d;

/* ---------------------------------------------------------------------------
 * Win32 window
 * -----------------------------------------------------------------------*/

static LRESULT CALLBACK d3d12_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        s_d3d.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            s_d3d.window_closed = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND create_window(u32 width, u32 height, const char* title)
{
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ps3recomp_d3d12";
    RegisterClassExA(&wc);

    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    return CreateWindowExA(
        0, "ps3recomp_d3d12",
        title ? title : "ps3recomp (D3D12)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);
}

/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

static int init_d3d12(u32 width, u32 height)
{
    HRESULT hr;

    /* Enable debug layer in debug builds */
#ifndef NDEBUG
    {
        ID3D12Debug* debug_controller = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller);
        if (SUCCEEDED(hr) && debug_controller) {
            debug_controller->lpVtbl->EnableDebugLayer(debug_controller);
            debug_controller->lpVtbl->Release(debug_controller);
            printf("[D3D12] Debug layer enabled\n");
        }
    }
#endif

    /* Create DXGI factory */
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", hr);
        return -1;
    }

    /* Create D3D12 device */
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void**)&s_d3d.device);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }
    printf("[D3D12] Device created (feature level 11.0)\n");

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s_d3d.device->lpVtbl->CreateCommandQueue(
        s_d3d.device, &queue_desc, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateCommandQueue failed\n");
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Create swap chain */
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = FRAME_COUNT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap_chain1 = NULL;
    hr = factory->lpVtbl->CreateSwapChainForHwnd(
        factory, (IUnknown*)s_d3d.cmd_queue,
        s_d3d.hwnd, &sc_desc, NULL, NULL, &swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateSwapChainForHwnd failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Disable Alt+Enter fullscreen toggle */
    factory->lpVtbl->MakeWindowAssociation(factory, s_d3d.hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->lpVtbl->Release(factory);

    /* Query SwapChain3 interface */
    hr = swap_chain1->lpVtbl->QueryInterface(
        swap_chain1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain);
    swap_chain1->lpVtbl->Release(swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: QueryInterface for SwapChain3 failed\n");
        return -1;
    }

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
        s_d3d.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap);
    if (FAILED(hr)) return -1;

    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* Create RTVs for each frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);

    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.swap_chain->lpVtbl->GetBuffer(
            s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]);
        if (FAILED(hr)) return -1;

        s_d3d.device->lpVtbl->CreateRenderTargetView(
            s_d3d.device, s_d3d.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += s_d3d.rtv_descriptor_size;
    }

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (DXGI_FORMAT_D24_UNORM_S8_UINT).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.dsv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] DSV heap creation failed\n");
            return -1;
        }

        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depth_desc = {0};
        depth_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.Width              = width;
        depth_desc.Height             = height;
        depth_desc.DepthOrArraySize   = 1;
        depth_desc.MipLevels          = 1;
        depth_desc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count   = 1;
        depth_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depth_clear = {0};
        depth_clear.Format                       = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_clear.DepthStencil.Depth           = 1.0f;
        depth_clear.DepthStencil.Stencil         = 0;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
            &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer);
        if (FAILED(hr)) {
            printf("[D3D12] Depth buffer creation failed (0x%08lX)\n", hr);
            return -1;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
        dsv_desc.Format         = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);
        s_d3d.device->lpVtbl->CreateDepthStencilView(
            s_d3d.device, s_d3d.depth_buffer, &dsv_desc, dsv_handle);

        printf("[D3D12] Depth buffer created (%ux%u D24S8)\n", width, height);
    }

    /* Create command allocators and command list */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.device->lpVtbl->CreateCommandAllocator(
            s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&s_d3d.cmd_allocators[i]);
        if (FAILED(hr)) return -1;
    }

    hr = s_d3d.device->lpVtbl->CreateCommandList(
        s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_d3d.cmd_allocators[0], NULL,
        &IID_ID3D12GraphicsCommandList, (void**)&s_d3d.cmd_list);
    if (FAILED(hr)) return -1;

    /* Close the command list (it starts in recording state) */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);

    /* Create fence */
    hr = s_d3d.device->lpVtbl->CreateFence(
        s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&s_d3d.fence);
    if (FAILED(hr)) return -1;

    s_d3d.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    memset(s_d3d.fence_values, 0, sizeof(s_d3d.fence_values));

    /* ---------------------------------------------------------------
     * Create root signature with 16 root constants (one mat4 MVP at b0).
     * Visible only to the vertex shader — pixel shader doesn't need it.
     * ---------------------------------------------------------------*/
    {
        D3D12_ROOT_PARAMETER root_params[1] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters = 1;
        rs_desc.pParameters   = root_params;
        rs_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* signature_blob = NULL;
        ID3DBlob* error_blob = NULL;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &error_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature serialization failed: %s\n",
                   error_blob ? (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob) : "?");
            if (error_blob) error_blob->lpVtbl->Release(error_blob);
            return -1;
        }

        hr = s_d3d.device->lpVtbl->CreateRootSignature(
            s_d3d.device, 0,
            signature_blob->lpVtbl->GetBufferPointer(signature_blob),
            signature_blob->lpVtbl->GetBufferSize(signature_blob),
            &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature);
        signature_blob->lpVtbl->Release(signature_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature creation failed\n");
            return -1;
        }
    }

    /* ---------------------------------------------------------------
     * Compile shaders and create PSO
     * ---------------------------------------------------------------*/
    {
        /* Basic vertex-colored shader.
         * The MVP matrix arrives via root constants as 4 vec4 columns
         * (PS3/OpenGL column-major convention). We multiply explicitly so
         * we don't depend on HLSL's matrix packing — matches PS3 semantics
         * `gl_Position = MVP * vec4(pos, 1.0)`. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0;\n"
            "    float4 mvp_col1;\n"
            "    float4 mvp_col2;\n"
            "    float4 mvp_col3;\n"
            "};\n"
            "struct VSInput  { float3 pos : POSITION; float4 col : COLOR; };\n"
            "struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "VSOutput main(VSInput i) {\n"
            "    VSOutput o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col = i.col;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col; }\n";

        ID3DBlob* vs_blob = NULL;
        ID3DBlob* ps_blob = NULL;
        ID3DBlob* err = NULL;

        hr = D3DCompile(vs_hlsl, sizeof(vs_hlsl) - 1, "vs_basic", NULL, NULL,
                        "main", "vs_5_0", 0, 0, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        hr = D3DCompile(ps_hlsl, sizeof(ps_hlsl) - 1, "ps_basic", NULL, NULL,
                        "main", "ps_5_0", 0, 0, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] PS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        if (vs_blob && ps_blob) {
            D3D12_INPUT_ELEMENT_DESC input_layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = input_layout;
            pso_desc.InputLayout.NumElements = 2;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = UINT_MAX;
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            /* Depth test enabled, write enabled, LESS func.
             * Games with depth_test_enable=0 in RSX state can still render —
             * LESS just means new-z must be less than existing — but future
             * work should mirror RSX depth state into a PSO cache. */
            pso_desc.DepthStencilState.DepthEnable    = TRUE;
            pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pso_desc.DepthStencilState.StencilEnable  = FALSE;
            pso_desc.SampleDesc.Count = 1;

            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state);
            if (SUCCEEDED(hr)) {
                s_d3d.pipeline_ready = 1;
                printf("[D3D12] Pipeline state created (triangle class)\n");
            } else {
                printf("[D3D12] PSO TRIANGLE creation failed (0x%08lX)\n", hr);
            }

            /* Line-class PSO — same shader, LINE topology type. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_lines);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (line class)\n");
            else printf("[D3D12] PSO LINE creation failed (0x%08lX)\n", hr);

            /* Point-class PSO. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_points);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (point class)\n");
            else printf("[D3D12] PSO POINT creation failed (0x%08lX)\n", hr);

            vs_blob->lpVtbl->Release(vs_blob);
            ps_blob->lpVtbl->Release(ps_blob);
        }
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc = {0};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = MAX_VERTICES * 28; /* 28 bytes per vertex (pos3+col4) */
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vertex_buffer);
        if (SUCCEEDED(hr)) {
            D3D12_RANGE read_range = {0, 0};
            s_d3d.vertex_buffer->lpVtbl->Map(
                s_d3d.vertex_buffer, 0, &read_range, &s_d3d.vb_mapped);
            s_d3d.vb_view.BufferLocation =
                s_d3d.vertex_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vertex_buffer);
            s_d3d.vb_view.StrideInBytes = 28;
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * 28;
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (MAX_VERTICES * 28) / 1024);
        }
    }

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

static void wait_for_gpu(void)
{
    u32 fi = s_d3d.frame_index;
    s_d3d.fence_values[fi]++;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, s_d3d.fence_values[fi]);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[fi]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[fi], s_d3d.fence_event);
        WaitForSingleObject(s_d3d.fence_event, INFINITE);
    }
}

static void move_to_next_frame(void)
{
    u64 current_fence = s_d3d.fence_values[s_d3d.frame_index];
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, current_fence);

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[s_d3d.frame_index]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[s_d3d.frame_index], s_d3d.fence_event);
        WaitForSingleObject(s_d3d.fence_event, INFINITE);
    }

    s_d3d.fence_values[s_d3d.frame_index] = current_fence + 1;
}

/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

static void render_frame(void)
{
    u32 fi = s_d3d.frame_index;

    /* Reset command allocator and list */
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);

    /* Transition render target to RENDER_TARGET state */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s_d3d.render_targets[fi];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Get RTV handle for current frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);
    rtv_handle.ptr += fi * s_d3d.rtv_descriptor_size;

    /* Get DSV handle (single depth buffer shared across frames) */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);

    /* Set render target + depth */
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);

    /* Clear color and depth */
    s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
        s_d3d.cmd_list, rtv_handle, s_d3d.clear_color, 0, NULL);
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(
        s_d3d.cmd_list, dsv_handle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);

    /* Set viewport and scissor */
    D3D12_VIEWPORT viewport = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, (LONG)s_d3d.width, (LONG)s_d3d.height};
    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &viewport);
    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &scissor);

    /* Bind pipeline state and push MVP if anything to draw */
    if (s_d3d.pipeline_ready && s_d3d.draw_count > 0) {
        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.root_signature);
        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &s_d3d.vb_view);

        /* Push the MVP matrix from RSX vertex constants slots 0..3.
         * If the game hasn't written any constants (e.g. placeholder data
         * already in clip space), fall back to identity. */
        float mvp[16];
        const rsx_state* st = s_d3d.current_rsx_state;
        int have_mvp = 0;
        if (st) {
            for (u32 r = 0; r < 4; r++) {
                for (u32 c = 0; c < 4; c++) {
                    float v = st->vertex_constants[r][c];
                    mvp[r * 4 + c] = v;
                    if (v != 0.0f) have_mvp = 1;
                }
            }
        }
        if (!have_mvp) {
            memset(mvp, 0, sizeof(mvp));
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f; /* identity */
        }
        s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstants(
            s_d3d.cmd_list, 0 /*root param 0*/, 16, mvp, 0);

        /* Replay each recorded draw with its own primitive topology and
         * the matching PSO class (triangle / line / point). If we have a
         * decompiled shader PSO, use it; otherwise fall back to the basic
         * vertex-colored PSO. */
        u32 last_topo = 0xFFFFFFFFu;
        ID3D12PipelineState* last_pso = NULL;
        u32 draws = s_d3d.draw_count;
        if (draws > MAX_DRAWS) draws = MAX_DRAWS;
        for (u32 d = 0; d < draws; d++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[d];

            /* Select PSO based on topology class, preferring active shader */
            ID3D12PipelineState* target_pso = NULL;
            ShaderCacheEntry* sc = s_d3d.active_shader;
            if (dr->topology == D3D_TOPOLOGY_POINTLIST) {
                target_pso = (sc && sc->pso_point) ? sc->pso_point
                             : (s_d3d.pipeline_state_points ? s_d3d.pipeline_state_points
                                                            : s_d3d.pipeline_state);
            } else if (dr->topology == D3D_TOPOLOGY_LINELIST ||
                       dr->topology == D3D_TOPOLOGY_LINESTRIP) {
                target_pso = (sc && sc->pso_line) ? sc->pso_line
                             : (s_d3d.pipeline_state_lines ? s_d3d.pipeline_state_lines
                                                           : s_d3d.pipeline_state);
            } else {
                target_pso = (sc && sc->pso_tri) ? sc->pso_tri : s_d3d.pipeline_state;
            }
            if (target_pso != last_pso) {
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, target_pso);
                last_pso = target_pso;
            }
            if (dr->topology != last_topo) {
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, dr->topology);
                last_topo = dr->topology;
            }
            u32 start_vert = dr->vb_byte_offset / 28; /* 28 bytes per BasicVertex */
            s_d3d.cmd_list->lpVtbl->DrawInstanced(
                s_d3d.cmd_list, dr->vertex_count, 1, start_vert, 0);
        }
    }
    s_d3d.vb_offset  = 0; /* reset for next frame */
    s_d3d.draw_count = 0;

    /* Transition render target to PRESENT state */
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Close and execute */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);

    /* Present */
    s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, 1, 0); /* vsync */

    move_to_next_frame();

    s_d3d.frame_count++;
}

/* ---------------------------------------------------------------------------
 * RSX backend callbacks
 * -----------------------------------------------------------------------*/

static int d3d12_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[D3D12] Backend init(%ux%u)\n", width, height);
    return 0;
}

static void d3d12_shutdown(void* ud)
{
    (void)ud;
    printf("[D3D12] Backend shutdown\n");
}

static void d3d12_begin_frame(void* ud)
{
    (void)ud;
}

static void d3d12_end_frame(void* ud)
{
    (void)ud;
}

static void d3d12_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    if (s_d3d.initialized)
        render_frame();

    /* FPS tracking */
    ULONGLONG now = GetTickCount64();
    if (now - s_d3d.last_fps_time >= 1000) {
        s_d3d.fps = (u32)s_d3d.frame_count; /* rough estimate */
        s_d3d.last_fps_time = now;
        s_d3d.frame_count = 0;
    }
}

static void d3d12_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)flags;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB u32 to float[4] RGBA */
    s_d3d.clear_color[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    s_d3d.clear_color[1] = ((color >> 8) & 0xFF) / 255.0f;  /* G */
    s_d3d.clear_color[2] = (color & 0xFF) / 255.0f;          /* B */
    s_d3d.clear_color[3] = ((color >> 24) & 0xFF) / 255.0f;  /* A */
}

static void d3d12_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;
    /* Log only the first few; set_render_target is called every frame and
     * floods the log otherwise. */
    static int s_count = 0;
    if (s_count < 5) {
        printf("[D3D12] set_render_target(%ux%u)\n",
               state->surface_clip_w, state->surface_clip_h);
        s_count++;
    }
}

static void d3d12_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: update D3D12 viewport from RSX state */
    (void)state;
}

/* Helper: read RSX vertex data from guest memory and convert to our BasicVertex format.
 * Reads position (float3) from attrib 0 and color (float4) from attrib 3.
 * If attribs aren't available, generates placeholder geometry.
 * Returns the actual number of vertices uploaded (may be less than `count`
 * if the per-frame buffer is full). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    extern uint8_t* vm_base;
    typedef struct { float x, y, z; float r, g, b, a; } BasicVertex;
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * 28 - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (count > max_verts) count = max_verts;

    const rsx_state* state = s_d3d.current_rsx_state;
    int has_position = 0;
    int has_color = 0;

    /* Check if position attrib (typically attrib 0) is enabled and is float */
    if (state) {
        const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
        if (pos->enabled && pos->type == 2 /* float */ && pos->size >= 3) {
            has_position = 1;
        }
        /* Color is typically attrib 3 (diffuse) */
        const rsx_vertex_attrib* col = &state->vertex_attribs[3];
        if (col->enabled && col->size >= 3) {
            has_color = 1;
        }
    }

    for (u32 i = 0; i < count; i++) {
        if (has_position && vm_base) {
            /* Read position from guest memory.
             * RSX stores vertices in big-endian. For float type, each component
             * is a 32-bit BE float. We need to byte-swap. */
            const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
            u32 addr = pos->offset + (first + i) * pos->stride;
            if (addr < 0x20000000) { /* sanity check */
                u8* src = vm_base + addr;
                /* Byte-swap each float component (BE → LE) */
                u32 fx, fy, fz;
                memcpy(&fx, src,     4); fx = ((fx>>24)&0xFF)|((fx>>8)&0xFF00)|((fx<<8)&0xFF0000)|((fx<<24)&0xFF000000);
                memcpy(&fy, src + 4, 4); fy = ((fy>>24)&0xFF)|((fy>>8)&0xFF00)|((fy<<8)&0xFF0000)|((fy<<24)&0xFF000000);
                memcpy(&fz, src + 8, 4); fz = ((fz>>24)&0xFF)|((fz>>8)&0xFF00)|((fz<<8)&0xFF0000)|((fz<<24)&0xFF000000);
                memcpy(&verts[i].x, &fx, 4);
                memcpy(&verts[i].y, &fy, 4);
                memcpy(&verts[i].z, &fz, 4);
            } else {
                verts[i].x = verts[i].y = verts[i].z = 0.0f;
            }
        } else {
            /* Placeholder: distribute vertices in a circle */
            float t = (float)(first + i) / 100.0f;
            verts[i].x = sinf(t * 6.28f) * 0.5f;
            verts[i].y = cosf(t * 6.28f) * 0.5f;
            verts[i].z = 0.0f;
        }

        if (has_color && vm_base) {
            const rsx_vertex_attrib* col = &state->vertex_attribs[3];
            u32 addr = col->offset + (first + i) * col->stride;
            if (addr < 0x20000000 && col->type == 4 /* ubyte */) {
                u8* src = vm_base + addr;
                /* RGBA bytes, normalized to 0-1 */
                verts[i].r = src[0] / 255.0f;
                verts[i].g = src[1] / 255.0f;
                verts[i].b = src[2] / 255.0f;
                verts[i].a = (col->size >= 4) ? src[3] / 255.0f : 1.0f;
            } else if (addr < 0x20000000 && col->type == 2 /* float */) {
                u8* src = vm_base + addr;
                u32 fr, fg, fb, fa;
                memcpy(&fr, src,     4); fr = ((fr>>24)&0xFF)|((fr>>8)&0xFF00)|((fr<<8)&0xFF0000)|((fr<<24)&0xFF000000);
                memcpy(&fg, src + 4, 4); fg = ((fg>>24)&0xFF)|((fg>>8)&0xFF00)|((fg<<8)&0xFF0000)|((fg<<24)&0xFF000000);
                memcpy(&fb, src + 8, 4); fb = ((fb>>24)&0xFF)|((fb>>8)&0xFF00)|((fb<<8)&0xFF0000)|((fb<<24)&0xFF000000);
                memcpy(&verts[i].r, &fr, 4);
                memcpy(&verts[i].g, &fg, 4);
                memcpy(&verts[i].b, &fb, 4);
                if (col->size >= 4) {
                    memcpy(&fa, src + 12, 4); fa = ((fa>>24)&0xFF)|((fa>>8)&0xFF00)|((fa<<8)&0xFF0000)|((fa<<24)&0xFF000000);
                    memcpy(&verts[i].a, &fa, 4);
                } else {
                    verts[i].a = 1.0f;
                }
            } else {
                verts[i].r = 1.0f; verts[i].g = 1.0f; verts[i].b = 1.0f; verts[i].a = 1.0f;
            }
        } else {
            /* Default white */
            verts[i].r = 1.0f; verts[i].g = 1.0f; verts[i].b = 1.0f; verts[i].a = 1.0f;
        }
    }
    s_d3d.vb_offset += count * sizeof(BasicVertex);
    return count;
}

static void d3d12_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    /* Log the first 20 calls in detail, then every 1000th to show liveness
     * without flooding. */
    static u64 s_total = 0;
    if (s_total < 20 || (s_total % 1000) == 0) {
        printf("[D3D12] draw_arrays #%llu prim=%u first=%u count=%u\n",
               (unsigned long long)s_total, primitive, first, count);
    }
    s_total++;

    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped) return;
    if (count == 0 || count > MAX_VERTICES) return;

    /* One-shot: dump the MVP and first vertex on the very first draw so we
     * can see what coordinate space the game is sending positions in. */
    static int s_dumped = 0;
    if (!s_dumped && s_d3d.current_rsx_state) {
        extern uint8_t* vm_base;
        s_dumped = 1;
        const rsx_state* st = s_d3d.current_rsx_state;
        printf("[D3D12-DUMP] vertex_constants slots 0..7:\n");
        for (u32 i = 0; i < 8; i++) {
            printf("  [%u] = (% .4f, % .4f, % .4f, % .4f)\n", i,
                   st->vertex_constants[i][0], st->vertex_constants[i][1],
                   st->vertex_constants[i][2], st->vertex_constants[i][3]);
        }
        printf("[D3D12-DUMP] vc dirty=%d range=[%u..%u]\n",
               st->vertex_constants_dirty,
               st->vertex_constants_lo, st->vertex_constants_hi);
        printf("[D3D12-DUMP] viewport=%ux%u clip=%ux%u\n",
               st->viewport_w, st->viewport_h,
               st->surface_clip_w, st->surface_clip_h);
        const rsx_vertex_attrib* pos = &st->vertex_attribs[0];
        printf("[D3D12-DUMP] attrib0: enabled=%d type=%u size=%u stride=%u offset=0x%08X\n",
               pos->enabled, pos->type, pos->size, pos->stride, pos->offset);
        if (pos->enabled && pos->type == 2 && vm_base) {
            for (u32 v = 0; v < (count < 4 ? count : 4); v++) {
                u32 addr = pos->offset + (first + v) * pos->stride;
                if (addr >= 0x20000000) break;
                u8* src = vm_base + addr;
                u32 fx, fy, fz;
                memcpy(&fx, src,     4); fx = ((fx>>24)&0xFF)|((fx>>8)&0xFF00)|((fx<<8)&0xFF0000)|((fx<<24)&0xFF000000);
                memcpy(&fy, src + 4, 4); fy = ((fy>>24)&0xFF)|((fy>>8)&0xFF00)|((fy<<8)&0xFF0000)|((fy<<24)&0xFF000000);
                memcpy(&fz, src + 8, 4); fz = ((fz>>24)&0xFF)|((fz>>8)&0xFF00)|((fz<<8)&0xFF0000)|((fz<<24)&0xFF000000);
                float x, y, z;
                memcpy(&x, &fx, 4); memcpy(&y, &fy, 4); memcpy(&z, &fz, 4);
                printf("[D3D12-DUMP] v[%u] pos=(% .4f, % .4f, % .4f)\n", v, x, y, z);
            }
        }
    }

    u32 topo = rsx_to_d3d12_topology(primitive);
    if (topo == D3D_TOPOLOGY_UNDEFINED) {
        /* Skip primitives that still need index-buffer conversion
         * (quads, line loops, triangle fans) rather than silently
         * rendering them as the wrong shape. */
        static int s_skipped_nontri = 0;
        if (s_skipped_nontri < 3) {
            printf("[D3D12] draw_arrays: skipping prim=%u (needs index conversion)\n",
                   primitive);
            s_skipped_nontri++;
        }
        return;
    }

    u32 record_offset = s_d3d.vb_offset;
    u32 actual_count  = upload_vertices_from_rsx(first, count);
    if (actual_count == 0) return;

    if (s_d3d.draw_count < MAX_DRAWS) {
        s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
        s_d3d.draws[s_d3d.draw_count].vertex_count   = actual_count;
        s_d3d.draws[s_d3d.draw_count].topology       = topo;
        s_d3d.draw_count++;
    }
}

static void d3d12_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 20) {
        printf("[D3D12] draw_indexed(prim=%u, offset=%u, count=%u)\n",
               primitive, offset, count);
        log_count++;
    }
    /* TODO: create index buffer and call DrawIndexedInstanced */
}

/* Immediate-mode draw: vertices already decoded into state->inline_verts as
 * pos.xyzw + col.rgba (clip space). Quads (prim 8) are expanded to a triangle
 * list since D3D12 has no quad topology. */
static void d3d12_draw_inline(void* ud, u32 primitive, const rsx_state* state)
{
    (void)ud;
    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped || !state) return;
    u32 vc = state->inline_vert_count;
    if (vc == 0) return;

    typedef struct { float x, y, z; float r, g, b, a; } BasicVertex;
    BasicVertex* dst = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * 28 - s_d3d.vb_offset) / (u32)sizeof(BasicVertex);
    u32 out = 0;

    static u64 s_total = 0;
    if (s_total < 20) {
        printf("[D3D12] draw_inline #%llu prim=%u verts=%u pos0=(%.3f,%.3f) col0=(%.2f,%.2f,%.2f,%.2f)\n",
               (unsigned long long)s_total, primitive, vc,
               state->inline_verts[0][0], state->inline_verts[0][1],
               state->inline_verts[0][4], state->inline_verts[0][5],
               state->inline_verts[0][6], state->inline_verts[0][7]);
    }
    s_total++;

    /* Debug visualization: derive a bright color from the texcoord (sv[4],sv[5])
     * so the menu's quad layout is VISIBLE before real texture sampling exists.
     * UC3_RSX_RAWCOL uses the raw vertex color instead. */
    int rawcol = (getenv("UC3_RSX_RAWCOL") != NULL);
    #define EMIT_V(idx) do { if (out < max_verts) {                       \
        const float* sv = state->inline_verts[idx];                       \
        dst[out].x = sv[0]; dst[out].y = sv[1]; dst[out].z = sv[2];        \
        if (rawcol) { dst[out].r = sv[6]; dst[out].g = sv[6];             \
                      dst[out].b = sv[6]; dst[out].a = sv[7]; }           \
        else { dst[out].r = 0.30f + 0.70f * sv[4];                        \
               dst[out].g = 0.30f + 0.70f * sv[5];                        \
               dst[out].b = 0.60f; dst[out].a = 1.0f; }                   \
        out++; } } while (0)

    if (primitive == 8) {                 /* QUADS -> triangle list (0,1,2, 0,2,3) */
        for (u32 q = 0; q + 4 <= vc; q += 4) {
            EMIT_V(q+0); EMIT_V(q+1); EMIT_V(q+2);
            EMIT_V(q+0); EMIT_V(q+2); EMIT_V(q+3);
        }
    } else {
        for (u32 i = 0; i < vc; i++) EMIT_V(i);
    }
    #undef EMIT_V
    if (out == 0) return;

    u32 topo = (primitive == 8) ? rsx_to_d3d12_topology(5) /* triangles */
                                : rsx_to_d3d12_topology(primitive);
    if (topo == D3D_TOPOLOGY_UNDEFINED) topo = rsx_to_d3d12_topology(5);

    if (s_d3d.draw_count < MAX_DRAWS) {
        s_d3d.draws[s_d3d.draw_count].vb_byte_offset = s_d3d.vb_offset;
        s_d3d.draws[s_d3d.draw_count].vertex_count   = out;
        s_d3d.draws[s_d3d.draw_count].topology       = topo;
        s_d3d.draw_count++;
    }
    s_d3d.vb_offset += out * (u32)sizeof(BasicVertex);
}

static void d3d12_bind_texture(void* ud, u32 unit, const rsx_texture_state* tex)
{
    (void)ud;
    extern uint8_t* vm_base;

    u32 width  = (tex->image_rect >> 16) & 0xFFFF;
    u32 height = tex->image_rect & 0xFFFF;
    u32 format = (tex->format >> 8) & 0xFF;
    u32 offset = tex->offset;
    u32 loc = tex->format & 0x3;   /* 1=LOCAL (RSX VRAM @0xC0000000), 2=MAIN */
    u32 tex_addr = (loc == 1 ? 0xC0000000u : 0u) + offset;

    static int log_count = 0;
    if (log_count < 80) {
        const u8* p = vm_base + tex_addr;
        uint32_t nz = p[0]|p[1]|p[2]|p[3]|p[4]|p[5]|p[6]|p[7];
        printf("[D3D12] bind_texture(unit=%u, offset=0x%X, fmt=0x%02X, %ux%u) loc=%u "
               "raw=%02X%02X%02X%02X%02X%02X%02X%02X %s\n",
               unit, offset, format, width, height, loc,
               p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
               nz ? "<-- NON-ZERO (populated!)" : "");
        log_count++;
    }

    /* De-risk: dump the first DXT1 texture to a BMP so addressing+format can be
     * visually verified before wiring the GPU upload. UC3_DUMP_TEX=path. */
    {
        const char* dump = getenv("UC3_DUMP_TEX");
        static int dumped = 0;
        u32 base_fmt = format & 0x9F;       /* strip NR/UN flags */
        if (dump && !dumped && base_fmt == 0x86 /* DXT1 */ &&
            width >= 4 && height >= 4 && width <= 8192 && height <= 8192 && vm_base) {
            dumped = 1;
            int swap = (getenv("UC3_TEX_SWAP") != NULL);
            const u8* src = vm_base + tex_addr;   /* VRAM @0xC0000000 + offset for loc=1 */
            u32 W = width, H = height;
            u8* rgb = (u8*)malloc((size_t)W * H * 3);
            #define RD16(p) (swap ? (u16)(((p)[0]<<8)|(p)[1]) : (u16)((p)[0]|((p)[1]<<8)))
            for (u32 by = 0; by < H; by += 4) {
              for (u32 bx = 0; bx < W; bx += 4) {
                const u8* b = src + ((size_t)(by/4)*(W/4) + bx/4) * 8;
                u16 c0 = RD16(b), c1 = RD16(b+2);
                u8 col[4][3];
                #define EXP(c,o) do { col[o][0]=(u8)(((c>>11)&0x1F)*255/31); \
                    col[o][1]=(u8)(((c>>5)&0x3F)*255/63); col[o][2]=(u8)((c&0x1F)*255/31);} while(0)
                EXP(c0,0); EXP(c1,1);
                for (int k=0;k<3;k++){
                  if (c0>c1){ col[2][k]=(u8)((2*col[0][k]+col[1][k])/3); col[3][k]=(u8)((col[0][k]+2*col[1][k])/3);}
                  else { col[2][k]=(u8)((col[0][k]+col[1][k])/2); col[3][k]=0; }
                }
                u32 idx = b[4]|(b[5]<<8)|(b[6]<<16)|((u32)b[7]<<24);
                for (u32 ty=0; ty<4; ty++) for (u32 tx=0; tx<4; tx++){
                  u32 px=bx+tx, py=by+ty; if(px>=W||py>=H) continue;
                  int ci=(idx>>(2*(ty*4+tx)))&3;
                  u8* d=rgb+((size_t)py*W+px)*3;
                  d[0]=col[ci][0]; d[1]=col[ci][1]; d[2]=col[ci][2];
                }
              }
            }
            #undef RD16
            #undef EXP
            FILE* f = fopen(dump, "wb");
            if (f) {
                u32 rowsz = (W*3+3)&~3u, imgsz = rowsz*H, filesz = 54+imgsz;
                u8 hdr[54]={0}; hdr[0]='B';hdr[1]='M';
                *(u32*)(hdr+2)=filesz; *(u32*)(hdr+10)=54; *(u32*)(hdr+14)=40;
                *(u32*)(hdr+18)=W; *(int*)(hdr+22)=-(int)H; /* top-down */
                *(u16*)(hdr+26)=1; *(u16*)(hdr+28)=24; *(u32*)(hdr+34)=imgsz;
                fwrite(hdr,1,54,f);
                u8 pad[3]={0};
                for (u32 y=0;y<H;y++){
                    for (u32 x=0;x<W;x++){ u8* d=rgb+((size_t)y*W+x)*3;
                        u8 bgr[3]={d[2],d[1],d[0]}; fwrite(bgr,1,3,f); }
                    fwrite(pad,1,rowsz-W*3,f);
                }
                fclose(f);
                printf("[D3D12] dumped DXT1 texture %ux%u to %s (swap=%d)\n",W,H,dump,swap);
            }
            free(rgb);
        }
    }

    /* Texture upload is complex and requires:
     * 1. Matching DXGI format from RSX format
     * 2. Creating a texture resource in DEFAULT heap
     * 3. Creating an upload buffer in UPLOAD heap
     * 4. Copying pixel data from guest memory (with potential deswizzle)
     * 5. Recording CopyTextureRegion into the command list
     * 6. Creating a SRV descriptor
     * 7. Binding to the shader
     *
     * For now we log the texture parameters. The infrastructure is ready:
     * - rsx_texture_formats.h provides format mapping
     * - rsx_to_dxgi_texture_format() converts RSX → DXGI
     * - Guest memory is accessible via vm_base + offset
     *
     * Full implementation requires:
     * - A texture cache (avoid re-uploading unchanged textures)
     * - A SRV descriptor heap (CBV_SRV_UAV type)
     * - Root signature update to include SRV table
     * - A textured PSO (with sampler state)
     */

    /* Validate that we CAN read this texture */
    if (!vm_base || width == 0 || height == 0) return;
    if (offset >= 0x20000000) return; /* out of range */

    /* Log format details for debugging */
    static int detail_log = 0;
    if (detail_log < 5) {
        const char* fmt_name = "unknown";
        switch (format) {
        case 0x85: fmt_name = "A8R8G8B8"; break;
        case 0x84: fmt_name = "R5G6B5"; break;
        case 0x86: fmt_name = "DXT1"; break;
        case 0x88: fmt_name = "DXT5"; break;
        case 0x81: fmt_name = "B8"; break;
        case 0x9E: fmt_name = "D8R8G8B8"; break;
        }
        printf("[D3D12]   texture: %s (%ux%u) at guest 0x%08X\n",
               fmt_name, width, height, offset);
        detail_log++;
    }
}

static void d3d12_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    /* Log enabled vertex attributes for debugging */
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_vertex_attribs:\n");
        for (int i = 0; i < 16; i++) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[i];
            if (a->enabled) {
                const char* type_name = "?";
                switch (a->type) {
                case 1: type_name = "snorm16"; break;
                case 2: type_name = "float32"; break;
                case 3: type_name = "float16"; break;
                case 4: type_name = "ubyte"; break;
                case 5: type_name = "s16"; break;
                case 7: type_name = "ubyte256"; break;
                }
                printf("  attrib[%d]: %s x%u, stride=%u, offset=0x%X\n",
                       i, type_name, a->size, a->stride, a->offset);
            }
        }
        log_count++;
    }
}

/* Build a PSO from decompiled FP+VP HLSL strings. Returns NULL on failure. */
static ID3D12PipelineState* compile_pso(const char* vs_hlsl, const char* ps_hlsl,
                                         ID3D12RootSignature* root_sig,
                                         D3D12_PRIMITIVE_TOPOLOGY_TYPE topo_type)
{
    ID3DBlob* vs_blob = NULL;
    ID3DBlob* ps_blob = NULL;
    ID3DBlob* err = NULL;
    HRESULT hr;

    hr = D3DCompile(vs_hlsl, strlen(vs_hlsl), "vp_decompiled", NULL, NULL,
                    "main", "vs_5_0", 0, 0, &vs_blob, &err);
    if (FAILED(hr)) {
        printf("[D3D12] Decompiled VS compile failed: %s\n",
               err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        return NULL;
    }

    hr = D3DCompile(ps_hlsl, strlen(ps_hlsl), "fp_decompiled", NULL, NULL,
                    "main", "ps_5_0", 0, 0, &ps_blob, &err);
    if (FAILED(hr)) {
        printf("[D3D12] Decompiled PS compile failed: %s\n",
               err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        if (vs_blob) vs_blob->lpVtbl->Release(vs_blob);
        return NULL;
    }

    /* Input layout for the decompiled VP: POSITION float3 + COLOR0 float4.
     * This matches the current vertex upload path. Future: widen to all 16 RSX attribs. */
    D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
    pso_desc.pRootSignature = root_sig;
    pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
    pso_desc.VS.BytecodeLength  = vs_blob->lpVtbl->GetBufferSize(vs_blob);
    pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
    pso_desc.PS.BytecodeLength  = ps_blob->lpVtbl->GetBufferSize(ps_blob);
    pso_desc.InputLayout.pInputElementDescs = input_layout;
    pso_desc.InputLayout.NumElements = 2;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = topo_type;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso_desc.DepthStencilState.DepthEnable    = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso_desc.DepthStencilState.StencilEnable  = FALSE;
    pso_desc.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = NULL;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
        s_d3d.device, &pso_desc, &IID_ID3D12PipelineState, (void**)&pso);

    vs_blob->lpVtbl->Release(vs_blob);
    ps_blob->lpVtbl->Release(ps_blob);

    if (FAILED(hr)) {
        printf("[D3D12] Decompiled PSO creation failed (0x%08lX)\n", hr);
        return NULL;
    }
    return pso;
}

static void d3d12_set_shader(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    extern uint8_t* vm_base;
    if (!vm_base || !s_d3d.device) return;

    /* Build a cache key from FP address + VP instruction count + first VP word.
     * Fragment programs live in RSX local memory (VRAM @0xC0000000) — apply the
     * location bits (state->shader_program & 3, 1=LOCAL) like textures do, else
     * vm_base+offset reads the wrong memory and the size probe returns 0. */
    u32 fp_loc  = state->shader_program & 0x3u;
    u32 fp_off  = state->fragment_program_addr;
    u32 fp_addr = (fp_loc == 1) ? (0xC0000000u + fp_off) : fp_off;
    u32 vp_count = state->vp_instruction_count;

    /* Hash FP microcode */
    u32 fp_size = 0;
    u32 hash = 0;
    if (fp_off && fp_off < 0x10000000) {
        fp_size = rsx_fp_program_size(vm_base + fp_addr, 0x10000);
        if (fp_size > 0) {
            hash = fnv1a_hash(vm_base + fp_addr, fp_size);
        }
    }
    /* Mix in VP data */
    if (vp_count > 0) {
        u32 vp_hash = fnv1a_hash((const u8*)state->vp_instructions,
                                  vp_count * 16);
        hash ^= vp_hash * 0x9e3779b9u;
    }
    if (hash == 0) hash = 1;

    /* Look up in cache */
    u32 slot = hash % SHADER_CACHE_SIZE;
    ShaderCacheEntry* entry = &s_d3d.shader_cache[slot];
    if (entry->key == hash) {
        s_d3d.active_shader = entry;
        return;
    }

    /* Cache miss — decompile and compile */
    static int compile_count = 0;
    if (compile_count < 5) {
        printf("[D3D12] Shader cache miss: fp=0x%X(off=0x%X loc=%u, %u bytes), vp=%u instrs, hash=0x%08X\n",
               fp_addr, fp_off, fp_loc, fp_size, vp_count, hash);
    }

    static char fp_hlsl[64 * 1024];
    static char vp_hlsl[64 * 1024];
    int fp_ok = -1, vp_ok = -1;

    if (fp_size > 0) {
        fp_ok = rsx_fp_decompile(vm_base + fp_addr, fp_size,
                                  fp_hlsl, sizeof(fp_hlsl));
    }
    if (vp_count > 0) {
        vp_ok = rsx_vp_decompile((const u8*)state->vp_instructions,
                                  vp_count * 16, vp_hlsl, sizeof(vp_hlsl));
    }

    if (fp_ok < 0 || vp_ok < 0) {
        if (compile_count < 5) {
            printf("[D3D12] Decompilation failed (fp=%d, vp=%d), using fallback\n",
                   fp_ok, vp_ok);
        }
        s_d3d.active_shader = NULL;
        compile_count++;
        return;
    }

    if (compile_count < 3) {
        printf("[D3D12] === Decompiled VP HLSL ===\n%s\n", vp_hlsl);
        printf("[D3D12] === Decompiled FP HLSL ===\n%s\n", fp_hlsl);
    }

    /* Evict old entry if present */
    if (entry->key != 0) {
        if (entry->pso_tri)   entry->pso_tri->lpVtbl->Release(entry->pso_tri);
        if (entry->pso_line)  entry->pso_line->lpVtbl->Release(entry->pso_line);
        if (entry->pso_point) entry->pso_point->lpVtbl->Release(entry->pso_point);
    }

    /* Use the existing root signature for now (it has b0 with 16 constants).
     * The decompiled VP expects vp_c[1024] at b0 which is larger, but the
     * root constants approach only sends 16 floats — good enough for MVP.
     * A full CBV will be added later. */
    ID3D12RootSignature* rs = s_d3d.root_signature;

    entry->key      = hash;
    entry->root_sig = rs;
    entry->pso_tri  = compile_pso(vp_hlsl, fp_hlsl, rs, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    entry->pso_line = compile_pso(vp_hlsl, fp_hlsl, rs, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    entry->pso_point= compile_pso(vp_hlsl, fp_hlsl, rs, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT);

    if (entry->pso_tri) {
        s_d3d.active_shader = entry;
        if (compile_count < 5)
            printf("[D3D12] Shader compiled and cached (hash=0x%08X)\n", hash);
    } else {
        entry->key = 0;
        s_d3d.active_shader = NULL;
        printf("[D3D12] Shader compilation failed, using fallback\n");
    }
    compile_count++;
}

static void d3d12_set_blend(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: modify PSO blend state or use dynamic state.
     * D3D12 requires PSO recreation for blend state changes,
     * so we'd need a PSO cache keyed by blend configuration. */
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_blend(enable=%d, sfactor=0x%X, dfactor=0x%X)\n",
               state->blend_enable, state->blend_sfactor, state->blend_dfactor);
        log_count++;
    }
}

static void d3d12_set_depth_stencil(void* ud, const rsx_state* state)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_depth_stencil(depth=%d, stencil=%d, func=0x%X)\n",
               state->depth_test_enable, state->stencil_test_enable,
               state->depth_func);
        log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_d3d12_backend = {0};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_d3d, 0, sizeof(s_d3d));
    s_d3d.width = width;
    s_d3d.height = height;
    s_d3d.clear_color[0] = 0.0f;
    s_d3d.clear_color[1] = 0.0f;
    s_d3d.clear_color[2] = 0.1f;
    s_d3d.clear_color[3] = 1.0f;

    /* Create window */
    s_d3d.hwnd = create_window(width, height, title);
    if (!s_d3d.hwnd) {
        printf("[D3D12] ERROR: Window creation failed\n");
        return -1;
    }

    /* Initialize D3D12 */
    if (init_d3d12(width, height) != 0) {
        printf("[D3D12] ERROR: D3D12 initialization failed\n");
        return -1;
    }

    /* Set up backend callbacks */
    s_d3d12_backend.userdata          = &s_d3d;
    s_d3d12_backend.init              = d3d12_init;
    s_d3d12_backend.shutdown          = d3d12_shutdown;
    s_d3d12_backend.begin_frame       = d3d12_begin_frame;
    s_d3d12_backend.end_frame         = d3d12_end_frame;
    s_d3d12_backend.present           = d3d12_present;
    s_d3d12_backend.clear             = d3d12_clear;
    s_d3d12_backend.set_render_target = d3d12_set_render_target;
    s_d3d12_backend.set_viewport      = d3d12_set_viewport;
    s_d3d12_backend.set_blend         = d3d12_set_blend;
    s_d3d12_backend.set_depth_stencil = d3d12_set_depth_stencil;
    s_d3d12_backend.set_shader        = d3d12_set_shader;
    s_d3d12_backend.set_vertex_attribs = d3d12_set_vertex_attribs;
    s_d3d12_backend.draw_arrays       = d3d12_draw_arrays;
    s_d3d12_backend.draw_indexed      = d3d12_draw_indexed;
    s_d3d12_backend.draw_inline       = d3d12_draw_inline;
    s_d3d12_backend.bind_texture      = d3d12_bind_texture;

    rsx_set_backend(&s_d3d12_backend);

    s_d3d.initialized = 1;
    s_d3d.last_fps_time = GetTickCount64();

    printf("[D3D12] Backend ready: %ux%u\n", width, height);
    return 0;
}

void rsx_d3d12_backend_shutdown(void)
{
    if (!s_d3d.initialized) return;

    wait_for_gpu();

    /* Release shader cache */
    for (u32 i = 0; i < SHADER_CACHE_SIZE; i++) {
        ShaderCacheEntry* e = &s_d3d.shader_cache[i];
        if (e->key) {
            if (e->pso_tri)   e->pso_tri->lpVtbl->Release(e->pso_tri);
            if (e->pso_line)  e->pso_line->lpVtbl->Release(e->pso_line);
            if (e->pso_point) e->pso_point->lpVtbl->Release(e->pso_point);
        }
    }

    /* Release D3D12 resources */
    if (s_d3d.vertex_buffer) {
        s_d3d.vertex_buffer->lpVtbl->Unmap(s_d3d.vertex_buffer, 0, NULL);
        s_d3d.vertex_buffer->lpVtbl->Release(s_d3d.vertex_buffer);
    }
    if (s_d3d.pipeline_state)        s_d3d.pipeline_state->lpVtbl->Release(s_d3d.pipeline_state);
    if (s_d3d.pipeline_state_lines)  s_d3d.pipeline_state_lines->lpVtbl->Release(s_d3d.pipeline_state_lines);
    if (s_d3d.pipeline_state_points) s_d3d.pipeline_state_points->lpVtbl->Release(s_d3d.pipeline_state_points);
    if (s_d3d.depth_buffer) s_d3d.depth_buffer->lpVtbl->Release(s_d3d.depth_buffer);
    if (s_d3d.dsv_heap)     s_d3d.dsv_heap->lpVtbl->Release(s_d3d.dsv_heap);
    if (s_d3d.root_signature) s_d3d.root_signature->lpVtbl->Release(s_d3d.root_signature);
    if (s_d3d.fence) s_d3d.fence->lpVtbl->Release(s_d3d.fence);
    if (s_d3d.fence_event) CloseHandle(s_d3d.fence_event);
    if (s_d3d.cmd_list) s_d3d.cmd_list->lpVtbl->Release(s_d3d.cmd_list);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (s_d3d.cmd_allocators[i]) s_d3d.cmd_allocators[i]->lpVtbl->Release(s_d3d.cmd_allocators[i]);
        if (s_d3d.render_targets[i]) s_d3d.render_targets[i]->lpVtbl->Release(s_d3d.render_targets[i]);
    }
    if (s_d3d.rtv_heap) s_d3d.rtv_heap->lpVtbl->Release(s_d3d.rtv_heap);
    if (s_d3d.swap_chain) s_d3d.swap_chain->lpVtbl->Release(s_d3d.swap_chain);
    if (s_d3d.cmd_queue) s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
    if (s_d3d.device) s_d3d.device->lpVtbl->Release(s_d3d.device);

    if (s_d3d.hwnd) DestroyWindow(s_d3d.hwnd);

    rsx_set_backend(NULL);
    s_d3d.initialized = 0;

    printf("[D3D12] Backend shut down\n");
}

int rsx_d3d12_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_d3d.window_closed ? -1 : 0;
}

void rsx_d3d12_backend_present(void)
{
    if (s_d3d.initialized)
        render_frame();
}

#else /* !_WIN32 */

#include <ps3emu/ps3types.h>   /* u32 (header includes above are inside the _WIN32 guard) */
#include <stdio.h>

/* Stub for non-Windows — D3D12 is Windows-only */
int rsx_d3d12_backend_init(u32 w, u32 h, const char* t)
{
    (void)w; (void)h; (void)t;
    printf("[D3D12] Not available on this platform (use Vulkan backend)\n");
    return -1;
}
void rsx_d3d12_backend_shutdown(void) {}
int rsx_d3d12_backend_pump_messages(void) { return 0; }
void rsx_d3d12_backend_present(void) {}

#endif /* _WIN32 */
