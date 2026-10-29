#pragma once
inline bool SaveD3dSnapshot(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture,const wchar_t* path){
 D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ComPtr<ID3D11Texture2D> staging;if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging)))return false;
 context->CopyResource(staging.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return false;
 HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);bool ok=file!=INVALID_HANDLE_VALUE;
 if(ok){BITMAPFILEHEADER header{};BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=desc.Width;info.biHeight=-LONG(desc.Height);info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=desc.Width*desc.Height*4;header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info);header.bfSize=header.bfOffBits+info.biSizeImage;DWORD written=0;
  ok=WriteFile(file,&header,sizeof(header),&written,nullptr)&&WriteFile(file,&info,sizeof(info),&written,nullptr);
  for(UINT row=0;ok&&row<desc.Height;++row)ok=WriteFile(file,static_cast<const unsigned char*>(mapped.pData)+row*mapped.RowPitch,desc.Width*4,&written,nullptr)&&written==desc.Width*4;
  CloseHandle(file);
 }
 context->Unmap(staging.Get(),0);return ok;
}
