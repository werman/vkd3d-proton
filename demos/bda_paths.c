/*
 * Copyright © 2026 Valve Corporation.
 * SPDX-License-Identifier: MIT
 */

#define INITGUID
#include <assert.h>
#include "demo.h"

#include "bda_paths_shaders/headers/bda_counter.h"
#include "bda_paths_shaders/headers/bda_exec_graphics_ps.h"
#include "bda_paths_shaders/headers/bda_exec_graphics_vs.h"
#include "bda_paths_shaders/headers/bda_present_ps.h"
#include "bda_paths_shaders/headers/bda_present_vs.h"
#include "bda_paths_shaders/headers/bda_query.h"
#include "bda_paths_shaders/headers/bda_root.h"
#include "bda_paths_shaders/headers/bda_store.h"

#define BDA_PATH_COUNT 6
#define BDA_PATHS_ENABLE_DGC 0
#define CB_SLOT_SIZE 256
#define SWAPCHAIN_BUFFER_COUNT 3

enum bda_path
{
    BDA_PATH_ROOT_DESCRIPTOR,
    BDA_PATH_UAV_COUNTER,
    BDA_PATH_EXECUTE_INDIRECT_COMPUTE,
    BDA_PATH_EXECUTE_INDIRECT_GRAPHICS,
    BDA_PATH_PREDICATION,
    BDA_PATH_QUERY_RESOLVE,
};

struct cxt_fence
{
    ID3D12Fence *fence;
    UINT64 value;
    HANDLE event;
};

struct shader_args
{
    UINT expected;
    UINT source_value;
    UINT output_index;
    UINT frame_id;
};

#if BDA_PATHS_ENABLE_DGC
struct indirect_compute_args
{
    D3D12_GPU_VIRTUAL_ADDRESS srv;
    D3D12_GPU_VIRTUAL_ADDRESS uav;
    D3D12_DISPATCH_ARGUMENTS dispatch;
};

struct indirect_graphics_args
{
    D3D12_GPU_VIRTUAL_ADDRESS uav;
    D3D12_DRAW_ARGUMENTS draw;
};
#endif

struct cx_bda_paths
{
    struct demo demo;
    struct demo_window *window;
    unsigned int width;
    unsigned int height;

    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor_rect;

    ID3D12Device *device;
    ID3D12CommandQueue *command_queue;
    struct demo_swapchain *swapchain;
    ID3D12DescriptorHeap *rtv_heap;
    ID3D12DescriptorHeap *srv_uav_heap;
    UINT rtv_descriptor_size;
    UINT srv_uav_descriptor_size;
    ID3D12Resource *render_targets[SWAPCHAIN_BUFFER_COUNT];
    ID3D12Resource *dummy_render_target;
    ID3D12CommandAllocator *command_allocator;
    ID3D12GraphicsCommandList *command_list;

    ID3D12RootSignature *root_bda_signature;
    ID3D12RootSignature *counter_signature;
    ID3D12RootSignature *store_signature;
    ID3D12RootSignature *exec_graphics_signature;
    ID3D12RootSignature *present_signature;
    ID3D12PipelineState *root_bda_pso;
    ID3D12PipelineState *counter_pso;
    ID3D12PipelineState *store_pso;
    ID3D12PipelineState *query_pso;
    ID3D12PipelineState *exec_graphics_pso;
    ID3D12PipelineState *present_pso;
#if BDA_PATHS_ENABLE_DGC
    ID3D12CommandSignature *compute_command_signature;
    ID3D12CommandSignature *graphics_command_signature;
#endif
    ID3D12QueryHeap *query_heap;

    ID3D12Resource *status_buffer;
    ID3D12Resource *status_clear_upload;
    ID3D12Resource *source_buffer;
    ID3D12Resource *constant_buffer;
    ID3D12Resource *counter_buffer;
    ID3D12Resource *counted_buffer;
    ID3D12Resource *counter_reset_upload;
    ID3D12Resource *reference_texture;
    ID3D12Resource *reference_upload;
    ID3D12Resource *query_buffer;
    ID3D12Resource *predicate_buffer;
#if BDA_PATHS_ENABLE_DGC
    ID3D12Resource *indirect_compute_buffer;
    ID3D12Resource *indirect_compute_count_buffer;
    ID3D12Resource *indirect_graphics_buffer;
#endif

    struct shader_args *constant_data;
    UINT *source_data;
    UINT *reference_data;
#if BDA_PATHS_ENABLE_DGC
    struct indirect_graphics_args *indirect_graphics_data;
#endif
    UINT exec_graphics_constants[2];

    unsigned int frame_idx;
    unsigned int frame_id;
    struct cxt_fence fence;
};

static UINT bda_expected_value(enum bda_path path, unsigned int frame_id)
{
    return 0x10000u | (path & 0xffu) | ((frame_id & 1u) << 8);
}

