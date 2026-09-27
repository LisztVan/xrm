#!/usr/bin/env python3
"""Make CMake's GCC 16 compilation database readable by clang-tidy.

CMake adds GCC module-dependency scanner flags to C++23 compile commands even
though XRM does not use C++ modules. Clang rejects those GCC-only flags before
it can analyze a translation unit. Keep every include, define, and language
option; remove only the scanner flags in a separate, generated database.
"""

import argparse
import json
import shlex
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compile_commands", type=Path)
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()

    commands = json.loads(args.compile_commands.read_text())
    for entry in commands:
        arguments = entry.get("arguments") or shlex.split(entry["command"])
        entry["arguments"] = [
            argument
            for argument in arguments
            if argument != "-fmodules-ts"
            and not argument.startswith(("-fmodule-mapper=", "-fdeps-format="))
        ]
        entry.pop("command", None)

    args.output_directory.mkdir(parents=True, exist_ok=True)
    (args.output_directory / "compile_commands.json").write_text(
        json.dumps(commands, indent=2) + "\n"
    )


if __name__ == "__main__":
    main()
