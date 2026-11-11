import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path


def set_value(text, section, key, value):
    header = rf"(?m)^\[{re.escape(section)}\]\s*$"
    match = re.search(header, text)
    if not match:
        return text + f"\n[{section}]\n{key} = {value}\n"
    end = re.search(r"(?m)^\[", text[match.end():])
    end = match.end() + end.start() if end else len(text)
    body = text[match.end():end]
    line = rf"(?m)^{re.escape(key)}\s*=.*$"
    body, count = re.subn(line, lambda _: f"{key} = {value}", body, count=1)
    if not count:
        body += f"{key} = {value}\n"
    return text[:match.end()] + body + text[end:]


def main():
    mode, storage, emulator = sys.argv[1:]
    root = Path(__file__).resolve().parent.parent
    extension = "dsi" if mode == "dsi" else "nds"
    rom = Path(os.environ["GTASADS_ROM"]).resolve() if "GTASADS_ROM" in os.environ else root / "build" / (
        ("fat-" if storage == "rom" else "") + f"gtasads.{extension}")
    config = Path(os.environ.get("MELONDS_CONFIG", Path.home() / ".config/melonDS/melonDS.toml"))
    source = config.read_text() if config.exists() else ""
    settings = tomllib.loads(source)
    firmware = root / "build/emulator/firmware"
    nand_source = None
    firmware_source = None
    ds_firmware_source = None
    if mode == "dsi":
        required = {"BIOS9Path": "biosdsi9.bin", "BIOS7Path": "biosdsi7.bin",
                    "FirmwarePath": "dsifirmware.bin", "NANDPath": "dsinand.bin"}
        missing = []
        for key, name in required.items():
            path = Path(settings.get("DSi", {}).get(key) or firmware / name).expanduser()
            if not path.is_file():
                path = firmware / name
            if path.is_file():
                source = set_value(source, "DSi", key, f'"{path}"')
                if key == "NANDPath":
                    nand_source = path
                if key == "FirmwarePath":
                    firmware_source = path
            else:
                missing.append(key)
        if missing:
            sys.exit("DSi mode requires your own DSi BIOS, firmware and NAND dumps. "
                     f"Place {', '.join(required[key] for key in missing)} in {firmware} "
                     f"or configure [DSi] {', '.join(missing)} in {config}.")
    ds_files = {"BIOS9Path": "biosnds9.bin", "BIOS7Path": "biosnds7.bin",
                "FirmwarePath": "dsfirmware.bin"}
    if all((firmware / name).is_file() for name in ds_files.values()):
        for key, name in ds_files.items():
            source = set_value(source, "DS", key, f'"{firmware / name}"')
        ds_firmware_source = firmware / "dsfirmware.bin"
    profile = root / "build" / "emulator" / f"{mode}-{storage}{os.environ.get('GTASADS_PROFILE', '')}"
    (profile / "melonDS").mkdir(parents=True, exist_ok=True)
    if nand_source:
        nand = profile / "nand.bin"
        if not nand.exists():
            shutil.copy2(nand_source, nand)
        source = set_value(source, "DSi", "NANDPath", f'"{nand}"')
    if firmware_source:
        profile_firmware = profile / "firmware.bin"
        if not profile_firmware.exists():
            shutil.copy2(firmware_source, profile_firmware)
        source = set_value(source, "DSi", "FirmwarePath", f'"{profile_firmware}"')
    if ds_firmware_source:
        ds_firmware = profile / "dsfirmware.bin"
        if not ds_firmware.exists():
            shutil.copy2(ds_firmware_source, ds_firmware)
        source = set_value(source, "DS", "FirmwarePath", f'"{ds_firmware}"')
    sd_root = profile / "sd-root"
    if mode == "dsi":
        sd_root.mkdir(exist_ok=True)
    if storage == "sd":
        def copy_asset(src, dst):
            source = Path(src)
            target = Path(dst)
            if not target.exists() or source.stat().st_mtime_ns > target.stat().st_mtime_ns:
                shutil.copy2(src, dst)
            return dst

        shutil.copytree(root / "build/gtasads", sd_root / "gtasads",
                        dirs_exist_ok=True, copy_function=copy_asset)
    source = set_value(source, "Emu", "ConsoleType", "1" if mode == "dsi" else "0")
    external_bios = mode == "dsi" or all((firmware / name).is_file() for name in (
        "biosnds9.bin", "biosnds7.bin", "dsfirmware.bin"))
    source = set_value(source, "Emu", "ExternalBIOSEnable", "true" if external_bios else "false")
    source = set_value(source, "DLDI", "Enable", "true" if storage == "sd" else "false")
    source = set_value(source, "DLDI", "FolderSync", "true" if storage == "sd" else "false")
    source = set_value(source, "DLDI", "FolderPath", f'"{sd_root}"')
    source = set_value(source, "DLDI", "ImagePath", f'"{profile / "dldi.bin"}"')
    source = set_value(source, "DSi.SD", "Enable", "true" if mode == "dsi" else "false")
    source = set_value(source, "DSi.SD", "FolderSync", "true" if mode == "dsi" else "false")
    source = set_value(source, "DSi.SD", "FolderPath", f'"{sd_root}"')
    source = set_value(source, "DSi.SD", "ImagePath", f'"{profile / "dsisd.bin"}"')
    (profile / "melonDS/melonDS.toml").write_text(source)
    if mode == "dsi":
        alias = profile / "gtasads.nds"
        if alias.is_symlink() and alias.resolve() != rom:
            alias.unlink()
        if not alias.exists():
            alias.symlink_to(rom)
        rom = alias
    try:
        subprocess.run([emulator, str(rom)], env={**os.environ, "XDG_CONFIG_HOME": str(profile)}, check=True)
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
