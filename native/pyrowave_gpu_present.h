#pragma once
#include "stage_gpu_probe.h"
// Three single-channel external images keep the native compute decoder path.
// Shared D3D11 fences order Vulkan writes and D3D11 reads without CPU readback.
class PyrowaveGpuPresent {
    ComPtr<ID3D11Texture2D> textures[3];
    ComPtr<ID3D11ShaderResourceView> views[3];
    ComPtr<ID3D11Fence> fence;
    ComPtr<ID3D11DeviceContext4> context;
    pyrowave_image images[3]{};
    pyrowave_sync_object sync=nullptr;
    pyrowave_gpu_external_reference external[3]{};
    pyrowave_gpu_buffers buffers{};
    uint64_t timeline=0;
    static void Check(HRESULT hr){if(FAILED(hr))throw hr;}
    static void CheckPyro(pyrowave_result result){if(result!=PYROWAVE_SUCCESS)throw HRESULT(E_FAIL);}
public:
    ~PyrowaveGpuPresent(){for(auto image:images)if(image)pyrowave_image_destroy(image);if(sync)pyrowave_sync_object_destroy(sync);}
    void Init(Nv12Renderer&renderer,pyrowave_device device,bool chroma444=false,int width=2560,int height=1440,bool fragmentDecode=false){
        fprintf(stderr,"GPU interop: importing three shared R8 planes\n");
        ComPtr<ID3D11Device5> d3d;Check(renderer.Device()->QueryInterface(IID_PPV_ARGS(&d3d)));
        Check(renderer.Context()->QueryInterface(IID_PPV_ARGS(&context)));
        for(int p=0;p<3;++p){
            UINT w=p&&!chroma444?width/2:width,h=p&&!chroma444?height/2:height;
            D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_R8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS|(fragmentDecode?D3D11_BIND_RENDER_TARGET:0);td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            Check(d3d->CreateTexture2D(&td,nullptr,&textures[p]));Check(d3d->CreateShaderResourceView(textures[p].Get(),nullptr,&views[p]));
            ComPtr<IDXGIResource1> resource;Check(textures[p].As(&resource));HANDLE handle=nullptr;Check(resource->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle));
            VkImageCreateInfo vk{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};vk.imageType=VK_IMAGE_TYPE_2D;vk.format=VK_FORMAT_R8_UNORM;vk.extent={w,h,1};vk.mipLevels=1;vk.arrayLayers=1;vk.samples=VK_SAMPLE_COUNT_1_BIT;vk.tiling=VK_IMAGE_TILING_OPTIMAL;vk.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|(fragmentDecode?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:0);vk.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            pyrowave_image_create_info import{};import.device=device;import.external_handle=reinterpret_cast<pyrowave_os_handle>(handle);import.handle_type=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;import.image_create_info=&vk;
            pyrowave_result imported=pyrowave_image_create(&import,&images[p]);if(imported!=PYROWAVE_SUCCESS)fprintf(stderr,"Interop import failed plane=%d fragment=%d result=%d\n",p,int(fragmentDecode),int(imported));CheckPyro(imported);
            pyrowave_result viewed=pyrowave_image_get_image_view(images[p],VK_IMAGE_ASPECT_COLOR_BIT,fragmentDecode?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_STORAGE_BIT,&buffers.planes[p]);if(viewed!=PYROWAVE_SUCCESS)fprintf(stderr,"Interop view failed plane=%d fragment=%d result=%d\n",p,int(fragmentDecode),int(viewed));CheckPyro(viewed);
            external[p]={images[p],VK_QUEUE_FAMILY_EXTERNAL};
        }
        Check(d3d->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)));HANDLE handle=nullptr;Check(fence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle));
        pyrowave_sync_object_create_info si{};si.device=device;si.external_handle=reinterpret_cast<pyrowave_os_handle>(handle);si.handle_type=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;si.semaphore_type=VK_SEMAPHORE_TYPE_TIMELINE;
        CheckPyro(pyrowave_sync_object_create(&si,&sync));
        fprintf(stderr,"GPU interop: shared timeline fence ready\n");
    }
    void WatchRenderCompletion(StageFenceProbe&probe,uint32_t frame){Check(context->Signal(fence.Get(),++timeline));probe.Watch(fence.Get(),timeline,frame);}
    void Views(ID3D11ShaderResourceView** out){for(int p=0;p<3;++p)out[p]=views[p].Get();}
    bool DecodeAndDraw(Nv12Renderer&renderer,pyrowave_decoder decoder,StageGpuProbe*probe=nullptr,StageFenceProbe*fenceProbe=nullptr,uint32_t frame=0,uint64_t*t5Return=nullptr,uint64_t*t7=nullptr,uint64_t*t5Submit=nullptr){
        if(!Decode(decoder,probe,fenceProbe,frame,t5Return,t7,t5Submit))return false;
        ID3D11ShaderResourceView* planeViews[3];Views(planeViews);renderer.DrawGpuPlanes(planeViews);return true;
    }
    // Decodes into the shared planes; D3D11 work submitted afterwards may read them.
    bool Decode(pyrowave_decoder decoder,StageGpuProbe*probe=nullptr,StageFenceProbe*fenceProbe=nullptr,uint32_t frame=0,uint64_t*t5Return=nullptr,uint64_t*t7=nullptr,uint64_t*t5Submit=nullptr){
        Check(context->Signal(fence.Get(),++timeline));
        context->Flush(); // Submit the acquire signal before Vulkan waits on it.
        pyrowave_gpu_sync_operation acquire{},release{};
        acquire.images=external;acquire.num_images=3;acquire.sync={pyrowave_sync_object_get_semaphore(sync),timeline};
        release.images=external;release.num_images=3;release.sync={pyrowave_sync_object_get_semaphore(sync),++timeline};
        if(t5Submit)*t5Submit=StageUtcUs();
        if(pyrowave_decoder_decode_gpu_buffer(decoder,&acquire,&release,&buffers)!=PYROWAVE_SUCCESS)return false;
        if(t5Return)*t5Return=StageUtcUs();
        if(fenceProbe)fenceProbe->Watch(fence.Get(),timeline,frame);
        Check(context->Wait(fence.Get(),timeline));
        if(probe)probe->Decoded();
        if(t7)*t7=StageUtcUs();
        return true;
    }
};
