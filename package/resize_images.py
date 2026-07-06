from pathlib import Path
from PIL import Image
files = [
    ("package/Assets/Square44x44Logo.png", (44, 44)),
    ("package/Assets/Square150x150Logo.png", (150, 150)),
    ("package/Assets/Wide310x150Logo.png", (310, 150)),
    ("package/Assets/SplashScreen.png", (620, 300)),
]
for path, size in files:
    p = Path(path)
    if not p.exists():
        print(f"MISSING {path}")
        continue
    with Image.open(p) as im:
        print(f"{path}: {im.size}")
        if im.size != size:
            im2 = im.resize(size, Image.LANCZOS)
            im2.save(p)
            print(f"  resized to {size}")
