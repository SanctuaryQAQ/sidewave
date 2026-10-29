// Offline comparison of one supplied real 2560x1440 document bitmap.
// Uses production D3D11 -> Vulkan GPU scaler/encoder, with CPU-buffer decode
// (the C API performs GPU decode plus readback). No desktop capture or transport.
#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#include <vulkan/vulkan.h>
#include "pyrowave.h"
#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
constexpr int Width = 2560, Height = 1440;
constexpr size_t PacketBoundary = 32768; // Production --packet-bytes; distinct from UDP fragments.
const std::vector<size_t> DefaultBudgets = {300000, 450000, 600000};

void Require(bool good, const char* what) { if (!good) throw std::runtime_error(what); }
void Check(pyrowave_result r, const char* what) {
    if (r != PYROWAVE_SUCCESS) throw std::runtime_error(std::string(what) + ": PyroWave " + std::to_string(int(r)));
}
void Hr(HRESULT r, const char* what) {
    if (FAILED(r)) {
        std::ostringstream msg; msg << what << ": HRESULT 0x" << std::hex << static_cast<unsigned long>(r);
        throw std::runtime_error(msg.str());
    }
}
double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b-a).count(); }
std::string Json(const std::string& value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << char(c);
    }
    out << '"'; return out.str();
}
std::string Utf8(const std::wstring& value) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
uint16_t U16(const std::vector<uint8_t>& v, size_t p) { return uint16_t(v[p]) | uint16_t(v[p+1]) << 8; }
uint32_t U32(const std::vector<uint8_t>& v, size_t p) {
    return uint32_t(v[p]) | uint32_t(v[p+1]) << 8 | uint32_t(v[p+2]) << 16 | uint32_t(v[p+3]) << 24;
}
void Put16(std::vector<uint8_t>& v, size_t p, uint16_t n) { for (size_t i=0; i<2; ++i) v[p+i] = uint8_t(n >> (8*i)); }
void Put32(std::vector<uint8_t>& v, size_t p, uint32_t n) { for (size_t i=0; i<4; ++i) v[p+i] = uint8_t(n >> (8*i)); }

