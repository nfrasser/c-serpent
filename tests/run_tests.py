#!/usr/bin/env python3
"""
C-serpent regression tests.

    python3 tests/run_tests.py                     # everything it can run
    python3 tests/run_tests.py -k struct           # only matching tests
    python3 tests/run_tests.py --python PATH       # python to build modules against
    python3 tests/run_tests.py --keep              # keep the temp dir for debugging

There are three kinds of test:

  codegen    run c-serpent over a snippet and check what it emitted, or which
             error it produced. Needs only a C compiler.

  functional actually compile the generated wrapper into an extension module,
             import it and assert on real behaviour. Needs Python headers and
             numpy headers as well.

  notebook   exercise cserpent.py, which reaches c-serpent through the
             _cserpent extension rather than the standalone binary.

The last two are skipped, loudly, if no suitable Python is available.
Pass --python to point at one (a virtualenv or conda env usually has the
headers that a bare system Python does not).
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import cases_codegen
import cases_functional
import cases_notebook

CC = os.environ.get("CC", "cc")


class Failure(Exception):
    pass


# ----------------------------------------------------------------- harness


def build_cserpent(workdir):
    exe = os.path.join(workdir, "cserpent")
    r = subprocess.run([CC, "-g", "-o", exe, os.path.join(ROOT, "cserpent.c")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("FATAL: could not build cserpent.c\n" + r.stderr)
        sys.exit(2)
    return exe


def probe_python(python):
    """Return (include, numpy_include) if this python can build extensions."""
    code = ("import sysconfig, numpy, os, json;"
            "print(json.dumps([sysconfig.get_paths()['include'], numpy.get_include()]))")
    try:
        r = subprocess.run([python, "-c", code], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    import json
    try:
        inc, npinc = json.loads(r.stdout.strip())
    except (ValueError, TypeError):
        return None
    if not os.path.exists(os.path.join(inc, "Python.h")):
        return None
    if not os.path.exists(os.path.join(npinc, "numpy", "arrayobject.h")):
        return None
    return inc, npinc


def run_cserpent(exe, workdir, name, source, args, stdin_text=None):
    src = os.path.join(workdir, name + ".c")
    with open(src, "w") as f:
        f.write(source)
    argv = [exe] + list(args)
    argv = [a.replace("@SRC@", src) for a in argv]
    if not any(a == src or a == "-" for a in argv):
        argv.append(src)
    return subprocess.run(argv, capture_output=True, text=True,
                          input=stdin_text, cwd=workdir)


def check(case, r):
    """Compare a completed run against a case's expectations."""
    want_rc = case.get("rc", 0)
    if r.returncode != want_rc:
        raise Failure("expected rc=%d, got %d\nstderr:\n%s"
                      % (want_rc, r.returncode, r.stderr.strip()[:900]))

    for needle in case.get("out_has", []):
        if needle not in r.stdout:
            raise Failure("stdout missing %r" % needle)
    for needle in case.get("out_lacks", []):
        if needle in r.stdout:
            raise Failure("stdout should not contain %r" % needle)
    for needle in case.get("err_has", []):
        if needle not in r.stderr:
            raise Failure("stderr missing %r\ngot:\n%s" % (needle, r.stderr.strip()[:900]))
    for needle in case.get("err_lacks", []):
        if needle in r.stderr:
            raise Failure("stderr should not contain %r" % needle)


def run_codegen_case(exe, workdir, case):
    d = os.path.join(workdir, case["name"])
    os.makedirs(d, exist_ok=True)

    for fname, text in case.get("files", {}).items():
        path = os.path.join(d, fname)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)

    r = run_cserpent(exe, d, case["name"], case.get("src", ""),
                     case.get("args", []), case.get("stdin"))
    check(case, r)


def run_functional_case(exe, workdir, case, python, incs):
    d = os.path.join(workdir, case["name"])
    os.makedirs(d, exist_ok=True)

    src = os.path.join(d, "lib.c")
    with open(src, "w") as f:
        f.write(case["src"])

    r = subprocess.run([exe] + case.get("args", []) + [src],
                       capture_output=True, text=True, cwd=d)
    if r.returncode != 0:
        raise Failure("c-serpent failed:\n" + r.stderr.strip()[:900])

    # One translation unit: the wrapper has to see struct definitions and enum
    # constants, exactly as the README says.
    amalg = os.path.join(d, "amalg.c")
    with open(amalg, "w") as f:
        f.write(case["src"] + "\n\n" + r.stdout)

    so = os.path.join(d, case["module"] + ".so")
    cmd = [CC, "-fPIC", "-shared", "-I" + incs[0], "-I" + incs[1], amalg, "-o", so]
    c = subprocess.run(cmd, capture_output=True, text=True)
    if c.returncode != 0:
        errs = [l for l in c.stderr.splitlines() if "error" in l]
        raise Failure("generated code did not compile:\n" + "\n".join(errs[:12]))

    t = subprocess.run([python, "-c", case["test"]], capture_output=True, text=True, cwd=d)
    if t.returncode != 0:
        raise Failure("assertions failed:\n" + (t.stderr.strip() or t.stdout.strip())[:1200])


