#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <gdiplus.h>
using Microsoft::WRL::ComPtr;
#include "d3d_snapshot.h"
static void Check(HRESULT hr) { if (FAILED(hr)) throw hr; }
static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) { if(m==WM_CLOSE){DestroyWindow(h);return 0;} if(m==WM_DESTROY){PostQuitMessage(0);return 0;} return DefWindowProcW(h,m,w,l); }
int wmain(int argc,wchar_t**argv) {
 if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))SetProcessDPIAware();
 if(argc<3)return 1;
 bool staticDocument=false,proveMotion=false;double runSeconds=45;std::wstring imagePath,snapshotPath;
 for(int i=3;i<argc;++i){if(!wcscmp(argv[i],L"--static"))staticDocument=true;else if(!wcscmp(argv[i],L"--prove-motion"))proveMotion=true;else if(!wcscmp(argv[i],L"--snapshot-file")&&i+1<argc)snapshotPath=argv[++i];else if(!wcscmp(argv[i],L"--image")&&i+1<argc)imagePath=argv[++i];else if(!wcscmp(argv[i],L"--seconds")&&i+1<argc)runSeconds=std::max(5,std::min(3600,_wtoi(argv[++i])));else return 1;}
 try {
  ComPtr<IDXGIFactory1> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;DXGI_OUTPUT_DESC display{};bool found=false;
  for(UINT a=0;!found;++a){ if(factory->EnumAdapters1(a,&adapter)==DXGI_ERROR_NOT_FOUND)break;
   for(UINT o=0;;++o){ComPtr<IDXGIOutput> output;if(adapter->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND)break;output->GetDesc(&display);if(display.AttachedToDesktop&&!wcscmp(display.DeviceName,argv[1])){found=true;break;}} }
  if(!found)return 2;
  WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DesktopGpuDocument";RegisterClassW(&wc);
  RECT r=display.DesktopCoordinates;int width=r.right-r.left,height=r.bottom-r.top;
  HWND window=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"Project source document",WS_POPUP,r.left,r.top,width,height,nullptr,nullptr,wc.hInstance,nullptr);
  ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
  DXGI_SWAP_CHAIN_DESC sc{};sc.BufferDesc.Width=width;sc.BufferDesc.Height=height;sc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sc.BufferCount=2;sc.OutputWindow=window;sc.Windowed=TRUE;sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
  Check(D3D11CreateDeviceAndSwapChain(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sc,&swap,&device,nullptr,&context));
  ComPtr<ID3D11Texture2D> back;ComPtr<ID3D11RenderTargetView> target;Check(swap->GetBuffer(0,IID_PPV_ARGS(&back)));Check(device->CreateRenderTargetView(back.Get(),nullptr,&target));
  std::ifstream source(argv[2]);std::string line;std::vector<std::wstring> lines;
  while(std::getline(source,line)){
   if(lines.empty()&&line.size()>=3&&static_cast<unsigned char>(line[0])==0xef&&static_cast<unsigned char>(line[1])==0xbb&&static_cast<unsigned char>(line[2])==0xbf)line.erase(0,3);
   if(!line.empty()&&line.back()=='\r')line.pop_back();
   int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,line.data(),int(line.size()),nullptr,0);
   UINT encoding=count?CP_UTF8:CP_ACP;
   if(!count)count=MultiByteToWideChar(encoding,0,line.data(),int(line.size()),nullptr,0);
   std::wstring text(count,L' ');if(count)MultiByteToWideChar(encoding,0,line.data(),int(line.size()),&text[0],count);
   lines.push_back(std::move(text));
  }
  if(lines.empty())throw HRESULT(E_INVALIDARG);
  // Keep the viewport over actual project text, including the last line.
  // A fixed 8192-row page created long blank intervals in shorter documents.
  const int documentHeight=std::max(height+28,std::min(16000,int(lines.size())*28+56));
  BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=width;bi.bmiHeader.biHeight=-documentHeight;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
  void* bits=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);HGDIOBJ oldBitmap=SelectObject(dc,bitmap);
  RECT page{0,0,width,documentHeight};FillRect(dc,&page,(HBRUSH)GetStockObject(WHITE_BRUSH));SetTextColor(dc,RGB(15,20,26));SetBkMode(dc,TRANSPARENT);
  HFONT font=CreateFontW(-22,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");HGDIOBJ oldFont=SelectObject(dc,font);
  int row=28;for(const auto& text:lines){if(row>=documentHeight-28)break;TextOutW(dc,24,row,text.c_str(),int(text.size()));row+=28;}
  if(!imagePath.empty()){
   Gdiplus::GdiplusStartupInput startup;ULONG_PTR token=0;
   if(Gdiplus::GdiplusStartup(&token,&startup,nullptr)!=Gdiplus::Ok)throw HRESULT(E_FAIL);
   bool imageOk=false;
   {Gdiplus::Bitmap picture(imagePath.c_str(),FALSE);Gdiplus::Graphics graphics(dc);graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);imageOk=picture.GetLastStatus()==Gdiplus::Ok&&graphics.DrawImage(&picture,0,0,width,height)==Gdiplus::Ok;}
   Gdiplus::GdiplusShutdown(token);if(!imageOk)throw HRESULT(E_FAIL);
  }
  D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=documentHeight;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{bits,UINT(width*4),0};ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;Check(device->CreateTexture2D(&td,&init,&texture));Check(device->CreateShaderResourceView(texture.Get(),nullptr,&view));
  SelectObject(dc,oldFont);SelectObject(dc,oldBitmap);DeleteObject(font);DeleteObject(bitmap);DeleteDC(dc);
  const char* shader="cbuffer Scroll:register(b0){float offset;float extent;float2 pad;}struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};V vs(uint i:SV_VertexID){V v;float2 p=float2((i<<1)&2,i&2);v.uv=p;v.p=float4(p*float2(2,-2)+float2(-1,1),0,1);return v;}Texture2D doc:register(t0);SamplerState pointSample:register(s0);float4 ps(V v):SV_Target{return float4(doc.Sample(pointSample,float2(v.uv.x,offset+v.uv.y*extent)).rgb,1);}";
  ComPtr<ID3DBlob> vsBlob,psBlob,error;Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vsBlob,&error));Check(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&psBlob,&error));
  ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;Check(device->CreateVertexShader(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),nullptr,&vs));Check(device->CreatePixelShader(psBlob->GetBufferPointer(),psBlob->GetBufferSize(),nullptr,&ps));
  D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> scroll;Check(device->CreateBuffer(&cb,nullptr,&scroll));
  D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;Check(device->CreateSamplerState(&sd,&sampler));
  D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->OMSetRenderTargets(1,target.GetAddressOf(),nullptr);context->PSSetShaderResources(0,1,view.GetAddressOf());context->PSSetSamplers(0,1,sampler.GetAddressOf());context->PSSetConstantBuffers(0,1,scroll.GetAddressOf());
  ComPtr<IDXGIDevice1> dxgi;if(SUCCEEDED(device.As(&dxgi)))dxgi->SetMaximumFrameLatency(1);
  ShowWindow(window,SW_SHOWNOACTIVATE);auto start=std::chrono::steady_clock::now();uint64_t frames=0;bool done=false;
  while(!done){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT)done=true;TranslateMessage(&message);DispatchMessageW(&message);}double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();if(elapsed>=runSeconds)break;
   float values[4]={staticDocument?0.0f:float((frames*2)%(documentHeight-height))/documentHeight,float(height)/documentHeight,0,0};context->UpdateSubresource(scroll.Get(),0,nullptr,values,0,0);context->OMSetRenderTargets(1,target.GetAddressOf(),nullptr);context->Draw(3,0);
   if(staticDocument&&frames==0&&!snapshotPath.empty())SaveD3dSnapshot(device.Get(),context.Get(),back.Get(),snapshotPath.c_str());
   if(proveMotion&&frames<2&&!snapshotPath.empty()){std::wstring path=snapshotPath+L"-"+std::to_wstring(frames)+L".bmp";SaveD3dSnapshot(device.Get(),context.Get(),back.Get(),path.c_str());}
   Check(swap->Present(1,0));++frames;
  }
  double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();printf("{\"workload\":\"actual_project_document_scroll\",\"frames\":%llu,\"elapsed_s\":%.3f,\"app_present_fps\":%.3f}\n",(unsigned long long)frames,seconds,frames/seconds);DestroyWindow(window);return 0;
 }catch(HRESULT hr){fprintf(stderr,"document workload failed 0x%08lx\n",(unsigned long)hr);return 3;}
}
