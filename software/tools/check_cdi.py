#!/usr/bin/env python3
"""Check the FemtoLCC CDI against the configuration layout in Config.h.

The CDI (software/FemtoLCC/Cdi.cpp) tells a configuration tool where each
setting lives in memory space 253; Config.h tells the firmware. Nothing ties the
two together at compile time, so this walks the CDI the way a tool does -
<int> is its size, <eventid> 8 bytes, <string> its size, group offset attributes
skip bytes, replication multiplies a group - and compares every field with the
constant in Config.h. Replicated groups may nest (the expansion boards' lines
and channels); each group's replication and stride are checked against their
constants too. It also checks the XML is well-formed.

    python3 software/tools/check_cdi.py

Exits non-zero on any mismatch.
"""

import pathlib
import re
import sys
import xml.etree.ElementTree as ET

HERE = pathlib.Path(__file__).resolve().parent
SKETCH = HERE.parent / "FemtoLCC"
CONFIG_H = SKETCH / "Config.h"
BOARD_H = SKETCH / "board.h"
CDI_CPP = SKETCH / "Cdi.cpp"

# Fallback for the AOLCB fragment, used when ConfigStorage.h cannot be found.
USER_INFO_FALLBACK = (
    "<segment space='251' origin='1'><name>Node</name>"
    "<string size='63'><name>Name</name></string>"
    "<string size='64'><name>Description</name></string>"
    "</segment>"
)

