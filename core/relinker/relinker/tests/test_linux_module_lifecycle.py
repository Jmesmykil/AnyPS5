"""Synthetic ELF lifecycle protocol, not a PS5 SDK fixture or title workaround."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

from test_guest_module_directories import main_with_needed
from test_guest_symbol_names import dynamic_tags, file_offset, module_symbols

RECORD = "__aps5_guest_lifecycle_nid_no_patch"
GETTER = "__aps5_get_pending_module_args_nid_no_patch"


def module(identity, result=17, entry=True, array=False, callback=False, needed=()):
    image = bytearray(0x4000)
    image[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", image, 16, 3, 62, 1, 0x1000, 64, 0, 0, 64, 56, 4, 64, 0, 0)
    struct.pack_into("<IIQQQQQQ", image, 232, 0x6474e551, 6, 0, 0, 0, 0, 0, 16)
    for index, (flags, start, length) in enumerate(((5, 0x1000, 0x1000), (6, 0x2000, 0x2000))):
        struct.pack_into("<IIQQQQQQ", image, 64 + index * 56, 1, flags, start, start, start, length, length, 0x1000)
    strings = b"\0fixture_state#A#B\0"
    callback_name = len(strings)
    if callback: strings += b"fixture_callback\0"
    need_offsets = []
    for name in needed:
        need_offsets.append(len(strings)); strings += name.encode() + b"\0"
    image[0x2200:0x2200 + len(strings)] = strings
    struct.pack_into("<IBBHQQ", image, 0x2618, 1, 0x11, 0, 1, 0x3000, 64)
    symbol_count = 2
    relas = []
    if callback:
        struct.pack_into("<IBBHQQ", image, 0x2630, callback_name, 0x12, 0, 0, 0, 0)
        symbol_count += 1
        relas.append((0x3200, (2 << 32) | 6, 0))
    if array: relas.append((0x3300, 8, 0x1100))
    struct.pack_into("<II", image, 0x2800, 1, symbol_count)
    for n, relocation in enumerate(relas): struct.pack_into("<QQq", image, 0x2900 + n * 24, *relocation)
    struct.pack_into("<Q", image, 0x3028, identity)

    def code(start, phase):
        body = bytearray()
        def rip(prefix, target):
            body.extend(bytes.fromhex(prefix)); body.extend(struct.pack("<i", target - (start + len(body) + 4)))
        if phase == 1:
            rip("48 89 3d", 0x3000)  # actual argc/argp/third argument
            rip("48 89 35", 0x3008)
            rip("48 89 15", 0x3010)
        rip("48 ff 05", 0x3018 if phase == 1 else 0x3020)
        if callback:
            body += b"\x50"  # align SysV stack before the host callback
            rip("48 8d 3d", 0x3000)
            body += b"\xbe" + struct.pack("<I", phase)
            rip("ff 15", 0x3200)
            body += b"\x58"
        body += b"\xb8" + struct.pack("<i", result) + b"\xc3"
        image[start:start + len(body)] = body
    code(0x1000, 1)
    code(0x1100, 2)
    tags = [(1, offset) for offset in need_offsets]
    tags += [(5, 0x2200), (10, len(strings)), (6, 0x2600), (11, 24), (4, 0x2800), (7, 0x2900), (8, len(relas) * 24), (9, 24)]
    if entry: tags.append((12, 0x1000))
    if array: tags += [(25, 0x3300), (27, 8)]
    tags.append((0, 0))
    for n, tag in enumerate(tags): struct.pack_into("<qQ", image, 0x2000 + n * 16, *tag)
    struct.pack_into("<IIQQQQQQ", image, 176, 2, 6, 0x2000, 0x2000, 0x2000, len(tags) * 16, len(tags) * 16, 8)
    return image


def main():
    relinker, kernel, harness = map(lambda value: Path(value).resolve(), sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="anyps5-linux-lifecycle-") as temporary:
        case = Path(temporary)
        (case / "plugins").mkdir()
        fixtures = {
            "plain": module(1), "negative": module(2, -37), "noentry": module(3, entry=False),
            "array": module(4, array=True), "nested": module(5, callback=True),
            "inner": module(6, -19), "recursive": module(7, callback=True),
            "arrayrecursive": module(8, array=True, callback=True),
            "dep": module(9, array=True, callback=True),
            "outerdep": module(10, callback=True, needed=("dep.prx",)),
            "thread1": module(11, callback=True), "thread2": module(12, callback=True),
            "badrecord": module(13), "missingrecord": module(14),
            "wrongowner": module(15, entry=False, needed=("plain.prx",)),
            "noentryarray": module(16, entry=False, array=True),
            "arraynested": module(17, array=True, callback=True), "same": module(18), "badextent": module(19),
            "unreadablerecord": module(20),
        }
        for name, image in fixtures.items(): (case / "plugins" / (name + ".prx")).write_bytes(image)
        source, output = case / "input.elf", case / "output.elf"
        source.write_bytes(main_with_needed())
        # Initializers require a declared real helper provider, even in unused plugins.
        refused = subprocess.run([str(relinker), str(source), str(output)], capture_output=True, text=True, timeout=30)
        assert refused.returncode == 2 and "libkernel.prx" in refused.stderr, refused.stderr
        result = subprocess.run([str(relinker), "--runtime-provider", "libkernel.prx", str(source), str(output)], capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        generated = case / "app0/plugins"
        for name, original in fixtures.items():
            assert (case / "plugins" / (name + ".prx")).read_bytes() == original
            image = (generated / (name + ".prx.guest.prx")).read_bytes()
            names, tags = module_symbols(image), dynamic_tags(image)
            assert RECORD in names
            has_entry = name not in ("noentry", "wrongowner", "noentryarray")
            assert (GETTER in names) == has_entry
            if has_entry:
                assert tags[8] >= 24  # includes a helper relocation when original RELA was empty
                assert image[file_offset(image, tags[12])] == 0x50
        # Deliberate malformed converted-fixture controls, never title inputs.
        for name in ("badrecord", "missingrecord", "wrongowner", "badextent", "unreadablerecord"):
            path = generated / (name + ".prx.guest.prx")
            image = bytearray(path.read_bytes())
            if name == "badrecord":
                position = image.index(b"APS5LCY1")
                struct.pack_into("<I", image, position + 12, 99)
            elif name in ("badextent", "unreadablerecord"):
                tags = dynamic_tags(image)
                phoff, = struct.unpack_from("<Q", image, 32)
                phsize, count = struct.unpack_from("<HH", image, 54)
                loads = [struct.unpack_from("<IIQQQQQQ", image, phoff + n * phsize) for n in range(count)
                         if struct.unpack_from("<I", image, phoff + n * phsize)[0] == 1]
                last = max(loads, key=lambda item: item[3] + item[6])
                if name == "badextent":
                    record_index = module_symbols(image).index(RECORD) + 1
                    struct.pack_into("<Q", image, file_offset(image, tags[6]) + record_index * 24 + 8, last[3] + last[6] - 16)
                else:
                    for n in range(count):
                        if struct.unpack_from("<IIQQQQQQ", image, phoff + n * phsize) == last:
                            struct.pack_into("<I", image, phoff + n * phsize + 4, last[1] & ~4)
            else:
                position = image.index(RECORD.encode())
                image[position] = ord("X")  # dlsym cannot borrow a dependency's record
            path.write_bytes(image)
        environment = os.environ.copy()
        environment["LD_LIBRARY_PATH"] = str(kernel.parent) + ":" + environment.get("LD_LIBRARY_PATH", "")
        result = subprocess.run([str(harness), str(kernel)], cwd=case, env=environment, capture_output=True, text=True, timeout=60)
        print(result.stdout, end=""); print(result.stderr, end="", file=sys.stderr)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    print("Linux module lifecycle controls passed using actual patched kernel")


if __name__ == "__main__": main()