static D3D12_CPU_DESCRIPTOR_HANDLE cxt_cpu_handle(struct cx_bda_paths *cxt,
        ID3D12DescriptorHeap *heap, UINT size, UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    handle.ptr += index * size;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE cxt_gpu_handle(struct cx_bda_paths *cxt,
        ID3D12DescriptorHeap *heap, UINT size, UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    handle.ptr += index * size;
    return handle;
}

static ID3D12Resource *cxt_create_buffer(struct cx_bda_paths *cxt, UINT64 size,
        D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap_desc;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Resource *resource;
    HRESULT hr;

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = heap_type;
    heap_desc.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_desc.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_desc.CreationNodeMask = 1;
    heap_desc.VisibleNodeMask = 1;

    memset(&resource_desc, 0, sizeof(resource_desc));
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Width = size;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = flags;

    hr = ID3D12Device_CreateCommittedResource(cxt->device, &heap_desc, D3D12_HEAP_FLAG_NONE,
            &resource_desc, state, NULL, &IID_ID3D12Resource, (void **)&resource);
    assert(SUCCEEDED(hr));
    return resource;
}

static ID3D12Resource *cxt_create_texture2d(struct cx_bda_paths *cxt, UINT width, UINT height,
        DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap_desc;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Resource *resource;
    HRESULT hr;

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_desc.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_desc.CreationNodeMask = 1;
    heap_desc.VisibleNodeMask = 1;

    memset(&resource_desc, 0, sizeof(resource_desc));
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = width;
    resource_desc.Height = height;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = format;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = flags;

    hr = ID3D12Device_CreateCommittedResource(cxt->device, &heap_desc, D3D12_HEAP_FLAG_NONE,
            &resource_desc, state, NULL, &IID_ID3D12Resource, (void **)&resource);
    assert(SUCCEEDED(hr));
    return resource;
}

static void cxt_transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier;

    if (before == after)
        return;

    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

static void cxt_uav_barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource)
{
    D3D12_RESOURCE_BARRIER barrier;

    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

static void cxt_update_frame_data(struct cx_bda_paths *cxt)
{
    unsigned int i;
    UINT source_value = 0x60000000u ^ cxt->frame_id;

    *cxt->source_data = source_value;

    for (i = 0; i < BDA_PATH_COUNT; i++)
        cxt->reference_data[i] = bda_expected_value(i, cxt->frame_id);

    cxt->constant_data[BDA_PATH_ROOT_DESCRIPTOR] = (struct shader_args)
    {
        bda_expected_value(BDA_PATH_ROOT_DESCRIPTOR, cxt->frame_id), source_value,
        BDA_PATH_ROOT_DESCRIPTOR, cxt->frame_id
    };
    cxt->constant_data[BDA_PATH_UAV_COUNTER] = (struct shader_args)
    {
        bda_expected_value(BDA_PATH_UAV_COUNTER, cxt->frame_id), source_value,
        BDA_PATH_UAV_COUNTER, cxt->frame_id
    };
    cxt->constant_data[BDA_PATH_EXECUTE_INDIRECT_COMPUTE] = (struct shader_args)
    {
        bda_expected_value(BDA_PATH_EXECUTE_INDIRECT_COMPUTE, cxt->frame_id), source_value,
        BDA_PATH_EXECUTE_INDIRECT_COMPUTE, cxt->frame_id
    };
    cxt->constant_data[BDA_PATH_PREDICATION] = (struct shader_args)
    {
        bda_expected_value(BDA_PATH_PREDICATION, cxt->frame_id), 0,
        BDA_PATH_PREDICATION, cxt->frame_id
    };
    cxt->constant_data[BDA_PATH_PREDICATION + 1] = (struct shader_args)
    {
        0xffffffffu, 0, BDA_PATH_PREDICATION, cxt->frame_id
    };
    cxt->constant_data[BDA_PATH_QUERY_RESOLVE + 1] = (struct shader_args)
    {
        bda_expected_value(BDA_PATH_QUERY_RESOLVE, cxt->frame_id), 0,
        BDA_PATH_QUERY_RESOLVE, cxt->frame_id
    };

    cxt->exec_graphics_constants[0] =
            bda_expected_value(BDA_PATH_EXECUTE_INDIRECT_GRAPHICS, cxt->frame_id);
    cxt->exec_graphics_constants[1] = BDA_PATH_EXECUTE_INDIRECT_GRAPHICS;
}

static D3D12_GPU_VIRTUAL_ADDRESS cxt_cbv(struct cx_bda_paths *cxt, unsigned int slot)
{
    return ID3D12Resource_GetGPUVirtualAddress(cxt->constant_buffer) + slot * CB_SLOT_SIZE;
}

static void cxt_populate_command_list(struct cx_bda_paths *cxt)
{
    D3D12_TEXTURE_COPY_LOCATION dst_location, src_location;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    ID3D12DescriptorHeap *heaps[] = { cxt->srv_uav_heap };
    HRESULT hr;

    cxt_update_frame_data(cxt);

    hr = ID3D12CommandAllocator_Reset(cxt->command_allocator);
    assert(SUCCEEDED(hr));
    hr = ID3D12GraphicsCommandList_Reset(cxt->command_list, cxt->command_allocator, NULL);
    assert(SUCCEEDED(hr));

    cxt_transition(cxt->command_list, cxt->status_buffer,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(cxt->command_list, cxt->status_buffer, 0,
            cxt->status_clear_upload, 0, BDA_PATH_COUNT * sizeof(UINT));
    cxt_transition(cxt->command_list, cxt->status_buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cxt_transition(cxt->command_list, cxt->reference_texture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    memset(&dst_location, 0, sizeof(dst_location));
    dst_location.pResource = cxt->reference_texture;
    dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_location.SubresourceIndex = 0;
    memset(&src_location, 0, sizeof(src_location));
    src_location.pResource = cxt->reference_upload;
    src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
    src_location.PlacedFootprint.Footprint.Width = BDA_PATH_COUNT;
    src_location.PlacedFootprint.Footprint.Height = 1;
    src_location.PlacedFootprint.Footprint.Depth = 1;
    src_location.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(cxt->command_list, &dst_location, 0, 0, 0, &src_location, NULL);
    cxt_transition(cxt->command_list, cxt->reference_texture,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cxt_transition(cxt->command_list, cxt->counter_buffer,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(cxt->command_list, cxt->counter_buffer, 0,
            cxt->counter_reset_upload, 0, sizeof(UINT));
    cxt_transition(cxt->command_list, cxt->counter_buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12GraphicsCommandList_SetComputeRootSignature(cxt->command_list, cxt->root_bda_signature);
    ID3D12GraphicsCommandList_SetPipelineState(cxt->command_list, cxt->root_bda_pso);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cxt->command_list, 0,
            cxt_cbv(cxt, BDA_PATH_ROOT_DESCRIPTOR));
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cxt->command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cxt->source_buffer));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cxt->command_list, 2,
            ID3D12Resource_GetGPUVirtualAddress(cxt->status_buffer));
    ID3D12GraphicsCommandList_Dispatch(cxt->command_list, 1, 1, 1);
    cxt_uav_barrier(cxt->command_list, cxt->status_buffer);

    ID3D12GraphicsCommandList_SetDescriptorHeaps(cxt->command_list, ARRAY_SIZE(heaps), heaps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cxt->command_list, cxt->counter_signature);
    ID3D12GraphicsCommandList_SetPipelineState(cxt->command_list, cxt->counter_pso);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cxt->command_list, 0,
            cxt_cbv(cxt, BDA_PATH_UAV_COUNTER));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cxt->command_list, 1,
            cxt_gpu_handle(cxt, cxt->srv_uav_heap, cxt->srv_uav_descriptor_size, 0));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cxt->command_list, 2,
            ID3D12Resource_GetGPUVirtualAddress(cxt->status_buffer));
    ID3D12GraphicsCommandList_Dispatch(cxt->command_list, 1, 1, 1);
    cxt_uav_barrier(cxt->command_list, cxt->status_buffer);

    ID3D12GraphicsCommandList_SetComputeRootSignature(cxt->command_list, cxt->root_bda_signature);
    ID3D12GraphicsCommandList_SetPipelineState(cxt->command_list, cxt->root_bda_pso);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cxt->command_list, 0,
            cxt_cbv(cxt, BDA_PATH_EXECUTE_INDIRECT_COMPUTE));
