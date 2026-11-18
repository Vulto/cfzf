#!/usr/bin/env python3
"""Differential check of --tmux / --popup against the pinned fzf binary.

Compares the tmux and zellij command lines, not a live popup. Temp-file
suffixes differ (digits versus mkstemp) and are normalized away.
"""
import os, re, subprocess, sys, textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMPL = os.environ.get("IMPL", str(ROOT / "cfzf"))
if os.environ.get("REF"):
    REF = os.environ["REF"]
elif os.environ.get("FZF_REF"):
    REF = os.environ["FZF_REF"]
else:
    REF = subprocess.check_output([str(ROOT / "tests" / "ref.sh")], text=True).strip()
BINDIR = Path("/tmp/cfzf-tmuxbin")
BINDIR.mkdir(parents=True, exist_ok=True)

(BINDIR / "tmux").write_text(textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, sys
    log = os.environ["TMUX_CAPTURE"]
    with open(log, "a") as f:
        f.write("ARGV\\n")
        for a in sys.argv[1:]:
            f.write(a + "\\n")
        f.write("ENDARGV\\n")
        for a in sys.argv[1:]:
            if a and os.path.isfile(a):
                f.write("FILE\\n")
                f.write(open(a, errors="replace").read())
                f.write("ENDFILE\\n")
    if "display-message" in sys.argv and os.environ.get("TMUX_FLOAT") == "1":
        sys.stdout.write("80 24\\nnew-pane\\n")
    sys.exit(0)
"""))
(BINDIR / "sh").write_text(textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, re, sys
    log = os.environ.get("TMUX_CAPTURE")
    if os.environ.get("TMUX_FLOAT_DRY") == "1" and len(sys.argv) >= 3 and sys.argv[1] == "-c":
        script = sys.argv[2]
        with open(log, "a") as f:
            f.write("SH\\n")
            f.write(script)
            f.write("\\nENDSH\\n")
            for m in re.findall(r"/tmp/fzf-temp-[A-Za-z0-9]+", script):
                if os.path.isfile(m):
                    f.write("FILE\\n")
                    f.write(open(m, errors="replace").read())
                    f.write("ENDFILE\\n")
        sys.exit(0)
    os.execv("/bin/sh", ["/bin/sh"] + sys.argv[1:])
"""))
(BINDIR / "zellij").write_text(textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, re, sys
    log = os.environ["TMUX_CAPTURE"]
    with open(log, "a") as f:
        f.write("ZELLIJ\\n")
        for a in sys.argv[1:]:
            f.write(a + "\\n")
        f.write("ENDZELLIJ\\n")
        blob = "\\n".join(sys.argv[1:])
        for m in re.findall(r"/tmp/fzf-temp-[A-Za-z0-9]+", blob):
            if os.path.isfile(m):
                f.write("FILE\\n")
                f.write(open(m, errors="replace").read())
                f.write("ENDFILE\\n")
    sys.exit(0)
"""))
for name in ("tmux", "sh", "zellij"):
    os.chmod(BINDIR / name, 0o755)

def norm(text, binary):
    text = text.replace(binary, "BIN")
    text = re.sub(r"/tmp/fzf-proxy-(?:input|output)-\d+", "FIFO", text)
    text = re.sub(r"/tmp/fzf-temp-[A-Za-z0-9]+", "TEMP", text)
    text = re.sub(r"fzf-fzf-temp-[A-Za-z0-9]+", "SIG", text)
    return text

def interesting(capture, binary):
    """ARGV block, last command line, and the floating-pane shell script."""
    if not capture:
        return ""
    text = norm(capture, binary)
    out = []
    if "ARGV\n" in text:
        block = text.split("ARGV\n", 1)[1].split("ENDARGV", 1)[0]
        lines = []
        for ln in block.splitlines():
            if "/tmp/fzf-temp-" in ln or ln.startswith("TEMP"):
                lines.append("TEMP")
            else:
                lines.append(ln)
        # The probe and the popup may both be present. Keep every ARGV.
        out.append("ARGV\n" + "\n".join(lines))
    # All ARGV blocks, not just the first. Re-scan.
    blocks = []
    rest = text
    while "ARGV\n" in rest:
        rest = rest.split("ARGV\n", 1)[1]
        block, _, rest = rest.partition("ENDARGV")
        lines = []
        for ln in block.splitlines():
            lines.append("TEMP" if ln == "TEMP" or ln.startswith("/tmp/fzf-temp") else ln)
        blocks.append("ARGV\n" + "\n".join(lines).strip())
    out = blocks
    if "SH\n" in text:
        sh = text.split("SH\n", 1)[1].split("ENDSH", 1)[0].strip()
        out.append("SH\n" + sh)
    if "ZELLIJ\n" in text:
        z = text.split("ZELLIJ\n", 1)[1].split("ENDZELLIJ", 1)[0].strip()
        out.append("ZELLIJ\n" + z)
    if "FILE\n" in text:
        body = text.split("FILE\n", 1)[1].split("ENDFILE", 1)[0]
        cmd = ""
        for ln in body.splitlines():
            if "BIN" in ln or "--no-popup" in ln:
                cmd = ln
        head = body.splitlines()[:3]
        out.append("HEAD\n" + "\n".join(head))
        out.append("CMD\n" + cmd)
    return "\n".join(out).strip() + "\n"

def run(binary, args, extra_env, stdin=b"a\n"):
    cap = f"/tmp/tmux-cap-{os.getpid()}-{os.path.basename(binary)}.txt"
    try:
        os.remove(cap)
    except FileNotFoundError:
        pass
    env = os.environ.copy()
    env.pop("TMUX_PANE", None)
    env.pop("ZELLIJ", None)
    env["PATH"] = str(BINDIR) + os.pathsep + env.get("PATH", "")
    env["TMUX_CAPTURE"] = cap
    env.update(extra_env)
    proc = subprocess.run([binary, *args], input=stdin, capture_output=True,
                          env=env, timeout=8, cwd="/home/vulto/C/cfzf")
    try:
        got = Path(cap).read_text(errors="replace")
    except FileNotFoundError:
        got = ""
    return proc.returncode, proc.stdout, proc.stderr, got

fails = 0

def check(name, args, extra, stdin=b"a\n", compare_io=False, allow_stderr_diff=False):
    global fails
    rc1, out1, err1, cap1 = run(REF, args, extra, stdin)
    rc2, out2, err2, cap2 = run(IMPL, args, extra, stdin)
    a = interesting(cap1, REF)
    b = interesting(cap2, IMPL)
    ok = rc1 == rc2 and a == b
    if compare_io:
        ok = ok and out1 == out2 and (allow_stderr_diff or err1 == err2)
    if not compare_io and not allow_stderr_diff and not extra.get("TMUX") and not extra.get("ZELLIJ"):
        ok = ok and err1 == err2 and out1 == out2
    # Error-only cases have no multiplexer env: compare stderr always.
    if "TMUX" not in extra and "ZELLIJ" not in extra:
        ok = rc1 == rc2 and out1 == out2 and err1 == err2 and a == b
    if ok:
        print("PASS", name)
    else:
        fails += 1
        print("FAIL", name)
        print("  ref rc", rc1, "impl rc", rc2)
        if out1 != out2:
            print("  stdout ref", out1, "impl", out2)
        if err1 != err2:
            print("  stderr ref", err1)
            print("  stderr impl", err2)
        if a != b:
            print("--- ref capture ---")
            print(a)
            print("--- impl capture ---")
            print(b)

# Parse errors, no multiplexer.
check("invalid", ["--tmux=nope"], {}, stdin=b"", compare_io=True)
check("empty", ["--tmux="], {}, stdin=b"", compare_io=True)
check("five", ["--tmux=top,10%,20%,border-native,extra"], {}, stdin=b"", compare_io=True)
check("huge", ["--tmux=101%"], {}, stdin=b"", compare_io=True)
check("filter-top", ["--filter=a", "--tmux=top,30%"], {}, stdin=b"a\nb\n", compare_io=True)
check("filter-native", ["--filter=a", "--tmux=border-native"], {}, stdin=b"a\n", compare_io=True)
check("filter-colon", ["--filter=b", "--tmux=left:20"], {}, stdin=b"a\n", compare_io=True)

TM = {"TMUX": "1"}
check("default", ["--tmux"], TM)
check("popup-alias", ["--popup"], TM)
check("top", ["--tmux=top"], TM)
check("bottom", ["--tmux=bottom,30%"], TM)
check("left", ["--tmux=left,20"], TM)
check("right", ["--tmux=right,20,30"], TM)
check("center", ["--tmux=center,40%,25"], TM)
check("pct", ["--tmux=30%"], TM)
check("native", ["--tmux=border-native"], TM)
check("line", ["--border=line", "--tmux"], TM)
check("sharp", ["--border=sharp", "--tmux"], TM)
check("none", ["--no-border", "--tmux"], TM)
check("margin", ["--margin=1", "--tmux=border-native"], TM)
check("height-before", ["--height=10%", "--tmux"], TM)
check("up-alias", ["--tmux=up,15"], TM)
check("down-alias", ["--tmux=down"], TM)
check("border-native-top", ["--popup=top,border-native"], TM)
check("two-unknown", ["--tmux=30%,40"], TM)

# Later --height or --no-tmux suppresses the popup. Both fail the TUI.
check("height-after", ["--tmux", "--height=10%"], TM, allow_stderr_diff=True)
check("no-tmux-after", ["--tmux", "--no-tmux"], TM, allow_stderr_diff=True)
check("no-popup-after", ["--popup", "--no-popup"], TM, allow_stderr_diff=True)

# Filter never launches, even inside tmux.
check("filter-in-tmux", ["--filter=a", "--tmux"], TM, stdin=b"a\nb\n", compare_io=True)

FLOAT = {"TMUX": "1", "TMUX_PANE": "%1", "TMUX_FLOAT": "1", "TMUX_FLOAT_DRY": "1"}
check("float-default", ["--tmux"], FLOAT)
check("float-top", ["--tmux=top,40%"], FLOAT)
check("float-native-sharp", ["--border=sharp", "--tmux=border-native"], FLOAT)
check("float-none", ["--no-border", "--tmux"], FLOAT)

Z = {"ZELLIJ": "1"}
check("zellij-top", ["--popup=top,20%"], Z)
check("zellij-default", ["--tmux"], Z)
check("zellij-sharp", ["--border=sharp", "--popup"], Z)
check("zellij-right", ["--popup=right,30,40%"], Z)

# tmux wins over zellij.
BOTH = {"TMUX": "1", "ZELLIJ": "1"}
check("tmux-over-zellij", ["--tmux=left,25"], BOTH)

print("FAILS", fails)
sys.exit(1 if fails else 0)
