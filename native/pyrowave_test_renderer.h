#pragma once
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <cstring>
#include <wrl/client.h>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
#include "d3d_snapshot.h"
class Nv12Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    HANDLE frameLatencyReady=nullptr;
    UINT presentFlags=0;
    int inputWidth=2560,inputHeight=1440;
    float videoX=0,videoY=0,videoWidth=0,videoHeight=0;
    ComPtr<ID3D11VideoDevice> video;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> input, output;
    ComPtr<ID3D11RenderTargetView> frameTarget;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    ComPtr<IDXGISurface1> surface;
    int width=0,height=0;
    HWND owner=nullptr;
    ComPtr<ID3D11VertexShader> planeVertex;
    ComPtr<ID3D11PixelShader> planePixel;
    ComPtr<ID3D11SamplerState> planePoint,planeLinear;
    ComPtr<ID3D11Texture2D> hudTexture;
    ComPtr<ID3D11ShaderResourceView> hudView;
    ComPtr<ID3D11PixelShader> hudPixel;
    ComPtr<ID3D11BlendState> hudBlend;
    std::wstring hudText;
    int hudWidth=0,hudHeight=22;
    // Partial frames update regions of this persistent source-sized canvas.
    ComPtr<ID3D11Texture2D> canvas;
    ComPtr<ID3D11RenderTargetView> canvasTarget;
    ComPtr<ID3D11ShaderResourceView> canvasView;
    ComPtr<ID3D11PixelShader> canvasPixel;
    static void Check(HRESULT hr){if(FAILED(hr))throw hr;}
    void EnsurePlaneShaders(){
        if(planePixel)return;
        const char* shader="struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};V vs(uint i:SV_VertexID){V v;float2 p=float2((i<<1)&2,i&2);v.uv=p;v.p=float4(p*float2(2,-2)+float2(-1,1),0,1);return v;}Texture2D y:register(t0);Texture2D u:register(t1);Texture2D v:register(t2);SamplerState pointSample:register(s0);SamplerState linearSample:register(s1);float4 ps(V p):SV_Target{float Y=y.Sample(pointSample,p.uv).r;float U=u.Sample(linearSample,p.uv).r-128.0/255.0;float V=v.Sample(linearSample,p.uv).r-128.0/255.0;return float4(Y+1.5748*V,Y-0.187324*U-0.468124*V,Y+1.8556*U,1);}";
        ComPtr<ID3DBlob> vs,ps,error;
        Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error));
        Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error));
        Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&planeVertex));
        Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&planePixel));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&sd,&planePoint));sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;Check(device->CreateSamplerState(&sd,&planeLinear));
    }