#if BDA_PATHS_ENABLE_DGC
    ID3D12GraphicsCommandList_ExecuteIndirect(cxt->command_list, cxt->compute_command_signature, 4,
            cxt->indirect_compute_buffer, 0, cxt->indirect_compute_count_buffer, 0);
#else
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cxt->command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cxt->source_buffer));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cxt->command_list, 2,
            ID3D12Resource_GetGPUVirtualAddress(cxt->status_buffer));
    ID3D12GraphicsCommandList_Dispatch(cxt->command_list, 1, 1, 1);
#endif
    cxt_uav_barrier(cxt->command_list, cxt->status_buffer);

    rtv_handle = cxt_cpu_handle(cxt, cxt->rtv_heap, cxt->rtv_descriptor_size, SWAPCHAIN_BUFFER_COUNT);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cxt->command_list, cxt->exec_graphics_signature);
    ID3D12GraphicsCommandList_SetPipelineState(cxt->command_list, cxt->exec_graphics_pso);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cxt->command_list, 1, &rtv_handle, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(cxt->command_list, 1, &cxt->viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(cxt->command_list, 1, &cxt->scissor_rect);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cxt->command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cxt->command_list, 0,
            ARRAY_SIZE(cxt->exec_graphics_constants), cxt->exec_graphics_constants, 0);
#if BDA_PATHS_ENABLE_DGC
    ID3D12GraphicsCommandList_ExecuteIndirect(cxt->command_list, cxt->graphics_command_signature, 1,
            cxt->indirect_graphics_buffer, 0, NULL, 0);
#else
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(cxt->command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cxt->status_buffer));
    ID3D12GraphicsCommandList_DrawInstanced(cxt->command_list, 3, 1, 0, 0);
