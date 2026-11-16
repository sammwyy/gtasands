"""Intro videos (.mpg) -> DVID: paletted 256x192 frames + interleaved PCM16 audio.

DVID layout:
    u32 magic 'DVID', u16 version, u16 w, u16 h, u16 fps, u16 reserved
    u32 numFrames, u32 audioRate, u16 palette[256] (RGB15)
    frames: u32 audioBytes, u8 audio[audioBytes], u8 pixels[w*h]
"""
import os
import struct
import subprocess
import tempfile
from pathlib import Path

W, H = 256, 256  # letterboxed to the DS texture size (pad keeps aspect)


def convert(src, dst, fps=10, rate=22050):
    if os.environ.get('GTASADS_NATIVE_LIB'):
        from .portable import convert_movie
        blob, count = convert_movie(Path(src).read_bytes(), fps, rate)
        Path(dst).write_bytes(blob)
        return count
    with tempfile.TemporaryDirectory() as tmp:
        return _convert(src, dst, fps, rate, tmp)


def _convert(src, dst, fps, rate, tmp):
    pal_png = os.path.join(tmp, 'pal.png')
    pal_raw = os.path.join(tmp, 'pal.raw')
    frames = os.path.join(tmp, 'f.bin')
    audio = os.path.join(tmp, 'a.pcm')
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', src,
                    '-vf', 'palettegen=reserve_transparent=0', '-frames:v', '1', pal_png], check=True)
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', pal_png,
                    '-f', 'rawvideo', '-pix_fmt', 'rgb24', pal_raw], check=True)
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', src, '-i', pal_png,
                    '-lavfi', f'[0:v]scale={W}:{H}:force_original_aspect_ratio=decrease,'
                              f'pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:black,fps={fps}[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3',
                    '-f', 'rawvideo', '-pix_fmt', 'pal8', frames], check=True)
    subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', src, '-vn', '-ac', '1',
                    '-ar', str(rate), '-f', 's16le', audio], check=True)

    pr = Path(pal_raw).read_bytes()
    pal = [(pr[i] >> 3) | ((pr[i + 1] >> 3) << 5) | ((pr[i + 2] >> 3) << 10) for i in range(0, 768, 3)]
    fdata = Path(frames).read_bytes()
    # ffmpeg appends the 1024B palette (256xRGBA) after each pal8 frame: the
    # stride is W*H+1024 there, not W*H - slicing at W*H would drift +4 rows
    # every frame and the video scrolls down forever
    stride = W * H + 1024 if len(fdata) % (W * H) and len(fdata) % (W * H + 1024) == 0 else W * H
    nframes = len(fdata) // stride
    adata = Path(audio).read_bytes()

    if os.environ.get('GTASADS_NATIVE_LIB'):
        from .portable import pack_video
        blob, count = pack_video(pr, fdata, adata, fps, rate)
        with open(dst, 'wb') as out:
            out.write(blob)
        return count

    per = rate * 2.0 / fps
    with open(dst, 'wb') as out:
        out.write(struct.pack('<4sHHHHHxxII', b'DVID', 1, W, H, fps, 0, nframes, rate))
        out.write(struct.pack('<256H', *pal))
        ai = 0
        for i in range(nframes):
            nxt = min(len(adata), int((i + 1) * per))
            chunk = adata[ai:nxt]
            ai = nxt
            if len(chunk) % 2:
                chunk += b'\0'
            out.write(struct.pack('<I', len(chunk)))
            out.write(chunk)
            out.write(fdata[i * stride:i * stride + W * H])
        if ai < len(adata):
            out.write(struct.pack('<I', len(adata) - ai))
            out.write(adata[ai:])
        out.write(struct.pack('<I', 0))  # end marker
    return nframes
