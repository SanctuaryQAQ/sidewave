#pragma once
#include <d3dcompiler.h>
#pragma comment(lib,"d3dcompiler.lib")
// Compose DXGI's separately reported pointer on the GPU. The transmitted
// desktop consequently has exactly one host cursor; no full-frame CPU readback.
class DesktopPointer {
 template<class T>using ComPtr=Microsoft::WRL::ComPtr<T>;
 ComPtr<ID3D11Texture2D> shape,background;
 ComPtr<ID3D11ShaderResourceView> shapeView,backgroundView;
 ComPtr<ID3D11RenderTargetView> target;
 ComPtr<ID3D11VertexShader> vertex;
 ComPtr<ID3D11PixelShader> pixel;
 ComPtr<ID3D11Buffer> constants;
 std::vector<unsigned char> bytes;
 DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
 DXGI_OUTDUPL_POINTER_POSITION position{};
 static void Check(HRESULT r){if(FAILED(r))throw std::runtime_error("Desktop pointer composition failed");}
 void Init(ID3D11Device*device,ID3D11Texture2D*texture){
  Check(device->CreateRenderTargetView(texture,nullptr,&target));
  const char*code=
   "cbuffer C:register(b0){uint type;uint w;uint h;uint pad;}"
   "struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
   "V vs(uint i:SV_VertexID){V v;float2 p=float2((i<<1)&2,i&2);v.uv=p;v.p=float4(p*float2(2,-2)+float2(-1,1),0,1);return v;}"
   "Texture2D b:register(t0);Texture2D c:register(t1);"
   "float4 ps(V p):SV_Target{int3 loc=int3(int2(p.uv*float2(w,h)),0);float4 B=b.Load(loc),C=c.Load(loc);"
   "uint3 bi=uint3(round(B.rgb*255)),ci=uint3(round(C.rgb*255));"
   "if(type==1){uint A=uint(round(C.r*255)),X=uint(round(C.g*255));return float4(float3((bi&A)^X)/255,1);}"
   "if(type==4)return float4(C.a>.5?float3(bi^ci)/255:C.rgb,1);"
   "return float4(C.rgb*C.a+B.rgb*(1-C.a),1);}";
  ComPtr<ID3DBlob>vs,ps,error;
  Check(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error));
  Check(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error));
  Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));
  Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel));
  D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
  Check(device->CreateBuffer(&cb,nullptr,&constants));
 }
public:
 // Screen-space rectangle last composed by Draw, clipped to the output.
 bool Bounds(int width,int height,RECT&r)const{
  if(!position.Visible||!info.Width||!info.Height)return false;
  const int w=int(info.Width),h=info.Type==DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME?int(info.Height/2):int(info.Height);
  const int x=position.Position.x,y=position.Position.y;
  r={std::max(0,x),std::max(0,y),std::min(width,x+w),std::min(height,y+h)};
  return r.right>r.left&&r.bottom>r.top;
 }
 void Draw(ID3D11Device*device,ID3D11DeviceContext*context,IDXGIOutputDuplication*dup,
           ID3D11Texture2D*texture,const DXGI_OUTDUPL_FRAME_INFO&frame,int width,int height){
  if(frame.LastMouseUpdateTime.QuadPart)position=frame.PointerPosition;
  if(!position.Visible)return;
  if(frame.PointerShapeBufferSize||bytes.empty()){
   bytes.resize(std::max<UINT>(frame.PointerShapeBufferSize,1024*1024));
   if(bytes.size()>4*1024*1024)throw std::runtime_error("Invalid pointer shape size");
   UINT required=0;Check(dup->GetFramePointerShape(UINT(bytes.size()),bytes.data(),&required,&info));
   if(!info.Width||!info.Height||info.Width>512||info.Height>1024)throw std::runtime_error("Unsupported pointer dimensions");
   const UINT h=info.Type==DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME?info.Height/2:info.Height;
   std::vector<unsigned char> rgba(size_t(info.Width)*h*4);
   if(info.Type==DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME){
    for(UINT y=0;y<h;++y)for(UINT x=0;x<info.Width;++x){
     size_t at=(size_t(y)*info.Width+x)*4;UINT mask=0x80>>(x%8);
     rgba[at+2]=(bytes[y*info.Pitch+x/8]&mask)?255:0;
     rgba[at+1]=(bytes[(y+h)*info.Pitch+x/8]&mask)?255:0;rgba[at+3]=255;
    }
   }else for(UINT y=0;y<h;++y)std::memcpy(rgba.data()+size_t(y)*info.Width*4,bytes.data()+size_t(y)*info.Pitch,size_t(info.Width)*4);
   D3D11_TEXTURE2D_DESC td{};td.Width=info.Width;td.Height=h;td.MipLevels=td.ArraySize=1;
   td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
   shape.Reset();shapeView.Reset();background.Reset();backgroundView.Reset();
   D3D11_SUBRESOURCE_DATA sd{rgba.data(),info.Width*4,0};
   Check(device->CreateTexture2D(&td,&sd,&shape));Check(device->CreateShaderResourceView(shape.Get(),nullptr,&shapeView));
   Check(device->CreateTexture2D(&td,nullptr,&background));Check(device->CreateShaderResourceView(background.Get(),nullptr,&backgroundView));
  }
  if(!vertex)Init(device,texture);
  const int w=int(info.Width),h=info.Type==DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME?int(info.Height/2):int(info.Height);
  const int x=position.Position.x,y=position.Position.y;
  int left=std::max(0,x),top=std::max(0,y),right=std::min(width,x+w),bottom=std::min(height,y+h);
  if(right<=left||bottom<=top)return;
  D3D11_BOX box{UINT(left),UINT(top),0,UINT(right),UINT(bottom),1};
  context->CopySubresourceRegion(background.Get(),0,UINT(left-x),UINT(top-y),0,texture,0,&box);
  UINT values[4]={info.Type,UINT(w),UINT(h),0};context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);
  D3D11_VIEWPORT viewport{float(x),float(y),float(w),float(h),0,1};context->RSSetViewports(1,&viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
  context->OMSetRenderTargets(1,target.GetAddressOf(),nullptr);context->PSSetConstantBuffers(0,1,constants.GetAddressOf());
  ID3D11ShaderResourceView*views[2]={backgroundView.Get(),shapeView.Get()};context->PSSetShaderResources(0,2,views);context->Draw(3,0);
  ID3D11ShaderResourceView*empty[2]={nullptr,nullptr};context->PSSetShaderResources(0,2,empty);context->OMSetRenderTargets(0,nullptr,nullptr);
 }
};
