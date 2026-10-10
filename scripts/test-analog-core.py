#!/usr/bin/env python3
"""Linux/Ninja integration test using an already built desktop core and frontend.
Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON before building.
The frontend objects supply Platform services; its main is renamed for this test.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
build = root / 'build/melonDS/build'
commands = json.loads((build / 'compile_commands.json').read_text())
main = next(c for c in commands if c['file'].endswith('/qt_sdl/main.cpp'))
with tempfile.TemporaryDirectory(prefix='psz-analog-core-') as tmp:
    objects = []
    for name, source, extra in [('frontend', main['file'], ['-Dmain=PSZFrontendMain']),
                                ('test', str(root / 'tests/analog_core.cpp'), ['-UNDEBUG'])]:
        command = shlex.split(main['command'])
        output = str(Path(tmp) / (name + '.o'))
        command[command.index('-o') + 1] = output
        command[command.index('-c') + 1] = source
        subprocess.run(command + extra, cwd=build, check=True)
        objects.append(output)
    lines = subprocess.check_output(['ninja', '-t', 'commands', 'melonDS'], cwd=build, text=True).splitlines()
    command = shlex.split(lines[-1])
    # CMake wraps linking with ': && ... && :'. Execute only the linker argv.
    if command[:2] == [':', '&&']:
        command = command[2:]
    if command[-2:] == ['&&', ':']:
        command = command[:-2]
    if any(token in ('&&', ';', '|') for token in command):
        raise RuntimeError('Unexpected link command structure')
    index = command.index(main['output'])
    command[index:index+1] = objects
    binary = str(Path(tmp) / 'analog-core')
    command[command.index('-o')+1] = binary
    subprocess.run(command, cwd=build, check=True)
    subprocess.run([binary], cwd=tmp, env={**os.environ, "PSZ_MOVE_ON": "1"},
                   check=True, timeout=60)