// Native pixels only. Accept standard 24-bit or 32-bit BI_RGB BMP, either row orientation.
// The source is gamma-coded RGB; no color correction, filtering, resizing, alpha blend or crop.
std::vector<uint8_t> LoadBgra(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    Require(bool(in), "Cannot read source BMP");
    const auto length = in.tellg();
    Require(length >= 54 && length <= 64*1024*1024, "Source BMP size is invalid");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    in.seekg(0); in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    Require(bool(in), "Short source BMP read");
    Require(U16(bytes,0)==0x4d42 && U32(bytes,14)>=40, "Expected Windows BMP with BITMAPINFOHEADER");
    const auto offset = U32(bytes,10), header = U32(bytes,14);
    Require(uint64_t(14)+header<=bytes.size() && offset>=uint64_t(14)+header, "Invalid BMP pixel offset");
    const uint32_t rawHeight=U32(bytes,22);
    const auto w = static_cast<int32_t>(U32(bytes,18)), h = static_cast<int32_t>(rawHeight);
    // The sign bit explicitly records BMP storage order. Keep row selection
    // independent of signed-height range checks in optimized builds.
    const bool bottomUp=(rawHeight & 0x80000000u)==0;
    const auto bpp = U16(bytes,28);
    uint64_t sourceHash=14695981039346656037ull;
    for (uint8_t value : bytes) { sourceHash^=value; sourceHash*=1099511628211ull; }
    std::printf("Source BMP: path=%s size=%zu raw_fnv64=%016llx width=%d height=%d bpp=%u first_row=%s\n",
                Utf8(fs::canonical(path).wstring()).c_str(),bytes.size(),static_cast<unsigned long long>(sourceHash),
                int(w),int(h),unsigned(bpp),bottomUp?"bottom":"top");
    Require(w==Width && (h==Height || h==-Height), "Expected native 2560x1440 source image");
    Require(U16(bytes,26)==1 && (bpp==24 || bpp==32) && U32(bytes,30)==BI_RGB,
            "Expected uncompressed 24-bit or 32-bit RGB BMP");
    const size_t stride = ((size_t(w)*bpp+31)/32)*4;
    Require(uint64_t(offset)+uint64_t(stride)*Height<=bytes.size(), "BMP pixel data is truncated");
    std::vector<uint8_t> bgra(size_t(Width)*Height*4);
    for (int y=0; y<Height; ++y) {
        const size_t sourceRow=bottomUp ? size_t(Height-1-y) : size_t(y);
        const auto* row = bytes.data()+offset+sourceRow*stride;
        if (y==0 || y==Height-1)
            std::printf("BMP row map: output_y=%d input_row=%zu first_BGR=%u,%u,%u\n",
                        y,sourceRow,unsigned(row[0]),unsigned(row[1]),unsigned(row[2]));
        for (int x=0; x<Width; ++x) {
            auto* pixel = bgra.data()+(size_t(y)*Width+x)*4;
            for (int c=0; c<3; ++c) pixel[c]=row[size_t(x)*(bpp/8)+c];
            pixel[3]=255;
        }
    }
    return bgra;
}
void SaveBgra(const fs::path& path, const std::vector<uint8_t>& bgra) {
    Require(!fs::exists(path), "Refusing to overwrite an output BMP");
    const size_t stride=size_t(Width)*3;
    std::vector<uint8_t> bmp(54+stride*Height, 0);
    Put16(bmp,0,0x4d42); Put32(bmp,2,uint32_t(bmp.size())); Put32(bmp,10,54); Put32(bmp,14,40);
    Put32(bmp,18,Width); Put32(bmp,22,uint32_t(-Height)); Put16(bmp,26,1); Put16(bmp,28,24);
    Put32(bmp,34,uint32_t(stride*Height));
    for (size_t p=0; p<size_t(Width)*Height; ++p)
        for (size_t c=0; c<3; ++c) bmp[54+p*3+c]=bgra[p*4+c];
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(bmp.data()),std::streamsize(bmp.size()));
    Require(bool(out), "Cannot write output BMP");
}
uint8_t Byte(double value) { return uint8_t(std::clamp(std::lround(value), 0L, 255L)); }
std::vector<uint8_t> ToBgra(const std::array<std::vector<uint8_t>,3>& yuv) {
    std::vector<uint8_t> bgra(size_t(Width)*Height*4);
    // BT.709 full range. Y is 0..255; Cb/Cr zero at 128, as production scaler.
    // Work on gamma-coded samples directly; sRGB EOTF/OETF is not applied here.
    for (size_t i=0; i<yuv[0].size(); ++i) {
        double y=yuv[0][i], cb=double(yuv[1][i])-128.0, cr=double(yuv[2][i])-128.0;
        bgra[i*4+0]=Byte(y+1.8556*cb);
        bgra[i*4+1]=Byte(y-0.1873242729306488*cb-0.4681242729306488*cr);
        bgra[i*4+2]=Byte(y+1.5748*cr);
        bgra[i*4+3]=255;
    }
    return bgra;
}

