#!/usr/bin/env python3

import argparse
import subprocess
import sys

from pathlib import Path


def read_file(path):
    with open(path) as f:
        return f.read()


def write_file(path, output):
    with open(path, "w") as f:
        f.write(output)


# Parse a file that looks like
#
#   FOO = foo1 foo2 foo3
#   BAR =
#   BAZ = baz1
#
# into a dictionary that looks like
#
#   {
#     "${FOO}": ["foo1", "foo2", "foo3"],
#     "${BAR}": [],
#     "${BAZ}": ["baz1"],
#   }
#
def load_values(path):
    values = {}

    with open(path) as f:
        lineno = 0
        for line in f:
            lineno += 1
            line = line.strip()

            # Skip empty lines and comments
            if line == "" or line[0] == "#":
                continue

            parts = line.split(" ")

            if len(parts) < 2 or parts[1] != "=":
                print(f"{path}:{lineno}: Invalid syntax")
                sys.exit(1)

            # Use "${FOO}" instead of "FOO" as the dictionary key.
            # This makes things more convenient later
            key = "${" + parts[0] + "}"
            value = parts[2:]

            values[key] = value

    return values


def process_control(path, arches):
    output = read_file(path)

    for arch in arches:
        output = output.replace(arch, " ".join(arches[arch]))

    return output


def process_debhelper(path, arches, mode, arch, os):
    output = []

    with open(path) as f:
        lineno = 0
        for line in f:
            lineno += 1
            line = line.strip()

            # Empty lines and lines that don't start with [cond] are
            # included in the output verbatim
            if line == "" or line[0] != "[":
                output.append(line)
                continue

            parts = line[1:].split("]", maxsplit=1)

            if len(parts) < 2:
                print(f"{path}:{lineno}: Invalid syntax")
                sys.exit(1)

            # The line looked like
            #
            #   [cond] file
            #
            cond = parts[0].strip()
            file = parts[1].strip()

            # In verify mode, strip the condition and output the rest.
            # Running wrap-and-sort against this output (see below)
            # guarantees that the input follows the requirements too
            if mode == "verify":
                output.append(file)
                continue

            # Handle lines that look like
            #
            #   [linux-any] file
            #
            if cond.endswith("-any"):
                if cond == os + "-any":
                    output.append(file)
                continue

            if cond not in arches:
                print(f"{path}:{lineno}: Unknown architecture group '{cond}'")
                sys.exit(1)

            # Only output the line if we the architecture we're building on
            # is one of those listed in cond. cond itself will be stripped
            if arch in arches[cond]:
                output.append(file)

    output.append("")
    return "\n".join(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["generate", "build", "verify"],
                                  default="generate")
    parser.add_argument("--arch")
    parser.add_argument("--os")
    args = parser.parse_args()
    mode = args.mode
    arch = args.arch
    os = args.os

    if mode == "build" and (arch is None or os is None):
        print("--arch and --os are required for --mode=build")
        sys.exit(1)

    template_ext = ".in"
    debian_dir = Path("debian")
    vars_file = Path(debian_dir, "arches.mk")

    values = load_values(vars_file)

    for infile in debian_dir.glob("*" + template_ext):
        basename = infile.name[:-len(template_ext)]
        outfile = Path(debian_dir, basename)

        # Generate mode is for maintainers, and is used to keep
        # debian/control in sync with its template.
        # All other files are ignored
        if mode == "generate" and basename != "control":
            continue

        print(f"  GEN {outfile}")

        # When building the package, debian/control should already be
        # in sync with its template. To confirm that is the case,
        # save the contents of the output file before regenerating it
        if mode in ["build", "verify"] and basename == "control":
            old_output = read_file(outfile)

        if basename == "control":
            output = process_control(infile, values)
        else:
            output = process_debhelper(infile, values, mode, arch, os)

        write_file(outfile, output)

        # When building the package, regenerating debian/control
        # should be a no-op. If that's not the case, it means that
        # the file and its template have gone out of sync, and we
        # don't know which one should be used.
        # Abort the build and let the user fix things
        if mode in ["build", "verify"] and basename == "control":
            if output != old_output:
                print(f"{outfile}: Needs to be regenerated from template")
                sys.exit(1)

    # In verify mode only, check that things are pretty
    if mode == "verify":
        print("  CHK wrap-and-sort")
        wrap_and_sort = subprocess.run(["wrap-and-sort", "-ast", "--dry-run"],
                                       capture_output=True, text=True)
        if wrap_and_sort.returncode != 0 or wrap_and_sort.stdout != "":
            print("stdout:")
            print(wrap_and_sort.stdout.strip())
            print(f"rc: {wrap_and_sort.returncode}\n")
            sys.exit(1)


if __name__ == "__main__":
    main()