# ----------------------------------------------------------------- main


def run_notebook_case(workdir, case, python, incs, shim):
    """The notebook path goes through the cserpent_py extension, not the binary."""
    d = os.path.join(workdir, case["name"])
    os.makedirs(d, exist_ok=True)

    shutil.copy(shim, os.path.join(d, "_cserpent.so"))
    shutil.copy(os.path.join(ROOT, "cserpent.py"), d)

    script = os.path.join(d, "check.py")
    with open(script, "w") as f:
        f.write(case["test"])

    t = subprocess.run([python, script], capture_output=True, text=True, cwd=d)
    if t.returncode != 0:
        raise Failure("assertions failed:\n" + (t.stderr.strip() or t.stdout.strip())[:1500])


def build_cserpent_py(workdir, incs):
    """Build the cserpent_py extension, or return None if it will not build."""
    so = os.path.join(workdir, "_cserpent.so")
    r = subprocess.run([CC, "-fPIC", "-shared", "-I" + incs[0],
                        os.path.join(ROOT, "cserpent_py.c"), "-o", so],
                       capture_output=True, text=True)
    return so if r.returncode == 0 else None


def check_versions(exe):
    """pyproject.toml, cserpent.py and cserpent.c must all say the same thing."""
    import re

    with open(os.path.join(ROOT, "pyproject.toml")) as f:
        proj = re.search(r'^version\s*=\s*"([^"]+)"', f.read(), re.M)
    with open(os.path.join(ROOT, "cserpent.py")) as f:
        pyv = re.search(r'^__version__\s*=\s*"([^"]+)"', f.read(), re.M)

    r = subprocess.run([exe, "--version"], capture_output=True, text=True)
    cliv = r.stdout.strip().split()[-1] if r.returncode == 0 else None

    found = {
        "pyproject.toml": proj.group(1) if proj else None,
        "cserpent.py": pyv.group(1) if pyv else None,
        "cserpent.c --version": cliv,
    }
    if len(set(found.values())) != 1 or None in found.values():
        raise Failure("version strings disagree: %r" % found)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", metavar="PATTERN", help="only run tests whose name contains this")
    ap.add_argument("--python", default=sys.executable,
                    help="python to build extension modules against")
    ap.add_argument("--keep", action="store_true", help="keep the temporary directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    opts = ap.parse_args()

    workdir = tempfile.mkdtemp(prefix="cserpent-tests-")
    passed = failed = skipped = 0
    failures = []

    try:
        exe = build_cserpent(workdir)

        incs = probe_python(opts.python)
        if incs is None and opts.python != sys.executable:
            print("WARNING: %s cannot build extension modules" % opts.python)

        selected = lambda n: (opts.k is None) or (opts.k in n)

        for case in cases_codegen.CASES:
            if not selected(case["name"]):
                continue
            try:
                run_codegen_case(exe, workdir, case)
                passed += 1
                if opts.verbose:
                    print("  ok      %s" % case["name"])
            except Failure as e:
                failed += 1
                failures.append((case["name"], str(e)))

        for case in cases_functional.CASES:
            if not selected(case["name"]):
                continue
            if incs is None:
                skipped += 1
                continue
            try:
                run_functional_case(exe, workdir, case, opts.python, incs)
                passed += 1
                if opts.verbose:
                    print("  ok      %s" % case["name"])
            except Failure as e:
                failed += 1
                failures.append((case["name"], str(e)))

        shim = build_cserpent_py(workdir, incs) if incs else None

        for case in cases_notebook.CASES:
            if not selected(case["name"]):
                continue
            if incs is None or shim is None:
                skipped += 1
                continue
            try:
                run_notebook_case(workdir, case, opts.python, incs, shim)
                passed += 1
                if opts.verbose:
                    print("  ok      %s" % case["name"])
            except Failure as e:
                failed += 1
                failures.append((case["name"], str(e)))

        # the version appears in three places; make drift a test failure
        try:
            check_versions(exe)
            passed += 1
            if opts.verbose:
                print("  ok      versions_agree")
        except Failure as e:
            failed += 1
            failures.append(("versions_agree", str(e)))

        for name, msg in failures:
            print("FAIL  %s\n      %s\n" % (name, msg.replace("\n", "\n      ")))

        print("%d passed, %d failed, %d skipped" % (passed, failed, skipped))

        if skipped:
            print("\n%d functional tests were skipped: %s has no usable Python\n"
                  "and/or numpy headers. Re-run with --python /path/to/python\n"
                  "(a venv or conda env usually has them)." % (skipped, opts.python))

        return 1 if failed else 0

    finally:
        if opts.keep:
            print("temp dir kept at %s" % workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
