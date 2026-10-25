from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "branding" / "paddrawboard-logo.png"
WINDOWS_ICON = ROOT / "desktop" / "resources" / "paddrawboard.ico"
ANDROID_RES = ROOT / "android" / "app" / "src" / "main" / "res"
PREVIEW = ROOT / "assets" / "branding" / "paddrawboard-app-icon.png"

DENSITIES = {
    "mdpi": 48,
    "hdpi": 72,
    "xhdpi": 96,
    "xxhdpi": 144,
    "xxxhdpi": 192,
}


def cropped_mark() -> Image.Image:
    image = Image.open(SOURCE).convert("RGBA")
    bounds = image.getchannel("A").getbbox()
    if bounds is None:
        raise RuntimeError(f"Logo has no visible pixels: {SOURCE}")
    return image.crop(bounds)


def place_mark(mark: Image.Image, size: int, coverage: float) -> Image.Image:
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    maximum = max(1, round(size * coverage))
    scaled = mark.copy()
    scaled.thumbnail((maximum, maximum), Image.Resampling.LANCZOS)
    position = ((size - scaled.width) // 2, (size - scaled.height) // 2)
    canvas.alpha_composite(scaled, position)
    return canvas


def legacy_icon(mark: Image.Image, size: int, round_icon: bool = False) -> Image.Image:
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    inset = max(1, round(size * 0.025))
    bounds = (inset, inset, size - inset - 1, size - inset - 1)
    if round_icon:
        draw.ellipse(bounds, fill="#F3F7FF")
    else:
        draw.rounded_rectangle(bounds, radius=round(size * 0.22), fill="#F3F7FF")
    foreground = place_mark(mark, size, 0.72)
    canvas.alpha_composite(foreground)
    return canvas


def main() -> None:
    mark = cropped_mark()

    WINDOWS_ICON.parent.mkdir(parents=True, exist_ok=True)
    windows = place_mark(mark, 1024, 0.88)
    windows.save(
        WINDOWS_ICON,
        format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
    )

    legacy_icon(mark, 512).save(PREVIEW, format="PNG", optimize=True)
    for density, launcher_size in DENSITIES.items():
        directory = ANDROID_RES / f"mipmap-{density}"
        directory.mkdir(parents=True, exist_ok=True)
        legacy_icon(mark, launcher_size).save(directory / "ic_launcher.png", optimize=True)
        legacy_icon(mark, launcher_size, round_icon=True).save(
            directory / "ic_launcher_round.png", optimize=True
        )
        adaptive_size = round(launcher_size * 2.25)
        place_mark(mark, adaptive_size, 0.66).save(
            directory / "ic_launcher_foreground.png", optimize=True
        )

    print(f"Windows icon: {WINDOWS_ICON}")
    print(f"Android icons: {ANDROID_RES}")


if __name__ == "__main__":
    main()