# CDI path -> (Config.h offset expression, size). Paths inside a replicated
# group describe its first replica; the stride is checked separately.
CH = "CFG_CHANNELS + "
PIN = "CFG_PINS + "
WIFI = "CFG_WIFI + "
XIO = "CFG_XIO + "
XLINE = "CFG_XIO + CFG_XIO_LINES + "
SVB = "CFG_SVB + "
SV = "CFG_SVB + CFG_SVB_CHANNELS + "
PD = "CFG_PULSE + "
XB = "I/O expansion boards/"
SB = "Servo and light boards/"
FIELDS = {
    "Outputs/Use":                          (CH + "CFG_CH_ROLE", 1),
    "Outputs/Fault":                        (CH + "CFG_CH_EV_FAULT", 8),
    "Outputs/Track block/At power-on":      (CH + "CFG_CH_BLK_POWER_ON", 1),
    "Outputs/Track block/DCC polarity":     (CH + "CFG_CH_BLK_DCC_REVERSED", 1),
    "Outputs/Track block/Occupied at (mA)": (CH + "CFG_CH_BLK_OCCUPIED_MA", 2),
    "Outputs/Track block/Clear below (mA)": (CH + "CFG_CH_BLK_CLEAR_MA", 2),
    "Outputs/Track block/Power on (DC)":    (CH + "CFG_CH_BLK_EV_ON", 8),
    "Outputs/Track block/Power off":        (CH + "CFG_CH_BLK_EV_OFF", 8),
    "Outputs/Track block/DCC on":           (CH + "CFG_CH_BLK_EV_DCC", 8),
    "Outputs/Track block/Occupied":         (CH + "CFG_CH_BLK_EV_OCCUPIED", 8),
    "Outputs/Track block/Clear":            (CH + "CFG_CH_BLK_EV_CLEAR", 8),
    "Outputs/Turnout motor/Motor type":     (CH + "CFG_CH_TO_MOTOR", 1),
    "Outputs/Turnout motor/Pulse length (ms)": (CH + "CFG_CH_TO_PULSE_MS", 2),
    "Outputs/Turnout motor/Drive strength": (CH + "CFG_CH_TO_DUTY", 1),
    "Outputs/Turnout motor/Direction":      (CH + "CFG_CH_TO_REVERSE", 1),
    "Outputs/Turnout motor/At power-on":    (CH + "CFG_CH_TO_POWER_ON", 1),
    "Outputs/Turnout motor/Throw":          (CH + "CFG_CH_TO_EV_THROW", 8),
    "Outputs/Turnout motor/Close":          (CH + "CFG_CH_TO_EV_CLOSE", 8),
    "Outputs/Turnout motor/Thrown":         (CH + "CFG_CH_TO_EV_THROWN", 8),
    "Outputs/Turnout motor/Closed":         (CH + "CFG_CH_TO_EV_CLOSED", 8),
    "I/O pins/Mode":                        (PIN + "CFG_PIN_MODE", 1),
    "I/O pins/Polarity":                    (PIN + "CFG_PIN_INVERT", 1),
    "I/O pins/Debounce (ms)":               (PIN + "CFG_PIN_DEBOUNCE_MS", 2),
    "I/O pins/Input active":                (PIN + "CFG_PIN_EV_ACTIVE", 8),
    "I/O pins/Input inactive":              (PIN + "CFG_PIN_EV_INACTIVE", 8),
    "I/O pins/Output on":                   (PIN + "CFG_PIN_EV_ON", 8),
    "I/O pins/Output off":                  (PIN + "CFG_PIN_EV_OFF", 8),
    "WiFi/WiFi":                            (WIFI + "CFG_WIFI_ENABLE", 1),
    "WiFi/Network name (SSID)":             (WIFI + "CFG_WIFI_SSID", "CFG_WIFI_SSID_SIZE"),
    "WiFi/Password":                        (WIFI + "CFG_WIFI_PASSWORD", "CFG_WIFI_PASSWORD_SIZE"),
    "WiFi/Hostname":                        (WIFI + "CFG_WIFI_HOSTNAME", "CFG_WIFI_HOSTNAME_SIZE"),
    "WiFi/Hub mode":                        (WIFI + "CFG_WIFI_HUB_MODE", 1),
    "WiFi/Hub address":                     (WIFI + "CFG_WIFI_HUB_HOST", "CFG_WIFI_HUB_HOST_SIZE"),
    "WiFi/TCP port":                        (WIFI + "CFG_WIFI_HUB_PORT", 2),
    "USB/GridConnect output":               ("CFG_USB + CFG_USB_GRIDCONNECT", 1),
    XB + "Board type":                      (XIO + "CFG_XIO_TYPE", 1),
    XB + "I2C address":                     (XIO + "CFG_XIO_ADDRESS", 1),
    XB + "Lines/Mode":                      (XLINE + "CFG_PIN_MODE", 1),
    XB + "Lines/Polarity":                  (XLINE + "CFG_PIN_INVERT", 1),
    XB + "Lines/Debounce (ms)":             (XLINE + "CFG_PIN_DEBOUNCE_MS", 2),
    XB + "Lines/Input active":              (XLINE + "CFG_PIN_EV_ACTIVE", 8),
    XB + "Lines/Input inactive":            (XLINE + "CFG_PIN_EV_INACTIVE", 8),
    XB + "Lines/Output on":                 (XLINE + "CFG_PIN_EV_ON", 8),
    XB + "Lines/Output off":                (XLINE + "CFG_PIN_EV_OFF", 8),
    SB + "Board type":                      (SVB + "CFG_SVB_TYPE", 1),
    SB + "I2C address":                     (SVB + "CFG_SVB_ADDRESS", 1),
    SB + "Servos at rest":                  (SVB + "CFG_SVB_HOLD", 1),
    SB + "Channels/Use":                    (SV + "CFG_SV_USE", 1),
    SB + "Channels/Throw / light on":       (SV + "CFG_SV_EV_THROW", 8),
    SB + "Channels/Close / light off":      (SV + "CFG_SV_EV_CLOSE", 8),
    SB + "Channels/Travel or fade time (ms)": (SV + "CFG_SV_TIME_MS", 2),
    SB + "Channels/Servo turnout/Closed position (µs)": (SV + "CFG_SV_CLOSED_US", 2),
    SB + "Channels/Servo turnout/Thrown position (µs)": (SV + "CFG_SV_THROWN_US", 2),
    SB + "Channels/Servo turnout/Thrown":   (SV + "CFG_SV_EV_THROWN", 8),
    SB + "Channels/Servo turnout/Closed":   (SV + "CFG_SV_EV_CLOSED", 8),
    SB + "Channels/Light/Brightness":       (SV + "CFG_SV_BRIGHTNESS", 1),
    "Detection while off/Detect while off": (PD + "CFG_PD_MODE", 1),
    "Detection while off/Pulse length (100 µs)": (PD + "CFG_PD_LENGTH", 1),
    "Detection while off/Pulse interval (ms)":   (PD + "CFG_PD_INTERVAL", 2),
}

