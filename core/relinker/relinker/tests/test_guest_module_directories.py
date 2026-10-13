from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

from test_guest_intel_trampolines import PLAIN_SITE, elf_loads, guest_fixture, main_fixture


def module_with_symbol(exported):
    image = guest_fixture(PLAIN_SITE)
    name = b"\0shared#A#B\0"
    image[0x800:0x800 + len(name)] = name
    struct.pack_into("<Q", image, 0x618, len(name))
    struct.pack_into("<Q", image, 0x628, 0x2280)
    struct.pack_into("<IIIII", image, 0x840, 1, 2, 1, 0, 0)
    struct.pack_into("<IBBHQQ", image, 0x898, 1, 0x12, 0,
                     1 if exported else 0, 0x1000 if exported else 0, 1 if exported else 0)
    if not exported:
        struct.pack_into("<Q", image, 0x668, 24)
        struct.pack_into("<QQq", image, 0x900, 0x2320, (1 << 32) | 6, 0)
    return image


def dynamic_strings(image, wanted=1):
    loads = elf_loads(image)

    def offset(address):
        for header in loads:
            if header[3] <= address < header[3] + header[5]:
                return header[2] + address - header[3]
        raise AssertionError(f"Unmapped address: {address:#x}")

    phoff, = struct.unpack_from("<Q", image, 32)
    phsize, phcount = struct.unpack_from("<HH", image, 54)
    dynamic = next(struct.unpack_from("<IIQQQQQQ", image, phoff + index * phsize)
                   for index in range(phcount)
                   if struct.unpack_from("<I", image, phoff + index * phsize)[0] == 2)
    tags = []
    for position in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from("<qQ", image, position)
        if tag == 0:
            break
        tags.append((tag, value))
    strings = offset(next(value for tag, value in tags if tag == 5))
    return [image[strings + value:image.index(0, strings + value)].decode()
            for tag, value in tags if tag == wanted]


def needed_libraries(image):
    return dynamic_strings(image)


def main_with_needed(*names):
    image = main_fixture()
    strings = b"\0" + b"".join(name.encode() + b"\0" for name in names)
    image[0x4A00:0x4A00 + len(strings)] = strings
    tags, cursor = [], 1
    for name in names:
        tags.append((1, cursor)); cursor += len(name) + 1
    tags += [(5, 0x4A00), (10, len(strings)), (6, 0x4620), (11, 24), (7, 0x4700), (8, 0), (9, 24), (0, 0)]
    for n, tag in enumerate(tags): struct.pack_into("<qQ", image, 0x4800 + n * 16, *tag)
    struct.pack_into("<QQQQQ", image, 128, 0x4800, 0x4800, 0x4800, len(tags) * 16, len(tags) * 16)
    return image


def guest_with_needed(*names):
    image = guest_fixture(PLAIN_SITE)
    strings = b"\0" + b"".join(name.encode() + b"\0" for name in names)
    image[0x780:0x780 + len(strings)] = strings
    tags, cursor = [], 1
    for name in names:
        tags.append((1, cursor)); cursor += len(name) + 1
    tags += [(5, 0x2180), (10, len(strings)), (6, 0x2220), (11, 24), (4, 0x2240), (7, 0x2300), (8, 0), (9, 24), (0, 0)]
    for n, tag in enumerate(tags): struct.pack_into("<qQ", image, 0x600 + n * 16, *tag)
    struct.pack_into("<QQ", image, 176 + 32, len(tags) * 16, len(tags) * 16)
    return image


