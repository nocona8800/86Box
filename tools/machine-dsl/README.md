# 86Box machine DSL toolkit

The toolkit parses, validates, safely normalizes whitespace, explains, and compiles `.86m` authoring files. It has no third-party Python dependencies. Formatting deliberately preserves comments and author-selected layout.

```sh
python tools/machine-dsl/86box_machine.py check examples/machines
python tools/machine-dsl/86box_machine.py fmt --check examples/machines
python tools/machine-dsl/86box_machine.py compile examples/machines -o machines.ir.json
python tools/machine-dsl/86box_machine.py explain p5a examples/machines
```

The JSON output is a stable tooling/debug envelope around the parsed source. The emulator independently compiles deployed `.86m` files to the compact in-memory IR declared in `machine_dsl.h`; device IDs and operations are resolved through its closed runtime registry.

The current in-tree pilots (`p5a`, `m579`, `5aa`, and `tx97xv`) are loaded from `src/machine/dsl` and execute through that IR. CMake deploys the files but does not compile or generate them.