#endif
    cxt_uav_barrier(cxt->command_list, cxt->status_buffer);

    ID3D12GraphicsCommandList_SetComputeRootSignature(cxt->command_list, cxt->store_signature);
    ID3D12GraphicsCommandList_SetPipelineState(cxt->command_list, cxt->store_pso);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cxt->command_list, 0,
            cxt_cbv(cxt, BDA_PATH_PREDICATION));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cxt->command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cxt->status_buffer));
    ID3D12GraphicsCommandList_SetPredication(cxt->command_list, cxt->predicate_buffer, 0,
            D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
    ID3D12GraphicsCommandList_Dispatch(cxt->command_list, 1, 1, 1);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cxt->command_list, 0,
            cxt_cbv(cxt, BDA_PATH_PREDICATION + 1));
    ID3D12GraphicsCommandList_SetPredication(cxt->command_list, cxt->predicate_buffer, sizeof(UINT64),
            D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
    ID3D12GraphicsCommandList_Dispatch(cxt->command_list, 1, 1, 1);
    ID3D12GraphicsCommandList_SetPredication(cxt->command_list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    cxt_uav_barrier(cxt->command_list, cxt->status_buffer);

    cxt_transition(cxt->command_list, cxt->query_buffer,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_EndQuery(cxt->command_list, cxt->query_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    ID3D12GraphicsCommandList_EndQuery(cxt->command_list, cxt->query_heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    ID3D12GraphicsCommandList_ResolveQueryData(cxt->command_list, cxt->query_heap,
            D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, cxt->query_buffer, 0);
    cxt_transition(cxt->command_list, cxt->query_buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cxt->command_list, cxt->root_bda_signature);
    ID3D12GraphicsCommandList_SetPipelineState(cxt->command_list, cxt->query_pso);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cxt->command_list, 0,
            cxt_cbv(cxt, BDA_PATH_QUERY_RESOLVE + 1));
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cxt->command_list, 1,
            ID3D12Resource_GetGPUVirtualAddress(cxt->query_buffer));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cxt->command_list, 2,
            ID3D12Resource_GetGPUVirtualAddress(cxt->status_buffer));
    ID3D12GraphicsCommandList_Dispatch(cxt->command_list, 1, 1, 1);
    cxt_uav_barrier(cxt->command_list, cxt->status_buffer);

    cxt_transition(cxt->command_list, cxt->status_buffer,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cxt_transition(cxt->command_list, cxt->render_targets[cxt->frame_idx],
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rtv_handle = cxt_cpu_handle(cxt, cxt->rtv_heap, cxt->rtv_descriptor_size, cxt->frame_idx);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cxt->command_list, 1, &rtv_handle, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cxt->command_list, cxt->present_signature);
    ID3D12GraphicsCommandList_SetPipelineState(cxt->command_list, cxt->present_pso);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cxt->command_list, 0,
            cxt_gpu_handle(cxt, cxt->srv_uav_heap, cxt->srv_uav_descriptor_size, 1));
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cxt->command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cxt->command_list, 3, 1, 0, 0);
    cxt_transition(cxt->command_list, cxt->render_targets[cxt->frame_idx],
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    hr = ID3D12GraphicsCommandList_Close(cxt->command_list);
    assert(SUCCEEDED(hr));
}

static void cxt_wait_for_previous_frame(struct cx_bda_paths *cxt)
{
    struct cxt_fence *fence = &cxt->fence;
    const UINT64 v = fence->value;
    HRESULT hr;

    hr = ID3D12CommandQueue_Signal(cxt->command_queue, fence->fence, v);
    assert(SUCCEEDED(hr));

    ++fence->value;

    if (ID3D12Fence_GetCompletedValue(fence->fence) < v)
    {
        hr = ID3D12Fence_SetEventOnCompletion(fence->fence, v, fence->event);
        assert(SUCCEEDED(hr));
        demo_wait_event(fence->event);
    }

    cxt->frame_idx = demo_swapchain_get_current_back_buffer_index(cxt->swapchain);
}

static void cxt_render_frame(struct demo_window *window, void *user_data)
{
    struct cx_bda_paths *cxt = user_data;

    cxt_populate_command_list(cxt);
    ID3D12CommandQueue_ExecuteCommandLists(cxt->command_queue, 1, (ID3D12CommandList **)&cxt->command_list);
    demo_swapchain_present(cxt->swapchain);
    cxt_wait_for_previous_frame(cxt);
    cxt->frame_id++;
}

static void cxt_create_root_signatures(struct cx_bda_paths *cxt)
{
    D3D12_DESCRIPTOR_RANGE ranges[2];
    D3D12_ROOT_SIGNATURE_DESC desc;
    D3D12_ROOT_PARAMETER params[3];
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    memset(params, 0, sizeof(params));
    desc.NumParameters = 3;
    desc.pParameters = params;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    hr = demo_create_root_signature(cxt->device, &desc, &cxt->root_bda_signature);
    assert(SUCCEEDED(hr));

    memset(&desc, 0, sizeof(desc));
    memset(params, 0, sizeof(params));
    memset(ranges, 0, sizeof(ranges));
    desc.NumParameters = 3;
    desc.pParameters = params;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 1;
    hr = demo_create_root_signature(cxt->device, &desc, &cxt->counter_signature);
    assert(SUCCEEDED(hr));

    memset(&desc, 0, sizeof(desc));
    memset(params, 0, sizeof(params));
    desc.NumParameters = 2;
    desc.pParameters = params;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    hr = demo_create_root_signature(cxt->device, &desc, &cxt->store_signature);
    assert(SUCCEEDED(hr));

    memset(&desc, 0, sizeof(desc));
    memset(params, 0, sizeof(params));
    desc.NumParameters = 2;
    desc.pParameters = params;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 2;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 1;
    hr = demo_create_root_signature(cxt->device, &desc, &cxt->exec_graphics_signature);
    assert(SUCCEEDED(hr));

    memset(&desc, 0, sizeof(desc));
    memset(params, 0, sizeof(params));
    memset(ranges, 0, sizeof(ranges));
    desc.NumParameters = 1;
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 0;
    hr = demo_create_root_signature(cxt->device, &desc, &cxt->present_signature);
    assert(SUCCEEDED(hr));
}

static void cxt_create_pipeline_states(struct cx_bda_paths *cxt)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc;
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc;
    HRESULT hr;

    memset(&compute_desc, 0, sizeof(compute_desc));
    compute_desc.pRootSignature = cxt->root_bda_signature;
    compute_desc.CS = bda_root_dxbc;
    hr = ID3D12Device_CreateComputePipelineState(cxt->device, &compute_desc,
            &IID_ID3D12PipelineState, (void **)&cxt->root_bda_pso);
    assert(SUCCEEDED(hr));

    compute_desc.pRootSignature = cxt->counter_signature;
    compute_desc.CS = bda_counter_dxbc;
    hr = ID3D12Device_CreateComputePipelineState(cxt->device, &compute_desc,
            &IID_ID3D12PipelineState, (void **)&cxt->counter_pso);
    assert(SUCCEEDED(hr));

    compute_desc.pRootSignature = cxt->store_signature;
    compute_desc.CS = bda_store_dxbc;
    hr = ID3D12Device_CreateComputePipelineState(cxt->device, &compute_desc,
            &IID_ID3D12PipelineState, (void **)&cxt->store_pso);
    assert(SUCCEEDED(hr));

    compute_desc.pRootSignature = cxt->root_bda_signature;
    compute_desc.CS = bda_query_dxbc;
    hr = ID3D12Device_CreateComputePipelineState(cxt->device, &compute_desc,
            &IID_ID3D12PipelineState, (void **)&cxt->query_pso);
    assert(SUCCEEDED(hr));

    memset(&graphics_desc, 0, sizeof(graphics_desc));
    graphics_desc.pRootSignature = cxt->exec_graphics_signature;
    graphics_desc.VS = bda_exec_graphics_vs_dxbc;
    graphics_desc.PS = bda_exec_graphics_ps_dxbc;
    demo_rasterizer_desc_init_default(&graphics_desc.RasterizerState);
    graphics_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    demo_blend_desc_init_default(&graphics_desc.BlendState);
    graphics_desc.DepthStencilState.DepthEnable = FALSE;
    graphics_desc.DepthStencilState.StencilEnable = FALSE;
    graphics_desc.SampleMask = UINT_MAX;
    graphics_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics_desc.NumRenderTargets = 1;
    graphics_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    graphics_desc.SampleDesc.Count = 1;
    hr = ID3D12Device_CreateGraphicsPipelineState(cxt->device, &graphics_desc,
            &IID_ID3D12PipelineState, (void **)&cxt->exec_graphics_pso);
    assert(SUCCEEDED(hr));

    graphics_desc.pRootSignature = cxt->present_signature;
    graphics_desc.VS = bda_present_vs_dxbc;
    graphics_desc.PS = bda_present_ps_dxbc;
    graphics_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    hr = ID3D12Device_CreateGraphicsPipelineState(cxt->device, &graphics_desc,
            &IID_ID3D12PipelineState, (void **)&cxt->present_pso);
    assert(SUCCEEDED(hr));
}

