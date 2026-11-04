#!/usr/bin/env python3
"""Cohérence des réglages entre la DLL (et l'injecteur) et l'outil de configuration.

La DLL lit sr2_vr.ini clé par clé (config::Int / config::Float) avec sa propre valeur par
défaut ; l'outil de configuration (config_tool/settings.cpp) a la sienne, qu'il écrit dans le
fichier. Si les deux divergent, le jeu lancé sans le lanceur (--no-launcher, ou sr2_vr.ini
absent) ne se comporte pas comme le lanceur l'affiche.

Vérifie, pour chaque clé présente des deux côtés, que la valeur par défaut est la même.
Les clés propres à la DLL (mise au point, valeurs apprises) sont seulement listées avec -v.

    python3 tools/check_settings.py        # depuis la racine du dépôt
    python3 tools/check_settings.py -v     # liste aussi les clés d'un seul côté

Code de sortie : 0 si tout concorde, 1 sinon.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Constantes nommées utilisées comme valeur par défaut dans la DLL
NAMED = {"input::vpad::kEast": 1.0}

DLL_READ = re.compile(r'config::(Int|Float)\(\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*((?:[^()]|\([^()]*\))*)\)', re.S)
# Injecteur : GetPrivateProfileIntW(L"Section", L"cle", défaut, ...)
INJ_READ = re.compile(r'GetPrivateProfileIntW\(\s*L"(\w+)"\s*,\s*L"(\w+)"\s*,\s*([^,]+),')
TOOL_DEF = re.compile(r'\{L"(\w+)",\s*L"(\w+)",\s*kTab\w+,\s*\{.*?\},\s*Kind::(\w+),\s*([-\d.]+)f?\s*,', re.S)


def number(text):
    text = text.strip()
    if text in NAMED:
        return NAMED[text]
    try:
        return float(int(text, 0))
    except ValueError:
        pass
    try:
        return float(text.rstrip("f"))
    except ValueError:
        return None  # expression (dépend d'un autre réglage)


def main():
    verbose = "-v" in sys.argv[1:]
    dll = {}
    for path in sorted((ROOT / "src").rglob("*.cpp")):
        for m in DLL_READ.finditer(path.read_text(encoding="utf-8")):
            key = (m.group(2), m.group(3))
            dll.setdefault(key, []).append((path.relative_to(ROOT).as_posix(), m.group(4).strip()))

    for path in sorted((ROOT / "injector").glob("*.cpp")):
        for m in INJ_READ.finditer(path.read_text(encoding="utf-8")):
            dll.setdefault((m.group(1), m.group(2)), []).append((path.relative_to(ROOT).as_posix(), m.group(3).strip()))

    tool = {}
    for m in TOOL_DEF.finditer((ROOT / "config_tool" / "settings.cpp").read_text(encoding="utf-8")):
        tool[(m.group(1), m.group(2))] = (m.group(3), float(m.group(4)))

    if not dll or not tool:
        print("aucun réglage trouvé : les expressions régulières ne correspondent plus au code")
        return 1

    errors = 0
    for key in sorted(set(dll) & set(tool)):
        kind, expected = tool[key]
        for where, text in dll[key]:
            value = number(text)
            if value is None:
                if verbose:
                    print(f"  [{key[0]}] {key[1]} : {where} calcule sa valeur par défaut ({text})")
                continue
            if kind == "Bool":
                value = 1.0 if value != 0 else 0.0
            if abs(value - expected) > 1e-6:
                errors += 1
                print(f"DIVERGENT [{key[0]}] {key[1]} : outil {expected:g}, {where} {text}")

    if verbose:
        for key in sorted(set(dll) - set(tool)):
            print(f"  [{key[0]}] {key[1]} : lu par la DLL seulement")
        for key in sorted(set(tool) - set(dll)):
            print(f"  [{key[0]}] {key[1]} : outil seulement")

    common = len(set(dll) & set(tool))
    print(f"{common} réglages communs, {errors} valeur(s) par défaut divergente(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