public:
    ~Nv12Renderer(){if(frameLatencyReady)CloseHandle(frameLatencyReady);}
    std::wstring adapterName;
    LUID adapterLuid{};
    UINT adapterVendor=0;
    UINT panelVendor=0;
    unsigned long long dedicatedVideoBytes=0,sharedSystemBytes=0;
    ID3D11Device* Device(){return device.Get();}
    ID3D11DeviceContext* Context(){return context.Get();}
    const char* stage="factory";
    HANDLE FrameLatencyReady(){return frameLatencyReady;}
    UINT PresentFlags()const{return presentFlags;}
    void Init(HWND hwnd,int w,int h,UINT preferredVendor=0,bool waitablePresent=false,int sourceWidth=2560,int sourceHeight=1440,bool allowTearing=false){
        width=w;height=h;owner=hwnd;inputWidth=sourceWidth;inputHeight=sourceHeight;
        const float scale=std::min(float(w)/sourceWidth,float(h)/sourceHeight);
        videoWidth=sourceWidth*scale;videoHeight=sourceHeight*scale;videoX=(w-videoWidth)/2;videoY=(h-videoHeight)/2;
        ComPtr<IDXGIFactory1> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        if(allowTearing){ComPtr<IDXGIFactory5>feature;BOOL supported=FALSE;if(SUCCEEDED(factory.As(&feature))&&SUCCEEDED(feature->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&supported,sizeof(supported)))&&supported)presentFlags=DXGI_PRESENT_ALLOW_TEARING;}
        ComPtr<IDXGIAdapter1> chosen;
        const auto monitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTOPRIMARY);
        // A vendor preference that this machine cannot satisfy falls back to automatic selection.
        if(preferredVendor){
            bool present=false;
            for(UINT a=0;!present;++a){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(a,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};present=SUCCEEDED(candidate->GetDesc1(&d))&&d.VendorId==preferredVendor;}
            if(!present){fprintf(stderr,"Preferred adapter vendor %04x unavailable; using automatic selection\n",preferredVendor);preferredVendor=0;}
        }
        for(UINT a=0;;++a){
            ComPtr<IDXGIAdapter1> candidate;
            if(factory->EnumAdapters1(a,&candidate)==DXGI_ERROR_NOT_FOUND)break;
            // Prefer the requested GPU, otherwise match the current monitor.
            DXGI_ADAPTER_DESC1 candidateDesc{};candidate->GetDesc1(&candidateDesc);
            if(preferredVendor){if(candidateDesc.VendorId==preferredVendor){chosen=candidate;break;}continue;}
            if(!chosen && !(candidateDesc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))chosen=candidate;
            for(UINT o=0;;++o){
                ComPtr<IDXGIOutput> display;
                if(candidate->EnumOutputs(o,&display)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_OUTPUT_DESC desc{};if(SUCCEEDED(display->GetDesc(&desc))&&desc.Monitor==monitor){DXGI_ADAPTER_DESC1 ad{};candidate->GetDesc1(&ad);chosen=candidate;break;}
            }
        }
        if(!chosen)throw E_FAIL;
        DXGI_ADAPTER_DESC1 adapter{};Check(chosen->GetDesc1(&adapter));adapterName=adapter.Description;adapterLuid=adapter.AdapterLuid;adapterVendor=adapter.VendorId;
        DXGI_SWAP_CHAIN_DESC sc{};sc.BufferDesc.Width=w;sc.BufferDesc.Height=h;
        sc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sc.SampleDesc.Count=1;
#ifdef PYROSCREEN_TEST_RGBA_SWAPCHAIN_DEFAULT
        sc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
#endif
        fprintf(stderr,"Swapchain format: %u\n",unsigned(sc.BufferDesc.Format));
        sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sc.BufferCount=2;sc.OutputWindow=hwnd;
        sc.Windowed=TRUE;sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sc.Flags=waitablePresent?DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT:0;
        if(presentFlags)sc.Flags|=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        D3D_FEATURE_LEVEL level;
        stage="device_swapchain";Check(D3D11CreateDeviceAndSwapChain(chosen.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&sc,&swap,&device,&level,&context));
        // Desktop capture and PyroWave's scaler carry full-range SDR sRGB.
        // Declare that same space to DXGI; apply no brightness/gamma adjustment.
        ComPtr<IDXGISwapChain3> colorSwap;Check(swap.As(&colorSwap));
        UINT colorSupport=0;Check(colorSwap->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,&colorSupport));
        if(!(colorSupport&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))throw E_FAIL;
        Check(colorSwap->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
        ComPtr<IDXGIDevice> actualDevice;ComPtr<IDXGIAdapter> actualAdapter;DXGI_ADAPTER_DESC actualDesc{};
        Check(device.As(&actualDevice));Check(actualDevice->GetAdapter(&actualAdapter));Check(actualAdapter->GetDesc(&actualDesc));
        adapterName=actualDesc.Description;adapterVendor=actualDesc.VendorId;adapterLuid=actualDesc.AdapterLuid;
        dedicatedVideoBytes=actualDesc.DedicatedVideoMemory;sharedSystemBytes=actualDesc.SharedSystemMemory;
        for(UINT a=0;;++a){ComPtr<IDXGIAdapter1>candidate;if(factory->EnumAdapters1(a,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 ad{};if(FAILED(candidate->GetDesc1(&ad)))continue;for(UINT o=0;;++o){ComPtr<IDXGIOutput>display;if(candidate->EnumOutputs(o,&display)==DXGI_ERROR_NOT_FOUND)break;DXGI_OUTPUT_DESC desc{};if(SUCCEEDED(display->GetDesc(&desc))&&desc.AttachedToDesktop&&desc.Monitor==monitor){panelVendor=ad.VendorId;fwprintf(stderr,L"Panel output adapter: %ls, vendor %04x, display %ls\n",ad.Description,ad.VendorId,desc.DeviceName);}}}
        fprintf(stderr,"Render/panel vendors: %04x/%04x\n",adapterVendor,panelVendor);
        stage="video_interfaces";
        Check(device.As(&video));Check(context.As(&videoContext));
        Check(swap->GetBuffer(0,IID_PPV_ARGS(&output)));Check(output.As(&surface));
        Check(device->CreateRenderTargetView(output.Get(),nullptr,&frameTarget));
        D3D11_TEXTURE2D_DESC tex{};tex.Width=inputWidth;tex.Height=inputHeight;tex.MipLevels=1;tex.ArraySize=1;
        tex.Format=DXGI_FORMAT_NV12;tex.SampleDesc.Count=1;tex.Usage=D3D11_USAGE_DEFAULT;tex.BindFlags=0;
        stage="nv12_texture";Check(device->CreateTexture2D(&tex,nullptr,&input));
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate={165,1};content.OutputFrameRate={165,1};content.InputWidth=inputWidth;content.InputHeight=inputHeight;
        content.OutputWidth=w;content.OutputHeight=h;content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        stage="video_enumerator";Check(video->CreateVideoProcessorEnumerator(&content,&enumerator));stage="video_processor";Check(video->CreateVideoProcessor(enumerator.Get(),0,&processor));
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};iv.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
        stage="input_view";Check(video->CreateVideoProcessorInputView(input.Get(),enumerator.Get(),&iv,&inputView));
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};ov.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
        stage="output_view";Check(video->CreateVideoProcessorOutputView(output.Get(),enumerator.Get(),&ov,&outputView));
        RECT src{0,0,inputWidth,inputHeight},dst{LONG(videoX),LONG(videoY),LONG(videoX+videoWidth),LONG(videoY+videoHeight)};
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(),0,TRUE,&src);
        videoContext->VideoProcessorSetStreamDestRect(processor.Get(),0,TRUE,&dst);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(),TRUE,&dst);
        videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(),0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(),0,FALSE);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inColor{};inColor.YCbCr_Matrix=1;
        inColor.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outColor{};outColor.RGB_Range=0;
        videoContext->VideoProcessorSetStreamColorSpace(processor.Get(),0,&inColor);
        videoContext->VideoProcessorSetOutputColorSpace(processor.Get(),&outColor);
        factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);
        if(waitablePresent){
            ComPtr<IDXGISwapChain2> lowLatency;Check(swap.As(&lowLatency));Check(lowLatency->SetMaximumFrameLatency(1));
            frameLatencyReady=lowLatency->GetFrameLatencyWaitableObject();if(!frameLatencyReady)throw E_FAIL;
        }else{ComPtr<IDXGIDevice1> dxgi;if(SUCCEEDED(device.As(&dxgi)))Check(dxgi->SetMaximumFrameLatency(1));}
    }
    void Draw(const unsigned char* data){
        context->UpdateSubresource(input.Get(),0,nullptr,data,inputWidth,inputWidth*inputHeight*3/2);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=inputView.Get();
        Check(videoContext->VideoProcessorBlt(processor.Get(),outputView.Get(),0,1,&stream));
    }
    HDC GetDC(){HDC dc=nullptr;Check(surface->GetDC(FALSE,&dc));return dc;}
    void Present(){Check(surface->ReleaseDC(nullptr));Check(swap->Present(0,presentFlags));}
    void PresentVideo(){Check(swap->Present(0,presentFlags));}
    bool Snapshot(const wchar_t*path){return SaveD3dSnapshot(device.Get(),context.Get(),output.Get(),path);}
    UINT LastPresentId(){UINT id=0;swap->GetLastPresentCount(&id);return id;}
    bool DisplayStatistics(DXGI_FRAME_STATISTICS& stats){return SUCCEEDED(swap->GetFrameStatistics(&stats))&&stats.PresentCount&&stats.SyncQPCTime.QuadPart;}
    void UpdateOverlay(const std::wstring& text){
        if(text==hudText)return;hudText=text;
        HDC dc=CreateCompatibleDC(nullptr);HFONT font=CreateFontW(-15,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");HGDIOBJ oldFont=SelectObject(dc,font);SIZE size{};GetTextExtentPoint32W(dc,text.c_str(),int(text.size()),&size);hudWidth=size.cx+12;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=hudWidth;info.bmiHeader.biHeight=-hudHeight;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);HGDIOBJ oldBitmap=SelectObject(dc,bitmap);
        RECT rect{0,0,hudWidth,hudHeight};HBRUSH background=CreateSolidBrush(RGB(14,18,24));FillRect(dc,&rect,background);DeleteObject(background);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(245,248,255));rect.left=6;DrawTextW(dc,text.c_str(),-1,&rect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        auto data=static_cast<unsigned char*>(pixels);for(int i=0;i<hudWidth*hudHeight;++i){int brightest=std::max(int(data[4*i]),std::max(int(data[4*i+1]),int(data[4*i+2])));data[4*i+3]=static_cast<unsigned char>(180+std::max(0,brightest-24)*75/231);}
        hudView.Reset();hudTexture.Reset();D3D11_TEXTURE2D_DESC td{};td.Width=hudWidth;td.Height=hudHeight;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA init{pixels,UINT(hudWidth*4),0};HRESULT textureResult=device->CreateTexture2D(&td,&init,&hudTexture);
        SelectObject(dc,oldBitmap);SelectObject(dc,oldFont);DeleteObject(bitmap);DeleteObject(font);DeleteDC(dc);Check(textureResult);Check(device->CreateShaderResourceView(hudTexture.Get(),nullptr,&hudView));
    }
    void DrawOverlay(){
        if(!hudView)return;
        if(!hudPixel){
            const char* shader="struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};V vs(uint i:SV_VertexID){V v;float2 p=float2((i<<1)&2,i&2);v.uv=p;v.p=float4(p*float2(2,-2)+float2(-1,1),0,1);return v;}Texture2D hud:register(t0);SamplerState pointSample:register(s0);float4 ps(V p):SV_Target{return hud.Sample(pointSample,p.uv);}";
            ComPtr<ID3DBlob> blob,error;Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error));Check(device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&hudPixel));
            if(!planeVertex){Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error));Check(device->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&planeVertex));}
            if(!planePoint){D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;Check(device->CreateSamplerState(&sd,&planePoint));}
            D3D11_BLEND_DESC bd{};auto&rt=bd.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_SRC_ALPHA;rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOp=D3D11_BLEND_OP_ADD;rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;Check(device->CreateBlendState(&bd,&hudBlend));
        }
        context->OMSetRenderTargets(1,frameTarget.GetAddressOf(),nullptr);
        D3D11_VIEWPORT viewport{3,float(height-hudHeight-3),float(hudWidth),float(hudHeight),0,1};context->RSSetViewports(1,&viewport);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(planeVertex.Get(),nullptr,0);context->PSSetShader(hudPixel.Get(),nullptr,0);context->PSSetShaderResources(0,1,hudView.GetAddressOf());context->PSSetSamplers(0,1,planePoint.GetAddressOf());context->OMSetBlendState(hudBlend.Get(),nullptr,0xffffffff);context->Draw(3,0);context->OMSetBlendState(nullptr,nullptr,0xffffffff);ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(0,1,&empty);
    }
    // Converts decoded planes covering source rectangle (x,y,w,h) into the canvas.
    void DrawGpuPlanesToCanvas(ID3D11ShaderResourceView** planes,int x,int y,int w,int h){
        EnsurePlaneShaders();
        if(!canvas){
            D3D11_TEXTURE2D_DESC td{};td.Width=inputWidth;td.Height=inputHeight;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            Check(device->CreateTexture2D(&td,nullptr,&canvas));Check(device->CreateRenderTargetView(canvas.Get(),nullptr,&canvasTarget));Check(device->CreateShaderResourceView(canvas.Get(),nullptr,&canvasView));
        }
        D3D11_VIEWPORT viewport{float(x),float(y),float(w),float(h),0,1};context->RSSetViewports(1,&viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(planeVertex.Get(),nullptr,0);context->PSSetShader(planePixel.Get(),nullptr,0);
        context->OMSetRenderTargets(1,canvasTarget.GetAddressOf(),nullptr);context->PSSetShaderResources(0,3,planes);
        ID3D11SamplerState* samplers[]={planePoint.Get(),planeLinear.Get()};context->PSSetSamplers(0,2,samplers);context->Draw(3,0);
        ID3D11ShaderResourceView* empty[]={nullptr,nullptr,nullptr};context->PSSetShaderResources(0,3,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    // Presents the canvas with the same letterboxed placement as DrawGpuPlanes.
    void DrawCanvas(){
        if(!canvasView)return;
        EnsurePlaneShaders();
        if(!canvasPixel){
            const char* shader="struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};Texture2D c:register(t0);SamplerState s:register(s0);float4 ps(V p):SV_Target{return float4(c.Sample(s,p.uv).rgb,1);}";
            ComPtr<ID3DBlob> blob,error;Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error));Check(device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&canvasPixel));
        }
        const float black[4]={0,0,0,1};context->ClearRenderTargetView(frameTarget.Get(),black);
        D3D11_VIEWPORT viewport{videoX,videoY,videoWidth,videoHeight,0,1};context->RSSetViewports(1,&viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(planeVertex.Get(),nullptr,0);context->PSSetShader(canvasPixel.Get(),nullptr,0);
        context->OMSetRenderTargets(1,frameTarget.GetAddressOf(),nullptr);context->PSSetShaderResources(0,1,canvasView.GetAddressOf());
        const bool exact=int(videoWidth)==inputWidth&&int(videoHeight)==inputHeight;
        context->PSSetSamplers(0,1,exact?planePoint.GetAddressOf():planeLinear.GetAddressOf());context->Draw(3,0);
        ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(0,1,&empty);
    }
    void DrawGpuPlanes(ID3D11ShaderResourceView** planes){
        EnsurePlaneShaders();
        const float black[4]={0,0,0,1};context->ClearRenderTargetView(frameTarget.Get(),black);
        D3D11_VIEWPORT viewport{videoX,videoY,videoWidth,videoHeight,0,1};context->RSSetViewports(1,&viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(planeVertex.Get(),nullptr,0);context->PSSetShader(planePixel.Get(),nullptr,0);
        context->OMSetRenderTargets(1,frameTarget.GetAddressOf(),nullptr);context->PSSetShaderResources(0,3,planes);
        ID3D11SamplerState* samplers[]={planePoint.Get(),planeLinear.Get()};context->PSSetSamplers(0,2,samplers);context->Draw(3,0);
        ID3D11ShaderResourceView* empty[]={nullptr,nullptr,nullptr};context->PSSetShaderResources(0,3,empty);
    }
};

