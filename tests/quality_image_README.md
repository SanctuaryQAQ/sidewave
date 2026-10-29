# Offline real document comparison

`quality_image_compare.cpp` uses the supplied native **2560x1440** real project
document BMP. It never captures a screen or opens a display, network socket,
GUI, or remote session. The input must be an uncompressed 24-bit or 32-bit RGB
BMP; alpha is ignored, with RGB pixels preserved exactly.

Include `tests/quality_image_target.cmake` after the existing `pyrowave-shared`
target, then build `QualityImageCompare`. The fragment adds one excluded target
and checks the linked codec is configured with production precision 1
(`PYROWAVE_FP32_MATH=ON`, `PYROWAVE_FP32_STORAGE=OFF`). It uses existing project
libraries only. It does not change the production sender or receiver.

```powershell
cmake --build <existing-build-directory> --config Release --target QualityImageCompare
& '<build-directory>\Release\QualityImageCompare.exe' --source '<real-capture-directory>\source.bmp' --output '<new-output-directory>' --adapter 0 --warmup 1 --repetitions 3
python .\tests\quality_image_analyze.py '<new-output-directory>'
```

The executable needs the built `libpyrowave-shared-1.dll` beside it or on PATH,
as the existing sender does. `--adapter` is a DXGI adapter index; its LUID is
used to select the matching Vulkan device. The chosen adapter is recorded in
the manifest. An output directory must be new, preventing result overwrites.

All six conditions use `maximum_bitstream_size` of exactly 300000, 450000 or
600000 bytes and `scale.skip_dither=false` or `true`. The rate budget stays
fixed. Every condition creates fresh encoder/decoder objects with the same
sequence history, discards the configured warmup count, and saves **every**
measured repetition. Defaults produce 18 decoded BMPs. The production settings
are 4:4:4, sRGB full range, BT.709 full range, midpoint 128/255, R8 intermediate
planes, and precision 1. Source RGB is uploaded through the same shared BGRA
D3D11 texture / imported Vulkan image mechanism as the production GPU path;
external image acquire/release and a shared timeline fence protect reuse.

The packetizer uses the production 32768-byte codec packet boundary. This is a
packing target, and an indivisible wavelet block can exceed it. Output storage
is provisioned from the mapped raw-buffer allocation plus header headroom,
and every returned packet is checked against the actual output capacity.
Maximum packet size and the count exceeding the target are logged. Each complete
packet is fed to the decoder and `decode_is_ready(..., false)` must succeed.
The CPU-buffer API decodes to full-resolution Y, Cb, Cr R8 planes. BMP output
uses `R=Y+1.5748*(Cr-128)`, `B=Y+1.8556*(Cb-128)`, and the corresponding BT.709
green coefficients, rounded to the nearest byte and clipped to 0..255. No
limited-range rescaling or additional gamma transform is applied.

`manifest.json` defines the settings and measurement scope.
`measurements.jsonl` records actual packetized bytes, packet count, encode API
submission time, readback wait, complete encode time, packetization, packet
push, CPU-buffer decode, and RGB conversion. `complete.json` appears only after
all conditions succeed. The actual byte count sums codec packet sizes including
codec packet headers; it is distinct from the requested codec budget and
excludes UDP, encryption and FEC overhead. Mapped raw-buffer allocation size is
not reported as compressed size.

The analyzer uses the project's existing numpy/Pillow dependencies, checks all
conditions/repetitions, and writes `analysis.json` and `analysis_samples.csv`.
It compares every native RGB pixel (`excluded_pixels=0`), without registration,
cropping, smoothing, resizing or color adjustments. It reports global RGB
PSNR/MAE, channel bias, error percentiles, exact pixel fraction, and supplemental
edge/nonedge/gradient MAE. Edge membership comes only from the reference image;
both sides of each thresholded neighboring pixel pair are included. It stores
reference/output hashes and checks original RGB equality when the original
source file is still available. Aggregate PSNR comes from pooled MSE over all
same-size repetitions. No samples are selected for favorable scores.

`encode_complete_ms` includes waiting for the GPU result/readback, while
`encode_api_submit_ms` alone does not. `cpu_buffer_decode_ms` includes **GPU
decode and synchronous CPU readback**, excludes RGB conversion/BMP write, and
is not a CPU-only codec benchmark. Timings are observations for this offline
image workload. These results cannot establish live 165 Hz, transport latency,
capture-to-display latency, or end-to-end improvement.