# Replicated groups: CDI path -> (replication constant, stride constant).
GROUPS = {
    "Outputs":                  ("NUM_CHANNELS", "CFG_CH_SIZE"),
    "I/O pins":                 ("EXP_GPIO_COUNT", "CFG_PIN_SIZE"),
    XB.rstrip("/"):             ("NUM_XIO_BOARDS", "CFG_XIO_SIZE"),
    XB + "Lines":               ("XIO_LINES", "CFG_PIN_SIZE"),
    SB.rstrip("/"):             ("NUM_SERVO_BOARDS", "CFG_SVB_SIZE"),
    SB + "Channels":            ("SERVO_CHANNELS", "CFG_SV_SIZE"),
    "Detection while off":      ("NUM_CHANNELS", "CFG_PD_SIZE"),
}

# Non-replicated groups whose total size is a constant.
SIZES = {"WiFi": "CFG_WIFI_SIZE", "USB": "CFG_USB_SIZE"}


def c_strings(src):
    """Contents of adjacent C string literals, with escapes resolved."""
    out = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', src):
        out.append(bytes(m.group(1), "utf-8").decode("unicode_escape"))
    return "".join(out)


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def load_constants():
    """Integer constants from board.h and Config.h, evaluated in order."""
    env = {}
    for path in (BOARD_H, CONFIG_H):
        src = strip_comments(path.read_text())
        for name, expr in re.findall(
                r"static\s+const\s+\w+\s+(\w+)\s*=\s*([^;{]+);", src):
            expr = re.sub(r"(?<=\d)[uUlL]+\b", "", expr)
            try:
                env[name] = int(eval(expr, {}, dict(env)))
            except Exception:
                pass    # arrays and anything non-numeric
    return env


def string_macros():
    macros = {}
    for path in SKETCH.glob("*.h"):
        for name, body in re.findall(r'#define\s+(\w+)\s+((?:"[^"\n]*"\s*)+)', path.read_text()):
            macros[name] = c_strings(body)
    return macros


def user_info_fragment():
    for base in (pathlib.Path.home() / "git" / "AOLCB", HERE.parents[2] / "AOLCB"):
        header = base / "src" / "ConfigStorage.h"
        if header.exists():
            m = re.search(r"#define\s+AOLCB_CDI_USER_INFO\s*((?:\\\n|[^\n])*)", header.read_text())
            if m:
                return c_strings(m.group(1).replace("\\\n", " "))
    return USER_INFO_FALLBACK


def tokenize(src):
    """Split C source into ('str', text) literals and ('code', text) runs,
    dropping comments. Comment markers inside literals (a URL) are kept."""
    out, i, code = [], 0, []
    while i < len(src):
        c = src[i]
        if src.startswith("//", i):
            i = src.find("\n", i)
            i = len(src) if i < 0 else i
        elif src.startswith("/*", i):
            i = src.find("*/", i) + 2
        elif c == '"':
            if code:
                out.append(("code", "".join(code)))
                code = []
            j = i + 1
            while src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append(("str", bytes(src[i + 1:j], "utf-8").decode("unicode_escape")))
            i = j + 1
        elif c == "'":
            j = src.find("'", i + 1)
            code.append(src[i:j + 1])
            i = j + 1
        else:
            code.append(c)
            i += 1
    if code:
        out.append(("code", "".join(code)))
    return out


def load_cdi():
    toks = tokenize(CDI_CPP.read_text())
    macros = string_macros()
    macros["AOLCB_CDI_USER_INFO"] = user_info_fragment()
    # Find "CDI_XML[] =", then join literals and expanded macros up to ';'.
    start = None
    for k, (kind, text) in enumerate(toks):
        if kind == "code" and re.search(r"CDI_XML\[\]\s*=", text):
            start = k
            break
    if start is None:
        sys.exit("CDI_XML not found in Cdi.cpp")
    xml = []
    first = re.split(r"CDI_XML\[\]\s*=", toks[start][1], maxsplit=1)[1]
    for kind, text in [("code", first)] + toks[start + 1:]:
        if kind == "str":
            xml.append(text)
            continue
        done = ";" in text
        for name in re.findall(r"\b[A-Za-z_]\w*\b", text.split(";")[0]):
            if name not in macros:
                sys.exit(f"unknown macro {name} in CDI_XML")
            xml.append(macros[name])
        if done:
            break
    return "".join(xml)