#if BDA_PATHS_ENABLE_DGC
static void cxt_create_command_signatures(struct cx_bda_paths *cxt)
{
    D3D12_COMMAND_SIGNATURE_DESC desc;
    D3D12_INDIRECT_ARGUMENT_DESC args[4];
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    memset(args, 0, sizeof(args));
    desc.NumArgumentDescs = 3;
    desc.pArgumentDescs = args;
    desc.ByteStride = sizeof(struct indirect_compute_args);
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW;
    args[0].ShaderResourceView.RootParameterIndex = 1;
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
    args[1].UnorderedAccessView.RootParameterIndex = 2;
    args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    hr = ID3D12Device_CreateCommandSignature(cxt->device, &desc, cxt->root_bda_signature,
            &IID_ID3D12CommandSignature, (void **)&cxt->compute_command_signature);
    assert(SUCCEEDED(hr));

    memset(&desc, 0, sizeof(desc));
    memset(args, 0, sizeof(args));
    desc.NumArgumentDescs = 2;
    desc.pArgumentDescs = args;
    desc.ByteStride = sizeof(struct indirect_graphics_args);
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
    args[0].UnorderedAccessView.RootParameterIndex = 1;
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    hr = ID3D12Device_CreateCommandSignature(cxt->device, &desc, cxt->exec_graphics_signature,
            &IID_ID3D12CommandSignature, (void **)&cxt->graphics_command_signature);
    assert(SUCCEEDED(hr));
}
#endif

