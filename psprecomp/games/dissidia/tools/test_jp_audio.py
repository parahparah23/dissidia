#!/usr/bin/env python3
"""Self-test for jp_audio.splice_movie_audio on synthetic PMF movies (no game data). Run: python3 -I test_jp_audio.py"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jp_audio as ja

FRAME = ja.ATRAC_FRAME
HEADER = 0x800   # where the packs start (byte 8 of the file)


def pes(stream_id, payload):
    hdr = b"\x81\x80\x00"   # flags, no PTS, header length 0
    return b"\x00\x00\x01" + bytes([stream_id]) + struct.pack(">H", len(hdr) + len(payload)) + hdr + payload


def movie(video, sound, chunk):
    """A PMF: per pack a video packet (from `video`) and a sound packet of up to `chunk` ATRAC bytes with its 4-byte header."""
    out = bytearray(b"PSMF0015" + struct.pack(">I", HEADER))
    out += b"\0" * (HEADER - len(out))
    vpos = spos = 0
    while spos < len(sound) or vpos < len(video):
        pack = bytearray(b"\x00\x00\x01\xba" + b"\x44\x00\x04\x00\x04\x01\x01\x89\xc3\xf8")
        v = video[vpos:vpos + 400]
        vpos += len(v)
        if v:
            pack += pes(0xE0, v)
        s = sound[spos:spos + chunk]
        if s:
            nxt = (-spos) % FRAME   # bytes until the next frame starts
            first = nxt if nxt < len(s) else 0xFFFF
            pack += pes(0xBD, b"\x00\x00" + struct.pack(">H", first) + s)
            spos += len(s)
        pack += b"\xff" * (ja.SECTOR - len(pack))
        out += pack
    return bytes(out)


def sound_of(data):
    parts, ok = ja._pes_audio(data)
    return b"".join(data[s:s + n] for s, n in parts), ok


def main():
    en_sound = bytes((i * 7) & 0xFF for i in range(FRAME * 5))
    jp_sound = bytes((i * 13 + 5) & 0xFF for i in range(FRAME * 6))   # one frame longer, like DES01_010
    mine = movie(b"E" * 3000, en_sound, 1500)
    undub = movie(b"J" * 3200, jp_sound, 1600)                       # other picture, other packets
    s, ok = sound_of(mine)
    assert ok and s == en_sound, "parser: the installed movie's sound"
    s, ok = sound_of(undub)
    assert ok and s == jp_sound, "parser: the undub movie's sound"

    out = ja.splice_movie_audio(mine, undub)
    assert out is not None and len(out) == len(mine)
    s, ok = sound_of(out)
    assert ok and s == jp_sound[:len(en_sound)], "the undub's sound, its last frame left out"
    parts, _ = ja._pes_audio(out)
    masked_out, masked_mine = bytearray(out), bytearray(mine)
    for st, n in parts:
        masked_out[st:st + n] = masked_mine[st:st + n] = b"\0" * n
    assert masked_out == masked_mine, "everything but the sound bytes is the installed movie (picture, packet headers)"

    shorter = movie(b"J" * 3200, jp_sound[:FRAME * 4], 1600)
    s, _ = sound_of(ja.splice_movie_audio(mine, shorter))
    assert s == jp_sound[:FRAME * 4] + en_sound[FRAME * 4:], "a shorter undub sound: the installed sound fills in"

    broken = bytearray(undub)
    i = broken.index(b"\x00\x00\x01\xbd", HEADER + ja.SECTOR)   # the second sound packet: a frame start off the 752 grid
    broken[i + 9 + 2:i + 9 + 4] = struct.pack(">H", 5)
    assert ja.splice_movie_audio(mine, bytes(broken)) is None, "a stream not on 752-byte frames is refused"
    print("test_jp_audio: ok")


if __name__ == "__main__":
    main()
