import json
import sys
from pathlib import Path
import numpy as np
from PIL import Image

folder = Path(sys.argv[1])
source = np.array(Image.open(folder / 'source.bmp').convert('RGB'), dtype=np.float32)
received = np.array(Image.open(folder / 'receiver.bmp').convert('RGB'), dtype=np.float32)
if source.shape != received.shape or source.shape[:2] != (1440, 2560):
    raise ValueError('Expected equal native 2560x1440 RGB images')
error = received - source
mse = float(np.mean(error * error))
edges = np.zeros(source.shape[:2], dtype=bool)
edges[:, 1:] |= np.max(np.abs(np.diff(source, axis=1)), axis=2) > 12
edges[1:, :] |= np.max(np.abs(np.diff(source, axis=0)), axis=2) > 12
dark = np.all(source <= 30, axis=2)
white = np.all(source >= 240, axis=2)
result = {
    'width': 2560, 'height': 1440, 'excludedPixels': 0,
    'mae': float(np.mean(np.abs(error))),
    'psnrDb': float(10 * np.log10(255**2 / mse)) if mse else None,
    'absoluteErrorP95': float(np.percentile(np.abs(error), 95)),
    'edgeMae': float(np.mean(np.abs(error[edges]))),
    'signedBiasRGB': np.mean(error, axis=(0, 1)).tolist(),
    'darkMedianReceivedRGB': np.median(received[dark], axis=0).tolist() if dark.any() else None,
    'whiteMedianReceivedRGB': np.median(received[white], axis=0).tolist() if white.any() else None,
    'scope': 'One static real project document, complete native-renderer RGB snapshot; not general image-quality acceptance'
}
(folder / 'comparison.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, ensure_ascii=False))