static void cxt_create_descriptors(struct cx_bda_paths *cxt)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc;

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 3;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    assert(SUCCEEDED(ID3D12Device_CreateDescriptorHeap(cxt->device, &heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cxt->srv_uav_heap)));
    cxt->srv_uav_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(cxt->device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    memset(&uav_desc, 0, sizeof(uav_desc));
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.NumElements = 1;
    uav_desc.Buffer.StructureByteStride = sizeof(UINT);
    ID3D12Device_CreateUnorderedAccessView(cxt->device, cxt->counted_buffer, cxt->counter_buffer,
            &uav_desc, cxt_cpu_handle(cxt, cxt->srv_uav_heap, cxt->srv_uav_descriptor_size, 0));

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = DXGI_FORMAT_R32_UINT;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(cxt->device, cxt->reference_texture, &srv_desc,
            cxt_cpu_handle(cxt, cxt->srv_uav_heap, cxt->srv_uav_descriptor_size, 1));

    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Buffer.NumElements = BDA_PATH_COUNT;
    srv_desc.Buffer.StructureByteStride = sizeof(UINT);
    ID3D12Device_CreateShaderResourceView(cxt->device, cxt->status_buffer, &srv_desc,
            cxt_cpu_handle(cxt, cxt->srv_uav_heap, cxt->srv_uav_descriptor_size, 2));
}