def five_strong_imports_fixture():
    # Explicit synthetic ELF consumer, not a Sony module or a dialog implementation.
    # The five NIDs reproduce the unresolved-import shape only; no API is called.
    nids = ("UtXl-tmi7iw", "CtygVRCL+bA", "4J5F23VgTjY", "VuKbx6zlEG4", "RmJKkzZFNFA")
    library = "libSceCdlgPlayerReview.prx"
    image = bytearray(0x3000)
    image[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", image, 16,
                     3, 62, 1, 0x1000, 64, 0, 0, 64, 56, 3, 64, 0, 0)
    for index, (flags, address) in enumerate(((5, 0x1000), (6, 0x2000))):
        struct.pack_into("<IIQQQQQQ", image, 64 + index * 56,
                         1, flags, address, address, address, 0x1000, 0x1000, 0x1000)
    image[0x1000] = 0xC3
    strings = b"\0" + library.encode() + b"\0"
    offsets = []
    for nid in nids:
        offsets.append(len(strings))
        strings += (nid + "#A#B").encode() + b"\0"
    image[0x2200:0x2200 + len(strings)] = strings
    for index, offset in enumerate(offsets, 1):
        struct.pack_into("<IBBHQQ", image, 0x2400 + index * 24, offset, 0x12, 0, 0, 0, 0)
        struct.pack_into("<QQq", image, 0x2600 + (index - 1) * 24,
                         0x2700 + (index - 1) * 8, (index << 32) | 6, 0)
    # DT_HASH's chain count establishes all six symbol-table entries.
    struct.pack_into("<II7I", image, 0x2500, 1, 6, *([0] * 7))
    tags = [(1, 1), (5, 0x2200), (10, len(strings)), (6, 0x2400), (11, 24),
            (4, 0x2500), (7, 0x2600), (8, 5 * 24), (9, 24), (0, 0)]
    for index, tag in enumerate(tags):
        struct.pack_into("<qQ", image, 0x2000 + index * 16, *tag)
    struct.pack_into("<IIQQQQQQ", image, 176, 2, 6, 0x2000, 0x2000, 0x2000,
                     len(tags) * 16, len(tags) * 16, 8)
    return image, nids, library


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-module-directories-") as directory:
        work = Path(directory)

        def convert(case, windows, options=(), image=None, destination=None):
            source = case / "input.elf"
            source.write_bytes(main_fixture() if image is None else image)
            output = (case if destination is None else destination) / ("output.exe" if windows else "output.elf")
            result = subprocess.run([str(relinker), *(["--windows"] if windows else []),
                                     *options, str(source), str(output)],
                                    capture_output=True, text=True, timeout=30)
            return result, output

        # Closure is a literal module-name check, distinct from export binding/calls.
        for windows in (False, True):
            consumer, nids, library = five_strong_imports_fixture()
            for label, declared in (("missing", None), ("unrelated", "unrelated.prx"),
                                    ("declared-present", library)):
                case = work / f"{windows}-five-imports-{label}"
                (case / "plugins").mkdir(parents=True)
                (case / "plugins/consumer.prx").write_bytes(consumer)
                options = [] if declared is None else ["--runtime-provider", declared]
                result, output = convert(case, windows, options)
                if declared != library:
                    assert result.returncode == 2 and library in result.stderr and "Unresolved dependency" in result.stderr, result.stderr
                    assert not output.exists() and not (case / "app0").exists()
                else:
                    assert result.returncode == 0, result.stderr
                    artifact = case / "app0/plugins/consumer.prx.guest.prx"
                    assert all(nid.encode() in artifact.read_bytes() for nid in nids)
                    if not windows:
                        assert needed_libraries(artifact.read_bytes()) == [library]
                    # A declaration proves closure only, not that a provider exists,
                    # implements these APIs or can return any particular SDK result.
            case = work / f"{windows}-unused-provider"
            (case / "prx").mkdir(parents=True)
            result, output = convert(case, windows, ["--runtime-provider", library])
            assert result.returncode == 0, result.stderr
            if not windows:
                assert library not in needed_libraries(output.read_bytes())
            print(f"five-import closure ({'Windows' if windows else 'Linux'}): missing/unrelated rejected; declared present accepted; unused omitted")

        # Runtime modules need not live in the three SDK directory names.
        for windows in (False, True):
            case = work / f"{windows}-nested-runtime"
            (case / "engine" / "modules").mkdir(parents=True)
            (case / "engine" / "plugins").mkdir()
            (case / "engine" / "modules" / "provider.prx").write_bytes(module_with_symbol(True))
            (case / "engine" / "plugins" / "consumer.prx").write_bytes(module_with_symbol(False))
            result, output = convert(case, windows)
            assert result.returncode == 0, (result.stdout, result.stderr)
            expected = {case / "app0" / "engine" / "modules" / "provider.prx.guest.prx",
                        case / "app0" / "engine" / "plugins" / "consumer.prx.guest.prx"}
            assert set((case / "app0").rglob("*.guest.prx")) == expected
            if not windows:
                # Discovery converts unused plugins but does not start them.
                assert needed_libraries(output.read_bytes()) == []
                assert dynamic_strings((case / "app0/engine/plugins/consumer.prx.guest.prx").read_bytes(), 29) == ["$ORIGIN/../../../libs"]
                assert needed_libraries((case / "app0/engine/plugins/consumer.prx.guest.prx").read_bytes()) == [
                    "$ORIGIN/../modules/provider.prx.guest.prx"]


        for windows in (False, True):
            case = work / f"{windows}-root-transitive"
            (case / "engine/plugins").mkdir(parents=True)
            (case / "root.prx").write_bytes(guest_fixture(PLAIN_SITE))
            (case / "engine/plugins/consumer.prx").write_bytes(guest_with_needed("root.prx"))
            result, output = convert(case, windows, image=main_with_needed("engine/plugins/consumer.prx"))
            assert result.returncode == 0, result.stderr
            assert (case / "app0/root.prx.guest.prx").exists()
            if not windows:
                assert needed_libraries(output.read_bytes()) == ["$ORIGIN/app0/root.prx.guest.prx", "$ORIGIN/app0/engine/plugins/consumer.prx.guest.prx"]
                assert needed_libraries((case / "app0/engine/plugins/consumer.prx.guest.prx").read_bytes()) == ["$ORIGIN/../../root.prx.guest.prx"]
                assert dynamic_strings((case / "app0/root.prx.guest.prx").read_bytes(), 29) == ["$ORIGIN/../libs"]

            for bad in ("missing.prx", "../escape.prx", "/absolute.prx"):
                case = work / (str(windows) + "-missing-" + bad.replace('/', '_'))
                (case / "prx").mkdir(parents=True)
                result, output = convert(case, windows, image=main_with_needed(bad))
                assert result.returncode == 2 and ("Unresolved dependency" in result.stderr or "Invalid dependency" in result.stderr), result.stderr
                assert not output.exists() and not (case / "app0").exists()

            case = work / f"{windows}-verified-runtime"
            (case / "prx").mkdir(parents=True)
            result, output = convert(case, windows, ["--runtime-provider", "host.prx"], main_with_needed("host.prx"))
            assert result.returncode == 0, result.stderr
            if not windows: assert needed_libraries(output.read_bytes()) == ["host.prx"]
            result, _ = convert(case, windows, ["--runtime-provider", "../bad.prx"], main_with_needed("host.prx"))
            assert result.returncode == 2 and "Invalid declared runtime" in result.stderr, result.stderr

            case = work / f"{windows}-matching-backup"
            (case / "prx").mkdir(parents=True)
            sample = module_with_symbol(True)
            (case / "prx/a.prx").write_bytes(sample)
            (case / "prx/a.prx.esbak").write_bytes(sample)
            (case / "prx/metadata.txt").write_text("metadata")
            result, output = convert(case, windows)
            assert result.returncode == 0, result.stderr
            assert {p.name for p in (case / "app0").rglob("*.guest.prx")} == {"a.prx.guest.prx"}

            case = work / f"{windows}-output-link-input"
            (case / "prx").mkdir(parents=True)
            (case / "prx/a.prx").write_bytes(guest_fixture(PLAIN_SITE))
            destination = work / f"{windows}-output-link-output"
            (destination / "app0/prx").mkdir(parents=True)
            victim = work / f"{windows}-unrelated-victim"
            victim.write_bytes(b"unchanged unrelated data")
            (destination / "app0/prx/a.prx.guest.prx").symlink_to(victim)
            result, output = convert(case, windows, destination=destination)
            assert result.returncode == 2 and "output file is a symbolic link" in result.stderr, result.stderr
            assert victim.read_bytes() == b"unchanged unrelated data" and not output.exists()

            case = work / f"{windows}-cycle"
            (case / "prx").mkdir(parents=True)
            (case / "prx/a.prx").write_bytes(guest_with_needed("b.prx"))
            (case / "prx/b.prx").write_bytes(guest_with_needed("a.prx"))
            result, output = convert(case, windows)
            assert result.returncode == 2 and "Cyclic guest" in result.stderr, result.stderr
            assert not output.exists()

            case = work / f"{windows}-unclean"
            (case / "engine").mkdir(parents=True)
            (case / "engine/wrapped.prx").write_bytes(b"\x4f\x15\x3d\x1d" + bytes(60))
            result, output = convert(case, windows)
            assert result.returncode == 2 and "clean ELF" in result.stderr, result.stderr
            assert not output.exists()

            case = work / f"{windows}-basename-collision"
            for folder in ("first", "second"):
                (case / folder).mkdir(parents=True)
                (case / folder / "same.prx").write_bytes(guest_fixture(PLAIN_SITE))
            result, output = convert(case, windows)
            assert result.returncode == 2 and "Conflicting guest output" in result.stderr, result.stderr
            assert not output.exists()

            case = work / f"{windows}-symlink"
            case.mkdir()
            outside = work / f"{windows}-outside.prx"
            outside.write_bytes(guest_fixture(PLAIN_SITE))
            (case / "outside.prx").symlink_to(outside)
            result, output = convert(case, windows)
            assert result.returncode == 2 and "symbolic link" in result.stderr, result.stderr
            assert not output.exists()

        for windows in (False, True):
            for standard in (None, "sce_module", "sce_modules"):
                case = work / f"{windows}-{standard}"
                prx = case / "prx"
                prx.mkdir(parents=True)
                provider = (case / standard) if standard else prx
                provider.mkdir(exist_ok=True)
                (provider / "provider.prx").write_bytes(module_with_symbol(True))
                (prx / "consumer.prx").write_bytes(module_with_symbol(False))
                (prx / "ignored.txt").write_text("not ELF")
                (prx / "old.prx.guest.prx").write_bytes(guest_fixture(PLAIN_SITE))
                result, output = convert(case, windows)
                assert result.returncode == 0, (result.stdout, result.stderr)
                artifacts = list((case / "app0").rglob("*.guest.prx"))
                expected = {case / "app0" / (standard or "prx") / "provider.prx.guest.prx",
                            case / "app0" / "prx" / "consumer.prx.guest.prx"}
                assert set(artifacts) == expected, artifacts
                for artifact in artifacts:
                    assert artifact.read_bytes().startswith(b"MZ" if windows else b"\x7fELF")
                    assert f"    {artifact.relative_to(case / 'app0').as_posix()}\n" in result.stdout, result.stdout
                if windows and os.name == "nt":
                    run = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
                    assert run.returncode == 42, (run.returncode, run.stdout, run.stderr)
                if not windows:
                    needed = needed_libraries(output.read_bytes())
                    assert needed == [], needed
                    consumer = case / "app0" / "prx" / "consumer.prx.guest.prx"
                    needed = needed_libraries(consumer.read_bytes())
                    assert needed == [f"$ORIGIN/{'../' + standard + '/' if standard else ''}provider.prx.guest.prx"], needed

            case = work / f"{windows}-exclude"
            for name in ("sce_module", "prx"):
                (case / name).mkdir(parents=True)
                (case / name / "omit.prx").write_bytes(module_with_symbol(True))
            result, output = convert(case, windows, ["--exclude-sce-module", "omit.prx"])
            assert result.returncode == 0 and output.exists(), result.stderr
            assert not (case / "app0").exists(), case
            result, _ = convert(case, windows, ["--exclude-sce-module", "missing.prx"])
            assert result.returncode == 2 and "file not found" in result.stderr, result.stderr

            case = work / f"{windows}-invalid"
            case.mkdir()
            (case / "prx").write_text("not a directory")
            result, output = convert(case, windows)
            assert result.returncode == 2 and "not a directory" in result.stderr, result.stderr
            assert not output.exists(), output

            case = work / f"{windows}-ambiguous"
            for name in ("sce_module", "sce_modules", "prx"):
                (case / name).mkdir(parents=True)
            result, output = convert(case, windows)
            assert result.returncode == 2 and "Both sce_module and sce_modules" in result.stderr, result.stderr
            assert not output.exists(), output

            case = work / f"{windows}-duplicate"
            for name in ("sce_module", "prx"):
                (case / name).mkdir(parents=True)
                (case / name / f"{name}.prx").write_bytes(module_with_symbol(True))
            result, output = convert(case, windows)
            assert result.returncode == 0 and output.exists(), result.stderr
            assert {path.name for path in (case / "app0").rglob("*.guest.prx")} == {"sce_module.prx.guest.prx", "prx.prx.guest.prx"}

            case = work / f"{windows}-ambiguous-import"
            for name in ("sce_module", "prx"):
                (case / name).mkdir(parents=True)
                (case / name / f"{name}.prx").write_bytes(module_with_symbol(True))
            (case / "prx" / "consumer.prx").write_bytes(module_with_symbol(False))
            result, output = convert(case, windows)
            assert result.returncode == 2 and "Ambiguous guest import" in result.stderr and "shared" in result.stderr, result.stderr
            assert not output.exists() and not (case / "app0").exists(), output
    print("Guest module directory integration tests passed")


if __name__ == "__main__":
    main()
