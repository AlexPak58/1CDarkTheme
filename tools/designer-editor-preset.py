"""Apply or restore the native Designer module editor colors."""

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


COLOR_KEYS = (
    "Keywords", "Numerics", "Strings", "Dates", "Identifiers", "Operators",
    "Comments", "Preprocessor", "Others", "Background", "CurrentToken",
    "CurrentSelection", "PairLexeme", "SearchResult", "AutoAssistBackground",
)
TOKEN = re.compile(r'"(?:[^"]|"")*"|[{}]')
SECTION = re.compile(r'\{\s*"ModuleColorCategory"\s*,')
RGB = re.compile(r"#[0-9a-fA-F]{6}")


def direct_blocks(text):
    depth = 0
    start = 0
    if not text.lstrip().startswith("{") or not text.rstrip().endswith("}"):
        raise ValueError("Expected a 1C profile block.")
    for token in TOKEN.finditer(text):
        if token[0] == "{":
            depth += 1
            if depth == 2:
                start = token.start()
        elif token[0] == "}":
            if depth == 2:
                yield start, token.end()
            depth -= 1
            if depth < 0 or (depth == 0 and text[token.end():].strip()):
                raise ValueError("Unexpected data outside the profile block.")
    if depth:
        raise ValueError("Unbalanced profile blocks.")


def find_section(text):
    root_children = list(direct_blocks(text))
    if len(root_children) != 2 or text[slice(*root_children[0])] != '{""}':
        raise ValueError("Unexpected 1C profile layout.")
    offset, end = root_children[1]
    children = [(offset + a, offset + b) for a, b in direct_blocks(text[offset:end])]
    if not children or text[slice(*children[-1])] != '{""}':
        raise ValueError("Missing profile section terminator.")
    sections = [span for span in children[:-1] if SECTION.match(text, span[0], span[1])]
    if len(sections) > 1:
        raise ValueError("Duplicate module color sections.")
    return sections[0] if sections else None, children[-1][0]


def replace_section(text, new_section):
    span, terminator = find_section(text)
    if span:
        start, end = span
        if new_section is None:
            separator = re.match(r",\s*", text[end:])
            if not separator:
                raise ValueError("Missing module color section separator.")
            return text[:start] + text[end + separator.end():]
        return text[:start] + new_section + text[end:]
    if new_section is None:
        return text
    newline = "\r\n" if "\r\n" in text else "\n"
    return text[:terminator] + new_section + "," + newline + text[terminator:]


def color_section(colors, newline):
    if set(colors) != set(COLOR_KEYS) or any(
        not isinstance(rgb, str) or not RGB.fullmatch(rgb) for rgb in colors.values()
    ):
        raise ValueError("The preset must supply each of the 15 native RGB colors.")
    entries = []
    for key in COLOR_KEYS:
        rgb = colors[key]
        colorref = int(rgb[1:3], 16) | int(rgb[3:5], 16) << 8 | int(rgb[5:7], 16) << 16
        entries.append(f'"{key}",{{"#",9cd510c7-abfc-11d4-9434-004095e12fc7,2,{{3,0,{{{colorref}}}}}}}')
    return newline.join(('{"ModuleColorCategory",',
                         '{' + (',' + newline).join(entries) + ',""},',
                         '{{""}}', '}'))


def require_closed_designer():
    command = (
        "$ErrorActionPreference='Stop'; "
        "Get-CimInstance Win32_Process -Filter \"Name='1cv8.exe'\" | "
        "Where-Object { !$_.CommandLine -or $_.CommandLine -match '(?i)(^|\\s)DESIGNER(\\s|$)' } | "
        "ForEach-Object ProcessId"
    )
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                            capture_output=True, text=True, check=True)
    if result.stdout.strip():
        raise RuntimeError("Close the remaining Designer windows before writing the shared profile "
                           f"(PID {', '.join(result.stdout.split())}).")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--preset", type=Path)
    source.add_argument("--restore-from", type=Path)
    parser.add_argument("--apply", action="store_true", help="Write the profile. Default: preview.")
    args = parser.parse_args()
    profile = Path(os.environ["APPDATA"]) / "1C/1cv8/1cv8.pfl"
    original = profile.read_bytes()
    if not original.startswith(b"\xef\xbb\xbf"):
        raise ValueError("Expected the native UTF-8 BOM in 1cv8.pfl.")
    text = original.decode("utf-8-sig")
    if args.preset:
        document = json.loads(args.preset.read_text(encoding="utf-8-sig"))
        replacement = color_section(document["colors"], "\r\n" if "\r\n" in text else "\n")
    else:
        backup_text = args.restore_from.read_bytes().decode("utf-8-sig")
        span, _ = find_section(backup_text)
        replacement = backup_text[slice(*span)] if span else None
    changed_text = replace_section(text, replacement)
    new_span, _ = find_section(changed_text)
    if (changed_text[slice(*new_span)] if new_span else None) != replacement:
        raise ValueError("Module color section failed verification.")
    if changed_text == text:
        print("The native module palette already matches.")
        return
    if not args.apply:
        print("Preview passed. Only the native ModuleColorCategory section would change.")
        return
    require_closed_designer()
    backup_dir = Path(os.environ["LOCALAPPDATA"]) / "1CDarkTheme"
    backup_dir.mkdir(parents=True, exist_ok=True)
    backup = backup_dir / f"editor-profile-{datetime.now():%Y%m%d-%H%M%S-%f}.pfl"
    with backup.open("xb") as saved:
        saved.write(original)
    with tempfile.NamedTemporaryFile(dir=profile.parent, delete=False) as pending:
        pending.write(changed_text.encode("utf-8-sig"))
    try:
        if profile.read_bytes() != original:
            raise RuntimeError("The original profile changed while preparing the update.")
        require_closed_designer()
        os.replace(pending.name, profile)
    finally:
        Path(pending.name).unlink(missing_ok=True)
    print(f"Native module palette saved. Backup: {backup}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from None
