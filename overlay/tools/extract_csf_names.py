#!/usr/bin/env python3
"""Extract RA3 CSF Name:* labels -> TypeId=中文 for the overlay.

Reads Lang-Chinese*.big (RefPack-compressed gamestrings.csf) from a RA3 install,
writes unit_names_csf.txt next to unit_names.txt. Runtime loads this as the
base catalog; unit_names.txt overrides win.

Usage:
  python extract_csf_names.py
  python extract_csf_names.py --ra3 "D:\\Steam\\steamapps\\common\\Command and Conquer Red Alert 3"
  python extract_csf_names.py --merge   # also refresh unit_names.txt gaps from CSF
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from pathlib import Path


def refpack_decompress(src: bytes) -> bytes:
    if len(src) < 5 or src[1] != 0xFB:
        raise ValueError("not EA RefPack (expected 0x?? 0xFB)")
    flags = src[0]
    i = 2
    if flags & 0x01:
        i += 4 if (flags & 0x80) else 3
    if flags & 0x80:
        unc = struct.unpack_from(">I", src, i)[0]
        i += 4
    else:
        unc = (src[i] << 16) | (src[i + 1] << 8) | src[i + 2]
        i += 3
    out = bytearray()
    while i < len(src):
        b0 = src[i]
        i += 1
        if b0 < 0x80:
            b1 = src[i]
            i += 1
            plain = b0 & 3
            for _ in range(plain):
                out.append(src[i])
                i += 1
            copy_n = ((b0 & 0x1C) >> 2) + 3
            off = ((b0 & 0x60) << 3) + b1 + 1
            src_i = len(out) - off
            for _ in range(copy_n):
                out.append(out[src_i])
                src_i += 1
        elif b0 < 0xC0:
            b1, b2 = src[i], src[i + 1]
            i += 2
            plain = b1 >> 6
            for _ in range(plain):
                out.append(src[i])
                i += 1
            copy_n = (b0 & 0x3F) + 4
            off = ((b1 & 0x3F) << 8) + b2 + 1
            src_i = len(out) - off
            for _ in range(copy_n):
                out.append(out[src_i])
                src_i += 1
        elif b0 < 0xE0:
            b1, b2, b3 = src[i], src[i + 1], src[i + 2]
            i += 3
            plain = b0 & 3
            for _ in range(plain):
                out.append(src[i])
                i += 1
            copy_n = ((b0 & 0x0C) << 6) + b3 + 5
            off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
            src_i = len(out) - off
            for _ in range(copy_n):
                out.append(out[src_i])
                src_i += 1
        else:
            if b0 >= 0xFC:
                plain = b0 & 3
                for _ in range(plain):
                    out.append(src[i])
                    i += 1
                break
            plain = ((b0 & 0x1F) << 2) + 4
            for _ in range(plain):
                out.append(src[i])
                i += 1
    return bytes(out[:unc])


def read_big_entry(path: Path, want_suffix: str = "gamestrings.csf") -> bytes:
    with path.open("rb") as f:
        magic = f.read(4)
        if magic not in (b"BIG4", b"BIGF"):
            raise ValueError(f"{path}: not a BIG archive ({magic!r})")
        f.read(4)  # archive size
        entry_count, _index_size = struct.unpack(">II", f.read(8))
        entries = []
        for _ in range(entry_count):
            pos, size = struct.unpack(">II", f.read(8))
            name = b""
            while True:
                c = f.read(1)
                if not c or c == b"\x00":
                    break
                name += c
            entries.append((name.decode("latin1", "replace"), pos, size))
        for name, pos, size in entries:
            if name.lower().endswith(want_suffix.lower()):
                f.seek(pos)
                return f.read(size)
    raise FileNotFoundError(f"{path}: no entry ending with {want_suffix}")


def parse_csf(data: bytes) -> dict[str, str]:
    if data[:4] != b" FSC":
        raise ValueError(f"bad CSF magic {data[:4]!r}")
    out: dict[str, str] = {}
    off = 24
    while True:
        p = data.find(b" LBL", off)
        if p < 0:
            break
        off = p + 4
        if off + 8 > len(data):
            break
        num_pairs = struct.unpack_from("<I", data, off)[0]
        off += 4
        name_len = struct.unpack_from("<I", data, off)[0]
        off += 4
        if off + name_len > len(data):
            break
        label = data[off : off + name_len].decode("ascii", "replace")
        off += name_len
        value = ""
        for _ in range(num_pairs):
            if off + 8 > len(data):
                break
            magic = data[off : off + 4]
            off += 4
            vlen = struct.unpack_from("<I", data, off)[0]
            off += 4
            raw = data[off : off + vlen * 2]
            off += vlen * 2
            value = bytes((~b) & 0xFF for b in raw).decode("utf-16-le", "replace")
            if magic == b"WRTS" and off + 4 <= len(data):
                elen = struct.unpack_from("<I", data, off)[0]
                off += 4 + elen
        out[label] = value
    return out


# Prefer CamelCase TypeIds when we already know them (memory / XML style).
KNOWN_CAMEL = [
    "AlliedAirfield",
    "AlliedAntiAirInfantry",
    "AlliedAntiAirShip",
    "AlliedAntiAirVehicleTech1",
    "AlliedAntiGroundAircraft",
    "AlliedAntiInfantryInfantry",
    "AlliedAntiInfantryVehicle",
    "AlliedAntiInfantryVehicle_Ground",
    "AlliedAntiNavyShipTech1",
    "AlliedAntiStructureShip",
    "AlliedAntiStructureVehicle",
    "AlliedAntiVehicleInfantry",
    "AlliedAntiVehicleVehicleTech1",
    "AlliedAntiVehicleVehicleTech3",
    "AlliedBarracks",
    "AlliedBaseDefense",
    "AlliedBaseDefenseAdvanced",
    "AlliedBomberAircraft",
    "AlliedChronosphere",
    "AlliedCommando",
    "AlliedCommandoTech1",
    "AlliedConstructionYard",
    "AlliedConYard",
    "AlliedCrane",
    "AlliedEngineer",
    "AlliedFighterAircraft",
    "AlliedInfantryVehicle",
    "AlliedInfiltrationInfantry",
    "AlliedMCV",
    "AlliedMCV_Naval",
    "AlliedMiner",
    "AlliedMiner_Naval",
    "AlliedNavalYard",
    "AlliedOutpost",
    "AlliedPowerPlant",
    "AlliedProtonCollider",
    "AlliedRefinery",
    "AlliedScoutInfantry",
    "AlliedShipyard",
    "AlliedSupportAircraft",
    "AlliedSurveyor",
    "AlliedTechStructure",
    "AlliedWallHub",
    "AlliedWarFactory",
    "AirportTechStructure",
    "DefensiveStructureTechStructure",
    "GarageTechStructure",
    "HospitalTechBuilding",
    "HospitalTechStructure",
    "JapanAntiAirInfantry",
    "JapanAntiAirShip",
    "JapanAntiAirShip_Air",
    "JapanAntiAirShip_Sea",
    "JapanAntiAirVehicleTech1",
    "JapanAntiAirVehicle_Air",
    "JapanAntiAirVehicle_Ground",
    "JapanAntiInfantryInfantry",
    "JapanAntiInfantryVehicle",
    "JapanAntiInfantryVehicleTech1",
    "JapanAntiInfantryVehicle_Air",
    "JapanAntiShipAircraft",
    "JapanAntiStructureShip",
    "JapanAntiStructureVehicle",
    "JapanAntiVehicleInfantry",
    "JapanAntiVehicleInfantryTech3",
    "JapanAntiVehicleShip",
    "JapanAntiVehicleVehicle",
    "JapanAntiVehicleVehicleTech1",
    "JapanAntiVehicleVehicleTech1_Naval",
    "JapanBarracks",
    "JapanBaseDefense",
    "JapanBaseDefenseAdv",
    "JapanBaseDefenseAdvanced",
    "JapanCommando",
    "JapanCommandoTech1",
    "JapanConstructionYard",
    "JapanConYard",
    "JapanCrane",
    "JapanEngineer",
    "JapanFinalSquadronAircraft",
    "JapanInfiltrationInfantry",
    "JapanLightTransportVehicle",
    "JapanMCV",
    "JapanMCV_Naval",
    "JapanMiner",
    "JapanNanotechMainframe",
    "JapanNavalYard",
    "JapanNavyScoutShip",
    "JapanOutpost",
    "JapanPointDefenseDrone",
    "JapanPowerPlant",
    "JapanPsionicDecimator",
    "JapanRadarShip",
    "JapanRefinery",
    "JapanScoutInfantry",
    "JapanShipyard",
    "JapanSurveyor",
    "JapanTechStructure",
    "JapanWallHub",
    "JapanWarFactory",
    "ObservationPostTechStructure",
    "OilDerrick",
    "OilDerrick_OnWater",
    "OreNode",
    "OreNode2a",
    "OreNode2b",
    "OreNode4a",
    "OreNode4b",
    "OreNode4c",
    "OreNode4d",
    "ShipYardTechStructure",
    "SovietAirfield",
    "SovietAntiAirInfantry",
    "SovietAntiAirShip",
    "SovietAntiAirShip_Ground",
    "SovietAntiAirVehicleTech1",
    "SovietAntiGroundAircraft",
    "SovietAntiInfantryInfantry",
    "SovietAntiInfantryVehicle",
    "SovietAntiNavyShipTech1",
    "SovietAntiNavyShipTech2",
    "SovietAntiStructureShip",
    "SovietAntiStructureVehicle",
    "SovietAntiVehicleInfantry",
    "SovietAntiVehicleVehicleTech1",
    "SovietAntiVehicleVehicleTech2",
    "SovietAntiVehicleVehicleTech3",
    "SovietBarracks",
    "SovietBaseDefenseAdv",
    "SovietBaseDefenseAdvanced",
    "SovietBaseDefenseAir",
    "SovietBaseDefenseGround",
    "SovietBomberAircraft",
    "SovietBunker",
    "SovietCommando",
    "SovietCommandoTech1",
    "SovietConstructionYard",
    "SovietConYard",
    "SovietCrane",
    "SovietEngineer",
    "SovietFighterAircraft",
    "SovietHeavyAntiVehicleInfantry",
    "SovietIronCurtain",
    "SovietMCV",
    "SovietMCV_Naval",
    "SovietMiner",
    "SovietNavalYard",
    "SovietOutpost",
    "SovietPowerPlant",
    "SovietPowerPlantAdv",
    "SovietPowerPlantAdvanced",
    "SovietRefinery",
    "SovietScoutInfantry",
    "SovietScoutVehicle",
    "SovietShipyard",
    "SovietSubmarine",
    "SovietSuperWeapon",
    "SovietSurveyor",
    "SovietSurveyor_Naval",
    "SovietTechStructure",
    "SovietTeslaWallHub",
    "SovietVacuumImploder",
    "SovietWarFactory",
    "TechBuildingOilDerrick",
    "TechBuildingOreNode",
    "TechBuildingOreNode2",
    "TechBuildingOreNode4",
    "VeterancyTechStructure",
]

# Extra aliases: wrong/historical ids → canonical CSF TypeId (for lookup only via duplicate rows)
ALIASES = {
    # Community / older docs used SovietSubmarine; CSF uses AntiNavyShipTech2.
    "SovietSubmarine": "SovietAntiNavyShipTech2",
    # Wiki sometimes uses HospitalTechStructure; CSF uses HospitalTechBuilding.
    "HospitalTechStructure": "HospitalTechBuilding",
    "OilDerrick": "TechBuildingOilDerrick",
    "OreNode": "TechBuildingOreNode",
    # Rocket Angel: CSF id is JapanAntiVehicleInfantryTech3.
    "JapanAntiAirInfantry": "JapanAntiVehicleInfantryTech3",
}


def camelize_key(upper_id: str, known_map: dict[str, str]) -> str:
    u = upper_id.upper()
    if u in known_map:
        return known_map[u]
    # Fallback: keep CSF label's trailing id as-is (usually ALLCAPS).
    return upper_id


def extract_name_map(csf: dict[str, str]) -> dict[str, str]:
    known_map = {k.upper(): k for k in KNOWN_CAMEL}
    names: dict[str, str] = {}
    for label, value in csf.items():
        if not label.upper().startswith("NAME:"):
            continue
        raw_id = label.split(":", 1)[1].strip()
        if not raw_id or not value.strip():
            continue
        # Skip ability / UI clutter that isn't a unit TypeId-ish token.
        up = raw_id.upper()
        if up.startswith("ABILITY") or up.startswith("PLAYERPOWER"):
            continue
        if up.endswith("TRANSFORM") or up.endswith("TRANSFORMOFF"):
            continue
        if up.endswith("SUBTAB") or up.endswith("TAB"):
            continue
        tid = camelize_key(raw_id, known_map)
        names[tid] = value.strip()
    # Aliases: copy display text under alternate keys.
    lower = {k.lower(): (k, v) for k, v in names.items()}
    for alias, canon in ALIASES.items():
        hit = lower.get(canon.lower())
        if hit:
            names.setdefault(alias, hit[1])
    return names


def find_lang_big(ra3_root: Path) -> Path:
    data = ra3_root / "Data"
    # Prefer highest Lang-Chinese* patch; fall back to ChineseT.big / ChineseS.big.
    cands: list[Path] = []
    for pat in ("Lang-ChineseS*.big", "Lang-ChineseT*.big", "ChineseS.big", "ChineseT.big"):
        cands.extend(sorted(data.glob(pat)))
    if not cands:
        raise FileNotFoundError(f"no Chinese language BIG under {data}")

    def rank(p: Path) -> tuple:
        name = p.name.lower()
        # Prefer Simplified if present, then highest patch number.
        simp = 0 if "chineses" in name else 1
        m = re.search(r"(\d+)\.big$", name)
        num = int(m.group(1)) if m else -1
        return (simp, -num, name)

    cands.sort(key=rank)
    return cands[0]


def write_map(path: Path, mapping: dict[str, str], header_lines: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = list(header_lines)
    for k in sorted(mapping.keys(), key=lambda s: (0 if not s.lower().startswith(("tpl_", "sig_")) else 1, s.lower())):
        lines.append(f"{k}={mapping[k]}")
    data = ("\r\n".join(lines) + "\r\n").encode("utf-8")
    path.write_bytes(b"\xef\xbb\xbf" + data)


def load_existing_map(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    raw = path.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    out: dict[str, str] = {}
    for line in raw.decode("utf-8", "replace").splitlines():
        s = line.strip()
        if not s or s[0] in "#;" or "=" not in s:
            continue
        k, v = s.split("=", 1)
        k, v = k.strip(), v.strip()
        if k and v:
            out[k] = v
    return out


def guess_ra3_root() -> Path | None:
    env = os.environ.get("RA3_ROOT") or os.environ.get("RA3_PATH")
    if env and Path(env).is_dir():
        return Path(env)
    guesses = [
        Path(r"D:\Steam\steamapps\common\Command and Conquer Red Alert 3"),
        Path(r"C:\Program Files (x86)\Steam\steamapps\common\Command and Conquer Red Alert 3"),
        Path(r"E:\Steam\steamapps\common\Command and Conquer Red Alert 3"),
    ]
    steam = os.environ.get("STEAM_PATH") or os.environ.get("STEAMPATH")
    if steam:
        guesses.insert(0, Path(steam) / "steamapps/common/Command and Conquer Red Alert 3")
    for g in guesses:
        if (g / "Data").is_dir():
            return g
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ra3", type=Path, help="RA3 install root")
    ap.add_argument(
        "--out",
        type=Path,
        default=None,
        help="output unit_names_csf.txt (default: repo root)",
    )
    ap.add_argument(
        "--merge",
        action="store_true",
        help="fill missing keys in unit_names.txt from CSF (never overwrite non-tpl user keys)",
    )
    args = ap.parse_args()

    here = Path(__file__).resolve()
    repo = here.parents[2]  # overlay/tools -> repo
    # Packaged layout: files sit beside the DLL in overlay/bin/
    out_csf = args.out or (repo / "overlay" / "bin" / "unit_names_csf.txt")
    user_path = repo / "overlay" / "bin" / "unit_names.txt"
    # Keep a copy at repo root for editing convenience when --merge
    user_path_root = repo / "unit_names.txt"

    ra3 = args.ra3 or guess_ra3_root()
    if not ra3:
        print("ERROR: RA3 install not found. Pass --ra3 PATH", file=sys.stderr)
        return 1

    big = find_lang_big(ra3)
    print(f"Using {big}")
    raw = read_big_entry(big)
    if raw[:2] == b"\x10\xfb" or (len(raw) > 1 and raw[1] == 0xFB):
        csf_bytes = refpack_decompress(raw)
        print(f"RefPack -> CSF {len(csf_bytes)} bytes")
    elif raw[:4] == b" FSC":
        csf_bytes = raw
    else:
        print(f"ERROR: unexpected gamestrings payload {raw[:8]!r}", file=sys.stderr)
        return 1

    csf = parse_csf(csf_bytes)
    names = extract_name_map(csf)
    print(f"CSF labels={len(csf)} Name: entries kept={len(names)}")

    # Sanity prints
    for k in (
        "SovietAntiVehicleVehicleTech3",
        "SovietAntiNavyShipTech2",
        "SovietAntiVehicleInfantry",
        "HospitalTechBuilding",
        "GarageTechStructure",
    ):
        print(f"  {k} => {names.get(k, names.get(k.upper(), '(missing)'))}")

    write_map(
        out_csf,
        names,
        [
            "# RA3 Overlay — CSF Name:* export (auto-generated)",
            f"# Source: {big}",
            "# Place beside ra3_overlay_v4.dll (overlay\\bin\\)",
            "# Do not hand-edit; re-run: python overlay/tools/extract_csf_names.py",
            "# Runtime loads this first; unit_names.txt overrides win.",
        ],
    )
    print(f"Wrote {out_csf} ({len(names)} entries)")
    # Mirror to repo root for convenience.
    root_csf = repo / "unit_names_csf.txt"
    if root_csf.resolve() != out_csf.resolve():
        write_map(
            root_csf,
            names,
            [
                "# RA3 Overlay — CSF Name:* export (mirror of overlay\\bin\\unit_names_csf.txt)",
                f"# Source: {big}",
                "# Runtime prefers the copy beside the DLL.",
            ],
        )

    if args.merge:
        user = load_existing_map(user_path)
        if not user and user_path_root.exists():
            user = load_existing_map(user_path_root)
        added = 0
        for k, v in names.items():
            if any(uk.lower() == k.lower() for uk in user):
                continue
            user[k] = v
            added += 1
        header = [
            "# RA3 Overlay 单位种类名（UTF-8）",
            "# 与 DLL 同目录：overlay\\bin\\unit_names.txt",
            "# 格式: TypeId=显示名 / sig_* / tpl_*",
            "# CSF 基底: unit_names_csf.txt（同目录）；本文件覆盖优先",
            "# 可在游戏内点「修改」保存，也可手动编辑本文件。",
        ]
        write_map(user_path, user, header)
        if user_path_root.resolve() != user_path.resolve():
            write_map(user_path_root, user, header)
        print(f"Merged into {user_path}: +{added} new keys, total {len(user)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
