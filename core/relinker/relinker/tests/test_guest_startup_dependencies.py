"""Static startup closure is independent of recursive artifact discovery."""
from pathlib import Path
import subprocess
import sys
import tempfile

from test_guest_module_directories import (guest_with_needed, main_with_needed,
                                            module_with_symbol, needed_libraries)
from test_guest_symbol_names import importing_executable


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-startup-closure-") as temporary:
        root = Path(temporary)
        for kind in ("unused", "explicit", "symbol", "missing-unused", "wrong-unused"):
            case = root / kind
            (case / "plugins").mkdir(parents=True)
            (case / "modules").mkdir()
            (case / "modules/provider.prx").write_bytes(module_with_symbol(True))
            (case / "plugins/consumer.prx").write_bytes(module_with_symbol(False))
            missing = kind.endswith("unused") and kind != "unused"
            (case / "plugins/unused.prx").write_bytes(guest_with_needed("absent.prx") if missing else guest_with_needed("host.prx"))
            main_image = (main_with_needed("plugins/consumer.prx") if kind == "explicit" else
                          importing_executable(["shared#A#B"]) if kind == "symbol" else main_with_needed())
            source, output = case / "input.elf", case / "output.elf"
            source.write_bytes(main_image)
            original = {path: path.read_bytes() for path in case.rglob("*.prx")}
            options = ["--runtime-provider", "host.prx"]
            if kind == "wrong-unused": options += ["--runtime-provider", "different.prx"]
            result = subprocess.run([str(relinker), *options, str(source), str(output)], capture_output=True, text=True, timeout=30)
            assert all(path.read_bytes() == value for path, value in original.items())
            if missing:
                assert result.returncode == 2 and "absent.prx" in result.stderr, result.stderr
                assert not output.exists()
                continue
            assert result.returncode == 0, result.stderr
            artifacts = list((case / "app0").rglob("*.guest.prx"))
            assert len(artifacts) == 3
            expected = []
            if kind in ("explicit", "symbol"):
                expected = ["$ORIGIN/app0/modules/provider.prx.guest.prx"]
            if kind == "explicit": expected += ["$ORIGIN/app0/plugins/consumer.prx.guest.prx"]
            assert needed_libraries(output.read_bytes()) == expected
            assert needed_libraries((case / "app0/plugins/consumer.prx.guest.prx").read_bytes()) == ["$ORIGIN/../modules/provider.prx.guest.prx"]
            assert needed_libraries((case / "app0/plugins/unused.prx.guest.prx").read_bytes()) == ["host.prx"]
            assert "host.prx" not in needed_libraries(output.read_bytes())
            print(kind, "all artifacts converted; main closure and plugin dependencies preserved")
    print("Startup dependency controls passed")


if __name__ == "__main__": main()