static void cxt_load_pipeline(struct cx_bda_paths *cxt)
{
    struct demo_swapchain_desc swapchain_desc;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    unsigned int i;
    HRESULT hr;

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&cxt->device);
    assert(SUCCEEDED(hr));

    memset(&queue_desc, 0, sizeof(queue_desc));
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = ID3D12Device_CreateCommandQueue(cxt->device, &queue_desc,
            &IID_ID3D12CommandQueue, (void **)&cxt->command_queue);
    assert(SUCCEEDED(hr));

    swapchain_desc.buffer_count = ARRAY_SIZE(cxt->render_targets);
    swapchain_desc.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapchain_desc.width = cxt->width;
    swapchain_desc.height = cxt->height;
    cxt->swapchain = demo_swapchain_create(cxt->command_queue, cxt->window, &swapchain_desc);
    assert(cxt->swapchain);
    cxt->frame_idx = demo_swapchain_get_current_back_buffer_index(cxt->swapchain);

    memset(&rtv_heap_desc, 0, sizeof(rtv_heap_desc));
    rtv_heap_desc.NumDescriptors = ARRAY_SIZE(cxt->render_targets) + 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = ID3D12Device_CreateDescriptorHeap(cxt->device, &rtv_heap_desc,
            &IID_ID3D12DescriptorHeap, (void **)&cxt->rtv_heap);
    assert(SUCCEEDED(hr));

    cxt->rtv_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(cxt->device,
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtv_handle = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cxt->rtv_heap);
    for (i = 0; i < ARRAY_SIZE(cxt->render_targets); ++i)
    {
        cxt->render_targets[i] = demo_swapchain_get_back_buffer(cxt->swapchain, i);
        ID3D12Device_CreateRenderTargetView(cxt->device, cxt->render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += cxt->rtv_descriptor_size;
    }

    cxt->dummy_render_target = cxt_create_texture2d(cxt, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Device_CreateRenderTargetView(cxt->device, cxt->dummy_render_target, NULL, rtv_handle);

    hr = ID3D12Device_CreateCommandAllocator(cxt->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&cxt->command_allocator);
    assert(SUCCEEDED(hr));
}

static void cxt_load_assets(struct cx_bda_paths *cxt)
{
#if BDA_PATHS_ENABLE_DGC
    struct indirect_compute_args *compute_args;
#endif
    UINT *clear_values, *count_value;
    UINT64 *predicate_values;
    D3D12_QUERY_HEAP_DESC query_heap_desc;
    D3D12_RANGE read_range = {0, 0};
    HRESULT hr;

    cxt->status_buffer = cxt_create_buffer(cxt, BDA_PATH_COUNT * sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cxt->status_clear_upload = cxt_create_buffer(cxt, BDA_PATH_COUNT * sizeof(UINT), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    cxt->source_buffer = cxt_create_buffer(cxt, sizeof(UINT), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    cxt->constant_buffer = cxt_create_buffer(cxt, 8 * CB_SLOT_SIZE, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    cxt->counter_buffer = cxt_create_buffer(cxt, sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cxt->counted_buffer = cxt_create_buffer(cxt, sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cxt->counter_reset_upload = cxt_create_buffer(cxt, sizeof(UINT), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    cxt->reference_texture = cxt_create_texture2d(cxt, BDA_PATH_COUNT, 1, DXGI_FORMAT_R32_UINT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cxt->reference_upload = cxt_create_buffer(cxt, 256, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    cxt->query_buffer = cxt_create_buffer(cxt, 2 * sizeof(UINT64), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cxt->predicate_buffer = cxt_create_buffer(cxt, 2 * sizeof(UINT64), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
#if BDA_PATHS_ENABLE_DGC
    cxt->indirect_compute_buffer = cxt_create_buffer(cxt, sizeof(struct indirect_compute_args),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    cxt->indirect_compute_count_buffer = cxt_create_buffer(cxt, sizeof(UINT), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    cxt->indirect_graphics_buffer = cxt_create_buffer(cxt, sizeof(struct indirect_graphics_args),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
#endif

    hr = ID3D12Resource_Map(cxt->status_clear_upload, 0, &read_range, (void **)&clear_values);
    assert(SUCCEEDED(hr));
    memset(clear_values, 0xff, BDA_PATH_COUNT * sizeof(UINT));
    ID3D12Resource_Unmap(cxt->status_clear_upload, 0, NULL);

    hr = ID3D12Resource_Map(cxt->counter_reset_upload, 0, &read_range, (void **)&count_value);
    assert(SUCCEEDED(hr));
    *count_value = 0;
    ID3D12Resource_Unmap(cxt->counter_reset_upload, 0, NULL);

    hr = ID3D12Resource_Map(cxt->source_buffer, 0, &read_range, (void **)&cxt->source_data);
    assert(SUCCEEDED(hr));
    hr = ID3D12Resource_Map(cxt->constant_buffer, 0, &read_range, (void **)&cxt->constant_data);
    assert(SUCCEEDED(hr));
    hr = ID3D12Resource_Map(cxt->reference_upload, 0, &read_range, (void **)&cxt->reference_data);
    assert(SUCCEEDED(hr));
#if BDA_PATHS_ENABLE_DGC
    hr = ID3D12Resource_Map(cxt->indirect_graphics_buffer, 0, &read_range, (void **)&cxt->indirect_graphics_data);
    assert(SUCCEEDED(hr));
#endif

    hr = ID3D12Resource_Map(cxt->predicate_buffer, 0, &read_range, (void **)&predicate_values);
    assert(SUCCEEDED(hr));
    predicate_values[0] = 1;
    predicate_values[1] = 0;
    ID3D12Resource_Unmap(cxt->predicate_buffer, 0, NULL);

#if BDA_PATHS_ENABLE_DGC
    hr = ID3D12Resource_Map(cxt->indirect_compute_count_buffer, 0, &read_range, (void **)&count_value);
    assert(SUCCEEDED(hr));
    *count_value = 1;
    ID3D12Resource_Unmap(cxt->indirect_compute_count_buffer, 0, NULL);

    hr = ID3D12Resource_Map(cxt->indirect_compute_buffer, 0, &read_range, (void **)&compute_args);
    assert(SUCCEEDED(hr));
    compute_args->srv = ID3D12Resource_GetGPUVirtualAddress(cxt->source_buffer);
    compute_args->uav = ID3D12Resource_GetGPUVirtualAddress(cxt->status_buffer);
    compute_args->dispatch.ThreadGroupCountX = 1;
    compute_args->dispatch.ThreadGroupCountY = 1;
    compute_args->dispatch.ThreadGroupCountZ = 1;
    ID3D12Resource_Unmap(cxt->indirect_compute_buffer, 0, NULL);

    cxt->indirect_graphics_data->uav = ID3D12Resource_GetGPUVirtualAddress(cxt->status_buffer);
    cxt->indirect_graphics_data->draw.VertexCountPerInstance = 3;
    cxt->indirect_graphics_data->draw.InstanceCount = 1;
    cxt->indirect_graphics_data->draw.StartVertexLocation = 0;
    cxt->indirect_graphics_data->draw.StartInstanceLocation = 0;
#endif

    memset(&query_heap_desc, 0, sizeof(query_heap_desc));
    query_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_heap_desc.Count = 2;
    hr = ID3D12Device_CreateQueryHeap(cxt->device, &query_heap_desc,
            &IID_ID3D12QueryHeap, (void **)&cxt->query_heap);
    assert(SUCCEEDED(hr));

    cxt_create_descriptors(cxt);
    cxt_create_root_signatures(cxt);
    cxt_create_pipeline_states(cxt);
#if BDA_PATHS_ENABLE_DGC
    cxt_create_command_signatures(cxt);
#endif

    hr = ID3D12Device_CreateCommandList(cxt->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            cxt->command_allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cxt->command_list);
    assert(SUCCEEDED(hr));
    hr = ID3D12GraphicsCommandList_Close(cxt->command_list);
    assert(SUCCEEDED(hr));
}

static void cxt_fence_create(struct cxt_fence *fence, ID3D12Device *device)
{
    HRESULT hr;

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence->fence);
    assert(SUCCEEDED(hr));
    fence->value = 1;
    fence->event = demo_create_event();
    assert(fence->event);
}

static void cxt_key_press(struct demo_window *window, demo_key key, void *user_data)
{
    if (key == DEMO_KEY_ESCAPE)
        demo_window_destroy_defer(window);
}

static void cxt_destroy_assets(struct cx_bda_paths *cxt)
{
    ID3D12Fence_Release(cxt->fence.fence);
    demo_destroy_event(cxt->fence.event);
#if BDA_PATHS_ENABLE_DGC
    ID3D12CommandSignature_Release(cxt->graphics_command_signature);
    ID3D12CommandSignature_Release(cxt->compute_command_signature);
#endif
    ID3D12PipelineState_Release(cxt->present_pso);
    ID3D12PipelineState_Release(cxt->exec_graphics_pso);
    ID3D12PipelineState_Release(cxt->query_pso);
    ID3D12PipelineState_Release(cxt->store_pso);
    ID3D12PipelineState_Release(cxt->counter_pso);
    ID3D12PipelineState_Release(cxt->root_bda_pso);
    ID3D12RootSignature_Release(cxt->present_signature);
    ID3D12RootSignature_Release(cxt->exec_graphics_signature);
    ID3D12RootSignature_Release(cxt->store_signature);
    ID3D12RootSignature_Release(cxt->counter_signature);
    ID3D12RootSignature_Release(cxt->root_bda_signature);
    ID3D12QueryHeap_Release(cxt->query_heap);
    ID3D12GraphicsCommandList_Release(cxt->command_list);
#if BDA_PATHS_ENABLE_DGC
    ID3D12Resource_Release(cxt->indirect_graphics_buffer);
    ID3D12Resource_Release(cxt->indirect_compute_count_buffer);
    ID3D12Resource_Release(cxt->indirect_compute_buffer);
#endif
    ID3D12Resource_Release(cxt->predicate_buffer);
    ID3D12Resource_Release(cxt->query_buffer);
    ID3D12Resource_Release(cxt->reference_upload);
    ID3D12Resource_Release(cxt->reference_texture);
    ID3D12Resource_Release(cxt->counter_reset_upload);
    ID3D12Resource_Release(cxt->counted_buffer);
    ID3D12Resource_Release(cxt->counter_buffer);
    ID3D12Resource_Release(cxt->constant_buffer);
    ID3D12Resource_Release(cxt->source_buffer);
    ID3D12Resource_Release(cxt->status_clear_upload);
    ID3D12Resource_Release(cxt->status_buffer);
    ID3D12DescriptorHeap_Release(cxt->srv_uav_heap);
}

static void cxt_destroy_pipeline(struct cx_bda_paths *cxt)
{
    unsigned int i;

    ID3D12CommandAllocator_Release(cxt->command_allocator);
    ID3D12Resource_Release(cxt->dummy_render_target);
    for (i = 0; i < ARRAY_SIZE(cxt->render_targets); ++i)
        ID3D12Resource_Release(cxt->render_targets[i]);
    ID3D12DescriptorHeap_Release(cxt->rtv_heap);
    demo_swapchain_destroy(cxt->swapchain);
    ID3D12CommandQueue_Release(cxt->command_queue);
    ID3D12Device_Release(cxt->device);
}

static int cxt_main(void)
{
    struct cx_bda_paths cxt;
    unsigned int width = 960, height = 360;

    memset(&cxt, 0, sizeof(cxt));

    if (!demo_init(&cxt.demo, NULL))
        return EXIT_FAILURE;

    cxt.window = demo_window_create(&cxt.demo, "vkd3d-proton BDA paths", width, height, &cxt);
    demo_window_set_expose_func(cxt.window, cxt_render_frame);
    demo_window_set_key_press_func(cxt.window, cxt_key_press);

    cxt.width = width;
    cxt.height = height;
    cxt.viewport.Width = (float)width;
    cxt.viewport.Height = (float)height;
    cxt.viewport.MaxDepth = 1.0f;
    cxt.scissor_rect.right = width;
    cxt.scissor_rect.bottom = height;

    cxt_load_pipeline(&cxt);
    cxt_load_assets(&cxt);
    cxt_fence_create(&cxt.fence, cxt.device);
    cxt_wait_for_previous_frame(&cxt);

    demo_process_events(&cxt.demo);

    cxt_wait_for_previous_frame(&cxt);
    cxt_destroy_assets(&cxt);
    cxt_destroy_pipeline(&cxt);
    demo_cleanup(&cxt.demo);

    return EXIT_SUCCESS;
}

#ifdef _WIN32
int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR lpCmdLine,
    int nCmdShow)
#else
int main(void)
#endif
{
    return cxt_main();
}