struct Resources {
    pyrowave_device device=nullptr;
    pyrowave_image image=nullptr;
    pyrowave_sync_object sync=nullptr;
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device5> d3d5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11Fence> fence;
    HANDLE textureHandle=nullptr, fenceHandle=nullptr;
    uint64_t timeline=0;
    DXGI_ADAPTER_DESC1 adapterDesc{};
    pyrowave_image_view view{};
    ~Resources() {
        if (image) pyrowave_image_destroy(image);
        if (sync) pyrowave_sync_object_destroy(sync);
        if (device) pyrowave_device_destroy(device);
        if (textureHandle) CloseHandle(textureHandle);
        if (fenceHandle) CloseHandle(fenceHandle);
    }
    void Init(UINT adapterIndex) {
        ComPtr<IDXGIFactory1> factory; ComPtr<IDXGIAdapter1> adapter;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
        Hr(factory->EnumAdapters1(adapterIndex,&adapter),"Select DXGI adapter");
        Hr(adapter->GetDesc1(&adapterDesc),"Read adapter description");
        Require(!(adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE),"Software adapter is unsupported");
        Hr(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            nullptr,0,D3D11_SDK_VERSION,&d3d,nullptr,&context),"D3D11CreateDevice");
        Hr(d3d.As(&d3d5),"Get D3D11Device5"); Hr(context.As(&context4),"Get D3D11Context4");
        Check(pyrowave_create_device_by_compat(0,0,nullptr,nullptr,
              reinterpret_cast<const pyrowave_luid*>(&adapterDesc.AdapterLuid),VK_QUEUE_GLOBAL_PRIORITY_MEDIUM,&device),"Create matching Vulkan device");
        D3D11_TEXTURE2D_DESC td{};
        td.Width=Width; td.Height=Height; td.MipLevels=1; td.ArraySize=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT;
        td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        Hr(d3d->CreateTexture2D(&td,nullptr,&texture),"Create shared BGRA texture");
        ComPtr<IDXGIResource1> resource; Hr(texture.As(&resource),"Get DXGI shared resource");
        Hr(resource->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&textureHandle),"Share texture");
        Hr(d3d5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"Create shared D3D fence");
        Hr(fence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&fenceHandle),"Share fence");
        VkImageCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        vi.imageType=VK_IMAGE_TYPE_2D; vi.extent={Width,Height,1}; vi.format=VK_FORMAT_B8G8R8A8_UNORM;
        vi.mipLevels=1; vi.arrayLayers=1; vi.samples=VK_SAMPLE_COUNT_1_BIT; vi.tiling=VK_IMAGE_TILING_OPTIMAL;
        vi.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT; vi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        pyrowave_image_create_info ii{}; ii.device=device; ii.image_create_info=&vi;
        ii.handle_type=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        ii.external_handle=reinterpret_cast<pyrowave_os_handle>(textureHandle);
        Check(pyrowave_image_create(&ii,&image),"Import shared texture"); textureHandle=nullptr;
        pyrowave_sync_object_create_info si{}; si.device=device;
        si.handle_type=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
        si.semaphore_type=VK_SEMAPHORE_TYPE_TIMELINE;
        si.external_handle=reinterpret_cast<pyrowave_os_handle>(fenceHandle);
        Check(pyrowave_sync_object_create(&si,&sync),"Import shared timeline fence"); fenceHandle=nullptr;
        Check(pyrowave_image_get_image_view(image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_USAGE_SAMPLED_BIT,&view),"Get source view");
    }
};
struct Codec {
    pyrowave_encoder encoder=nullptr; pyrowave_decoder decoder=nullptr;
    ~Codec() { if (decoder) pyrowave_decoder_destroy(decoder); if (encoder) pyrowave_encoder_destroy(encoder); }
    void Init(pyrowave_device device, int width=Width, int height=Height) {
        pyrowave_encoder_create_info ei{}; ei.device=device; ei.width=width; ei.height=height;
        ei.chroma=PYROWAVE_CHROMA_SUBSAMPLING_444;
        Check(pyrowave_encoder_create(&ei,&encoder),"Create 444 encoder");
        pyrowave_decoder_create_info di{}; di.device=device; di.width=width; di.height=height;
        di.chroma=PYROWAVE_CHROMA_SUBSAMPLING_444; di.fragment_path=false;
        Check(pyrowave_decoder_create(&di,&decoder),"Create 444 decoder");
    }
};
struct Result {
    size_t packets=0, bytes=0, maximumPacketBytes=0, packetsOverBoundary=0;
    double submitMs=0, waitMs=0, encodeMs=0, packetizeMs=0, pushMs=0, decodeMs=0, rgbMs=0;
    double sliceEncodeMaxMs=0, sliceDecodeMaxMs=0;
    std::vector<uint8_t> bgra;
};
Result RoundTrip(Resources& r, Codec& codec, const std::vector<uint8_t>& source, size_t budget, bool skipDither) {
    Result result;
    if (r.timeline) Hr(r.context4->Wait(r.fence.Get(),r.timeline),"D3D wait for previous encoder release");
    r.context->UpdateSubresource(r.texture.Get(),0,nullptr,source.data(),Width*4,0);
    const uint64_t acquired=++r.timeline;
    Hr(r.context4->Signal(r.fence.Get(),acquired),"Signal source upload ready");
    r.context->Flush();
    pyrowave_gpu_external_reference ref{r.image,VK_QUEUE_FAMILY_EXTERNAL};
    pyrowave_gpu_sync_operation acquire{},release{};
    acquire.images=&ref; acquire.num_images=1;
    release.images=&ref; release.num_images=1;
    acquire.sync={pyrowave_sync_object_get_semaphore(r.sync),acquired};
    release.sync={pyrowave_sync_object_get_semaphore(r.sync),++r.timeline};
    pyrowave_scaled_encode_info scale{};
    scale.view=r.view; scale.input_color_space=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scale.output_color_space=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scale.intermediate_plane_format=VK_FORMAT_R8_UNORM; scale.ycbcr_chroma_midpoint=128.0f/255.0f;
    scale.skip_dither=skipDither;
    const pyrowave_rate_control rate{budget}; // Immutable throughout this condition; no network adaptation.
    const auto start=Clock::now();
    Check(pyrowave_encoder_encode_gpu_scaled(codec.encoder,&acquire,&release,&scale,&rate),"GPU scaled encode");
    const auto submitted=Clock::now();
    const void* raw=nullptr; const void* metadata=nullptr; size_t rawSize=0, metadataSize=0;
    Check(pyrowave_encoder_get_mapped_raw_bitstream(codec.encoder,&raw,&rawSize,&metadata,&metadataSize),"Wait encoded readback");
    const auto complete=Clock::now();
    result.submitMs=Ms(start,submitted); result.waitMs=Ms(submitted,complete); result.encodeMs=Ms(start,complete);
    size_t count=0;
    Check(pyrowave_encoder_compute_num_packets(codec.encoder,PacketBoundary,&count),"Compute packet count");
    Require(count>0 && count<=std::numeric_limits<size_t>::max()/sizeof(pyrowave_packet),"Invalid packet count");
    std::vector<pyrowave_packet> packets(count);
    // packet_boundary is a packing target: one indivisible wavelet block can
    // exceed it. The mapped buffer holds all encoded blocks; provision its
    // allocation plus generous sequence-header headroom BEFORE packetize,
    // whose upstream implementation only asserts output capacity in Debug.
    // This allocation size is never used as the measured compressed size.
    Require(raw && metadata && rawSize>0 && rawSize<=std::numeric_limits<size_t>::max()-65536,
            "Invalid mapped encoder buffers");
    std::vector<uint8_t> bitstream(rawSize+65536);
    const size_t capacity=count;
    Check(pyrowave_encoder_packetize(codec.encoder,packets.data(),PacketBoundary,&count,bitstream.data(),bitstream.size()),"Packetize");
    Require(count>0 && count<=capacity,"Packetization returned invalid packet count");
    for (size_t i=0; i<count; ++i) {
        const auto& packet=packets[i];
        if (!packet.size || packet.offset>bitstream.size() || packet.size>bitstream.size()-packet.offset) {
            std::ostringstream message;
            message << "Invalid encoded packet bounds: packet=" << i << " offset=" << packet.offset
                    << " size=" << packet.size << " output_capacity=" << bitstream.size()
                    << " raw_allocation=" << rawSize << " packet_boundary=" << PacketBoundary;
            throw std::runtime_error(message.str());
        }
        result.bytes+=packet.size;
        result.maximumPacketBytes=std::max(result.maximumPacketBytes,packet.size);
        if (packet.size>PacketBoundary) ++result.packetsOverBoundary;
    }
    result.packets=count; const auto packetized=Clock::now(); result.packetizeMs=Ms(complete,packetized);
    pyrowave_decoder_clear(codec.decoder);
    const auto pushStart=Clock::now();
    for (size_t i=0; i<count; ++i)
        Check(pyrowave_decoder_push_packet(codec.decoder,bitstream.data()+packets[i].offset,packets[i].size),"Push encoded packet");
    Require(pyrowave_decoder_decode_is_ready(codec.decoder,false),"All packets did not produce a complete frame");
    result.pushMs=Ms(pushStart,Clock::now());
    std::array<std::vector<uint8_t>,3> yuv;
    pyrowave_cpu_buffer buffer{}; buffer.width=Width; buffer.height=Height; buffer.format=PYROWAVE_CPU_BUFFER_FORMAT_YUV444P;
    for (int i=0; i<3; ++i) {
        yuv[i].resize(size_t(Width)*Height); buffer.data[i]=yuv[i].data();
        buffer.row_stride_in_bytes[i]=Width; buffer.plane_size_in_bytes[i]=yuv[i].size();
    }
    const auto decodeStart=Clock::now();
    Check(pyrowave_decoder_decode_cpu_buffer_synchronous(codec.decoder,&buffer),"Decode to CPU YUV444 buffers");
    const auto decoded=Clock::now(); result.decodeMs=Ms(decodeStart,decoded);
    result.bgra=ToBgra(yuv); result.rgbMs=Ms(decoded,Clock::now());
    return result;
}
// Horizontal-slice experiment: each band is an independent PyroWave image
// cropped 1:1 from the same source with budget/N bytes, decoded separately and
// reassembled. Times are summed (serial cost) and the slowest slice is kept,
// which bounds what an encode/send/decode pipeline could overlap.
Result RoundTripSliced(Resources& r, std::vector<std::unique_ptr<Codec>>& codecs, const std::vector<uint8_t>& source, size_t budget, bool skipDither) {
    Result result;
    const int slices=int(codecs.size());
    std::array<std::vector<uint8_t>,3> yuv;
    for (auto& plane : yuv) plane.assign(size_t(Width)*Height,0);
    if (r.timeline) Hr(r.context4->Wait(r.fence.Get(),r.timeline),"D3D wait for previous encoder release");
    r.context->UpdateSubresource(r.texture.Get(),0,nullptr,source.data(),Width*4,0);
    for (int s=0; s<slices; ++s) {
        const int top=Height*s/slices, height=Height*(s+1)/slices-top;
        Codec& codec=*codecs[s];
        const uint64_t acquired=++r.timeline;
        Hr(r.context4->Signal(r.fence.Get(),acquired),"Signal source upload ready");
        r.context->Flush();
        pyrowave_gpu_external_reference ref{r.image,VK_QUEUE_FAMILY_EXTERNAL};
        pyrowave_gpu_sync_operation acquire{},release{};
        acquire.images=&ref; acquire.num_images=1; release.images=&ref; release.num_images=1;
        acquire.sync={pyrowave_sync_object_get_semaphore(r.sync),acquired};
        release.sync={pyrowave_sync_object_get_semaphore(r.sync),++r.timeline};
        const VkRect2D crop{{0,top},{uint32_t(Width),uint32_t(height)}};
        pyrowave_scaled_encode_info scale{};
        scale.view=r.view; scale.input_color_space=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        scale.output_color_space=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        scale.intermediate_plane_format=VK_FORMAT_R8_UNORM; scale.ycbcr_chroma_midpoint=128.0f/255.0f;
        scale.skip_dither=skipDither; scale.crop_rect=&crop;
        const pyrowave_rate_control rate{budget/size_t(slices)};
        const auto start=Clock::now();
        Check(pyrowave_encoder_encode_gpu_scaled(codec.encoder,&acquire,&release,&scale,&rate),"GPU sliced encode");
        const void* raw=nullptr; const void* metadata=nullptr; size_t rawSize=0, metadataSize=0;
        Check(pyrowave_encoder_get_mapped_raw_bitstream(codec.encoder,&raw,&rawSize,&metadata,&metadataSize),"Wait encoded readback");
        const double encodeMs=Ms(start,Clock::now());
        size_t count=0;
        Check(pyrowave_encoder_compute_num_packets(codec.encoder,PacketBoundary,&count),"Compute packet count");
        std::vector<pyrowave_packet> packets(count);
        std::vector<uint8_t> bitstream(rawSize+65536);
        Check(pyrowave_encoder_packetize(codec.encoder,packets.data(),PacketBoundary,&count,bitstream.data(),bitstream.size()),"Packetize");
        pyrowave_decoder_clear(codec.decoder);
        for (size_t i=0; i<count; ++i) {
            result.bytes+=packets[i].size; result.maximumPacketBytes=std::max(result.maximumPacketBytes,packets[i].size);
            if (packets[i].size>PacketBoundary) ++result.packetsOverBoundary;
            Check(pyrowave_decoder_push_packet(codec.decoder,bitstream.data()+packets[i].offset,packets[i].size),"Push encoded packet");
        }
        result.packets+=count;
        Require(pyrowave_decoder_decode_is_ready(codec.decoder,false),"Slice packets did not produce a complete frame");
        pyrowave_cpu_buffer buffer{}; buffer.width=Width; buffer.height=height; buffer.format=PYROWAVE_CPU_BUFFER_FORMAT_YUV444P;
        for (int i=0; i<3; ++i) {
            buffer.data[i]=yuv[i].data()+size_t(top)*Width;
            buffer.row_stride_in_bytes[i]=Width; buffer.plane_size_in_bytes[i]=size_t(Width)*height;
        }
        const auto decodeStart=Clock::now();
        Check(pyrowave_decoder_decode_cpu_buffer_synchronous(codec.decoder,&buffer),"Decode slice to CPU YUV444 buffers");
        const double decodeMs=Ms(decodeStart,Clock::now());
        result.encodeMs+=encodeMs; result.decodeMs+=decodeMs;
        result.sliceEncodeMaxMs=std::max(result.sliceEncodeMaxMs,encodeMs); result.sliceDecodeMaxMs=std::max(result.sliceDecodeMaxMs,decodeMs);
    }
    const auto rgbStart=Clock::now(); result.bgra=ToBgra(yuv); result.rgbMs=Ms(rgbStart,Clock::now());
    return result;
}
int Integer(const std::wstring& text, int minimum, int maximum) {
    size_t end=0; const int value=std::stoi(text,&end);
    Require(end==text.size() && value>=minimum && value<=maximum,"Numeric option outside allowed range"); return value;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        fs::path sourcePath, outputPath; int warmup=1, repetitions=3; UINT adapter=0;
        bool bmpRoundtripOnly=false;
        std::vector<size_t> budgets=DefaultBudgets; std::vector<bool> ditherValues={false,true}; int slices=1;
        for (int i=1; i<argc; ++i) {
            const std::wstring key=argv[i];
            if (key==L"--bmp-roundtrip-only") { bmpRoundtripOnly=true; continue; }
            if (key==L"--help") {
                std::puts("QualityImageCompare --source source.bmp --output NEW_DIRECTORY [--adapter 0] [--warmup 1] [--repetitions 3] [--budgets 300000,450000,600000] [--skip-dither both|false|true] [--slices 1..8] [--bmp-roundtrip-only]\n"
                          "Native 2560x1440 RGB BMP; default 300000/450000/600000 codec budgets; both skip_dither values; precision 1.\n"
                          "--bmp-roundtrip-only saves/reloads the reference and checks exact BGRA identity without initializing a GPU.\n"
                          "CPU-buffer decode includes GPU decode/readback. Offline image results do not establish live 165 Hz or display latency.");
                return 0;
            }
            Require(i+1<argc,"Option requires a value"); const std::wstring value=argv[++i];
            if (key==L"--source") sourcePath=value;
            else if (key==L"--output") outputPath=value;
            else if (key==L"--adapter") adapter=UINT(Integer(value,0,255));
            else if (key==L"--warmup") warmup=Integer(value,0,100);
            else if (key==L"--repetitions") repetitions=Integer(value,1,100);
            else if (key==L"--budgets") {
                budgets.clear(); std::wstringstream list(value); std::wstring item;
                while (std::getline(list,item,L',')) budgets.push_back(size_t(Integer(item,16384,8*1048576)));
                Require(!budgets.empty(),"--budgets requires at least one value");
            }
            else if (key==L"--slices") slices=Integer(value,1,8);
            else if (key==L"--skip-dither") {
                if (value==L"both") ditherValues={false,true};
                else if (value==L"false") ditherValues={false};
                else if (value==L"true") ditherValues={true};
                else throw std::runtime_error("--skip-dither must be both, false or true");
            }
            else throw std::runtime_error("Unknown option: "+Utf8(key));
        }
        Require(!sourcePath.empty() && !outputPath.empty(),"--source and --output are required; see --help");
        sourcePath=fs::canonical(sourcePath); outputPath=fs::absolute(outputPath);
        const auto source=LoadBgra(sourcePath);
        Require(!fs::exists(outputPath),"Output directory must be new to preserve prior results");
        if (bmpRoundtripOnly) {
            Require(fs::create_directories(outputPath),"Cannot create output directory");
            SaveBgra(outputPath/L"source.bmp",source);
            Require(LoadBgra(outputPath/L"source.bmp")==source,"Saved reference changed source RGB");
            std::puts("BMP-only saved reference reload: exact BGRA match");
            return 0;
        }
        Resources resources; resources.Init(adapter);
        Require(fs::create_directories(outputPath),"Cannot create output directory");
        SaveBgra(outputPath/L"source.bmp",source);
        std::ofstream manifest(outputPath/L"manifest.json");
        manifest << "{\n  \"source_path\": " << Json(Utf8(sourcePath.wstring()))
                 << ",\n  \"reference\": \"source.bmp\",\n  \"width\": 2560, \"height\": 1440,\n"
                 << "  \"chroma\": \"444\", \"input_color_space\": \"sRGB_full\", \"output_color_space\": \"sRGB_full\",\n"
                 << "  \"ycbcr_matrix\": \"BT709\", \"ycbcr_range\": \"full\", \"chroma_midpoint\": \"128/255\",\n"
                 << "  \"intermediate_plane_format\": \"R8_UNORM\", \"precision\": 1, \"packet_boundary_bytes\": " << PacketBoundary << ",\n"
                 << "  \"adapter_index\": " << adapter << ", \"adapter_vendor\": " << resources.adapterDesc.VendorId
                 << ", \"adapter_device\": " << resources.adapterDesc.DeviceId << ",\n  \"adapter_description\": " << Json(Utf8(resources.adapterDesc.Description))
                 << ",\n  \"warmup_per_condition\": " << warmup << ", \"repetitions_per_condition\": " << repetitions << ",\n"
                 << "  \"budgets\": [";
        for (size_t i=0; i<budgets.size(); ++i) manifest << (i?",":"") << budgets[i];
        manifest << "], \"skip_dither_values\": [";
        for (size_t i=0; i<ditherValues.size(); ++i) manifest << (i?",":"") << (ditherValues[i]?"true":"false");
        manifest << "], \"slices\": " << slices << ",\n"
                 << "  \"actual_bitstream_bytes_definition\": \"sum of packet.size, including PyroWave packet headers; no UDP/encryption/FEC\",\n"
                 << "  \"encode_complete_ms_definition\": \"GPU scaled encode submission through mapped encoded readback ready\",\n"
                 << "  \"cpu_buffer_decode_ms_definition\": \"GPU decoder plus synchronous YUV444 R8 CPU readback, excludes push/RGB conversion/BMP write\",\n"
                 << "  \"scope\": \"one static supplied real document; offline image comparison; no live 165 Hz, network, capture, or display evidence\"\n}\n";
        manifest.flush(); Require(bool(manifest),"Cannot write manifest");
        std::ofstream rows(outputPath/L"measurements.jsonl"); Require(bool(rows),"Cannot create measurements log");
        rows << std::fixed << std::setprecision(6);
        for (size_t budget : budgets) for (bool skipDither : ditherValues) {
            Codec codec; codec.Init(resources.device); // Same fresh sequence history for both dither conditions.
            std::vector<std::unique_ptr<Codec>> sliceCodecs;
            for (int s=0; slices>1 && s<slices; ++s) {
                sliceCodecs.emplace_back(new Codec());
                sliceCodecs.back()->Init(resources.device,Width,Height*(s+1)/slices-Height*s/slices);
            }
            for (int rep=-warmup; rep<repetitions; ++rep) {
                auto result=slices>1?RoundTripSliced(resources,sliceCodecs,source,budget,skipDither):RoundTrip(resources,codec,source,budget,skipDither);
                if (rep<0) continue;
                const std::string name="decoded_budget_"+std::to_string(budget)+"_skip_dither_"+(skipDither?"true":"false")+"_rep_"+std::to_string(rep)+".bmp";
                SaveBgra(outputPath/fs::path(name),result.bgra);
                rows << "{\"budget_bytes\":" << budget << ",\"skip_dither\":" << (skipDither?"true":"false")
                     << ",\"repetition\":" << rep << ",\"bitmap\":" << Json(name)
                     << ",\"actual_bitstream_bytes\":" << result.bytes << ",\"packet_count\":" << result.packets
                     << ",\"maximum_packet_bytes\":" << result.maximumPacketBytes << ",\"packets_over_boundary\":" << result.packetsOverBoundary
                     << ",\"encode_api_submit_ms\":" << result.submitMs << ",\"encode_wait_readback_ms\":" << result.waitMs
                     << ",\"encode_complete_ms\":" << result.encodeMs << ",\"packetize_ms\":" << result.packetizeMs
                     << ",\"decoder_push_ms\":" << result.pushMs << ",\"cpu_buffer_decode_ms\":" << result.decodeMs
                     << ",\"yuv_to_rgb_ms\":" << result.rgbMs << ",\"slices\":" << slices
                     << ",\"slice_encode_max_ms\":" << result.sliceEncodeMaxMs << ",\"slice_decode_max_ms\":" << result.sliceDecodeMaxMs << "}\n";
                rows.flush(); Require(bool(rows),"Cannot write measurements log");
                std::printf("budget=%zu skip_dither=%s rep=%d bytes=%zu encode_complete_ms=%.3f cpu_buffer_decode_ms=%.3f\n",
                            budget,skipDither?"true":"false",rep,result.bytes,result.encodeMs,result.decodeMs);
            }
        }
        std::ofstream done(outputPath/L"complete.json"); done << "{\"complete\":true,\"measured_frames\":" << budgets.size()*ditherValues.size()*repetitions << "}\n";
        done.flush(); Require(bool(done),"Cannot write completion marker");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"QualityImageCompare: %s\n",e.what()); return 1; }
}