def element_size(el):
    tag = el.tag
    if tag == "int":
        return int(el.get("size", "1"))
    if tag == "eventid":
        return 8
    if tag == "string":
        return int(el.get("size"))
    if tag == "float":
        return int(el.get("size", "4"))
    return None


def name_of(el):
    n = el.find("name")
    return n.text.strip() if n is not None and n.text else ""


class Walker:
    def __init__(self):
        self.leaves = []      # (path, offset, size)
        self.groups = {}      # name -> (offset, replica size, replication)

    def walk(self, el, origin, path, record=True):
        """Walk the children of el starting at origin; return the end offset."""
        pos = origin
        for child in el:
            if child.tag in ("name", "description", "repname", "hints", "map",
                             "min", "max", "default"):
                continue
            pos += int(child.get("offset", "0"))
            if child.tag == "group":
                rep = int(child.get("replication", "1"))
                gname = name_of(child)
                gpath = f"{path}/{gname}" if path else gname
                start = pos
                end = self.walk(child, start, gpath, record)
                size = end - start
                if gname:
                    self.groups[gpath] = (start, size, rep)
                # Later replicas are the same shape; walk them only for offsets.
                pos = start + size * rep
            else:
                size = element_size(child)
                if size is None:
                    raise ValueError(f"unhandled CDI element <{child.tag}> at {path}")
                if record:
                    self.leaves.append((f"{path}/{name_of(child)}", pos, size))
                pos += size
        return pos


def main():
    errors = []
    xml = load_cdi()
    try:
        root = ET.fromstring(xml)
    except ET.ParseError as e:
        print(f"CDI is not well-formed XML: {e}")
        return 1
    print(f"CDI well-formed, {len(xml)} bytes")

    env = load_constants()

    def value(expr):
        if isinstance(expr, int):
            return expr
        return int(eval(expr, {}, dict(env)))

    segments = [s for s in root.findall("segment") if s.get("space") == "253"]
    if len(segments) != 1:
        print("expected exactly one segment for space 253")
        return 1
    seg = segments[0]
    w = Walker()
    end = w.walk(seg, int(seg.get("origin", "0")), "")
    if end > env["CFG_SPACE_SIZE"]:
        errors.append(f"CDI runs to {end}, past CFG_SPACE_SIZE {env['CFG_SPACE_SIZE']}")
    if end != env["CFG_END"]:
        errors.append(f"CDI ends at {end}, Config.h CFG_END is {env['CFG_END']}")

    seen = set()
    for path, off, size in w.leaves:
        if path in seen:
            errors.append(f"duplicate CDI path {path}")
        seen.add(path)
        if path not in FIELDS:
            errors.append(f"{path} @ {off} has no entry in check_cdi.py FIELDS")
            continue
        expr, want_size = FIELDS[path]
        want = value(expr)
        want_size = value(want_size)
        status = "ok"
        if off != want or size != want_size:
            status = "MISMATCH"
            errors.append(f"{path}: CDI offset {off} size {size}, "
                          f"Config.h {expr} = {want} size {want_size}")
        print(f"  {off:4d} +{size:<2d} {path:70s} {status}")
    for path in FIELDS:
        if path not in seen:
            errors.append(f"{path} is in FIELDS but not in the CDI")

    for gname, (rep_const, stride_const) in GROUPS.items():
        if gname not in w.groups:
            errors.append(f"group {gname} missing from CDI")
            continue
        _, size, rep = w.groups[gname]
        if rep != env[rep_const]:
            errors.append(f"group {gname}: replication {rep}, {rep_const} is {env[rep_const]}")
        if size != env[stride_const]:
            errors.append(f"group {gname}: replica is {size} bytes, {stride_const} is {env[stride_const]}")
    for gname, const in SIZES.items():
        if gname in w.groups and w.groups[gname][1] != env[const]:
            errors.append(f"group {gname}: {w.groups[gname][1]} bytes, {const} is {env[const]}")

    if errors:
        print("\nFAILED:")
        for e in errors:
            print("  " + e)
        return 1
    print(f"\nall {len(w.leaves)} fields match Config.h; layout ends at {end} of {env['CFG_SPACE_SIZE']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
