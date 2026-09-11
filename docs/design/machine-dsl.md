# 86Box machine description DSL

Status: runtime-only machine catalogue prototype on `dsl-branch`. The compiled-in `machine_t` rows have been removed; machines must register from complete root `.86m` catalogue blocks. The legacy C initializer and device implementations remain available during migration but are no longer catalogue entries.

## Implemented vertical slice

- `tools/machine-dsl/86box_machine.py` lexes/parses schema 1, validates cross-file declarations and selected invariants, formats without losing comments, emits JSON frontend IR, and explains declarations.
- `src/include/86box/machine_dsl.h` defines the versioned normalized runtime IR.
- `src/machine/machine_dsl.c` validates and executes firmware, platform, PCI, device, SPD, GPIO-default, conditional, result-capture, and native-hook operations.
- `p5a`, `m579`, `5aa`, and `tx97xv` now execute equivalent IR recipes while retaining their existing `machine_t` catalogue rows and callback entry points.
- Advanced source constructs are parsed by the authoring toolkit today. Runtime descriptions use the normalized `runtime` block; higher-level constructs must be lowered to that finite form or rejected.

The four pilot `.86m` files under `src/machine/dsl` are deployed beside 86Box. At boot, 86Box discovers the selected file, parses it, resolves registered device IDs, validates the resulting IR, and executes it. CMake copies the source files but performs no DSL generation.

A file in `<userfiles>/machines` or `<86Box executable>/machines` becomes a machine-table entry before configuration and UI initialization. `extends` remains supported for inheritance between already registered `.86m` machines, but there is no longer a compiled-in C catalogue to inherit from. Reusing an existing ID is rejected as a duplicate.

Root machines omit `extends` and provide a complete `catalogue` block. Required fields are `type`, `chipset`, CPU package/bus/voltage/multiplier limits, numeric `bus_flags` and `features`, and the memory minimum/maximum/step in KiB. `nvrmask`, `kbc_p1`, `gpio`, and `gpio_acpi` are optional. This form creates a machine without copying any compiled-in `machine_t` entry.

The old machine set has deliberately not been translated yet. A build with no externally installed complete root descriptions exits with a clear diagnostic instead of entering configuration code with an invalid machine index.

## Decision

Use a small, declarative, versioned language (`.86m`) which 86Box compiles to a typed in-memory machine-description IR during startup. The IR drives the existing C APIs in a fixed order. Keep device emulation in C and provide named native hooks only for boards whose behavior cannot be expressed declaratively.

The DSL should replace:

- rows in `src/machine/machine_table.c`;
- routine BIOS loads;
- calls to common machine initializers;
- PCI bus and slot registration tables;
- ordered `device_add*()` calls and their parameters;
- SPD registration;
- conditional selection of onboard video, sound, and network devices;
- simple GPIO/P1 defaults, masks, bit fields, and value mappings;
- explicit connections between typed device endpoints.

It should not replace device implementations, timing-sensitive emulation, arbitrary I/O handlers, or genuinely procedural board logic. Those remain C devices or reviewed native hooks.

## Why this boundary fits the current code

The present machine layer has three different concerns mixed together:

1. `machine_t` is catalogue and capability metadata used by the UI and configuration code.
2. Hundreds of `machine_*_init()` callbacks describe a mostly repeated startup recipe: load a BIOS, run a common initializer, register PCI routing, add devices, and optionally add onboard devices.
3. A minority of callbacks contain real behavior: configuration-dependent ROM selection, GPIO handlers that relocate devices, custom I/O handlers, or machine-specific state.

At the inspected revision, `machine_table.c` is roughly 25,700 lines. Under `src/machine` there are roughly 559 init definitions, 1,765 PCI-slot registrations, and 2,036 device-add calls. These are rough textual counts, not a semantic audit, but they show why a table-oriented representation is worthwhile.

The runtime must preserve initialization order. For example, PCI topology has to exist before PCI devices are added, and the existing BIOS-only availability path must stop after ROM validation. The IR therefore contains ordered phases rather than an unordered property bag.

## Surface syntax

The syntax is intentionally closer to a hardware description than YAML or a programming language:

```text
schema 1

profile ali_aladdin_v_ss7 {
    # reusable defaults
}

machine p5a extends ali_aladdin_v_ss7 {
    name = "[ALi ALADDiN V] ASUS P5A"

    cpu { ... }
    memory { ... }
    firmware bios { ... }
    pci { ... }
    devices { ... }
}
```

Rules:

- identifiers are ASCII `snake_case`; stable external IDs are strings where necessary;
- integers are decimal or `0x` hexadecimal; frequencies and sizes require units (`66.667 MHz`, `256 KiB`, `1.5 GiB`);
- lists preserve order; object fields do not;
- comments begin with `#`;
- trailing commas are allowed but not required;
- enum values are namespaced (`pci.normal`, `cpu.socket5_7`), not leaked C constants;
- defaults are supplied by profiles or the schema, not repeated in every machine;
- `extends` is single inheritance and resolved at compile time; list fields use explicit `append`, `prepend`, or `replace` so inheritance is never surprising;
- conditions are limited to typed emulator configuration facts. There are no loops, user functions, pointer values, or arbitrary arithmetic.

This custom syntax is preferable to YAML here because PCI routes, ordered actions, references, inheritance, typed units, and diagnostics are first-class. It also avoids YAML's implicit scalar rules. A JSON representation of the IR can be emitted for tooling; JSON should not be the authoring format.

## Core schema

### Identity and catalogue metadata

```text
machine <stable-id-or-string> [extends <profile-id>] {
    name = "display name"
    aliases = ["old name"]
    family = socket7 | super_socket7 | slot1 | ...
    chipset = intel.i430tx | ali.aladdin_v | ...
    available = true
    schema = 1                         # normally inherited from file
}
```

The machine ID replaces `internal_name` and remains stable across display-name changes. IDs which are not legal bare identifiers, such as `"5aa"`, use string syntax. Duplicate IDs and aliases are errors.

### CPU, memory, buses, and capabilities

```text
cpu {
    socket = socket5_7
    fsb = 60 MHz .. 120 MHz
    voltage = 2.000 V .. 3.520 V
    multiplier = 1.5 .. 5.5
    exclude = []
}

memory {
    range = 8 MiB .. 1536 MiB
    step = 8 MiB
}

buses = [isa, isa16, pci, agp, ps2, usb]
features = [ide.primary, ide.secondary, apm, acpi, gameport, usb, sound]
```

The compiler translates symbolic values to the existing flags but validates contradictions, such as `agp` without `pci`, invalid RAM steps, or a voltage minimum above its maximum. Composite C macros are conveniences, not DSL vocabulary: the normalized IR stores individual capabilities.

### Firmware

```text
firmware bios {
    load linear {
        file = "roms/machines/p5a/1011.005"
        address = 0x000c0000
        size = 256 KiB
    }
}
```

Supported loaders should initially mirror existing, common ROM helpers: `linear`, `interleaved`, `combined`, `inverted`, and `mapping`. Each loader has a typed argument schema. Firmware loading is always phase 1. In `bios_only` mode, successful validation exits before platform/device construction, matching current behavior.

Multiple BIOS choices belong under `option`, with a device configuration key, rather than an unrestricted conditional:

```text
firmware bios {
    select config.bios default "release" {
        option "release" { load linear { file = "..." address = 0xe0000 size = 128 KiB } }
        option "beta"    { load linear { file = "..." address = 0xe0000 size = 128 KiB } }
    }
}
```

### Initialization phases

The canonical execution order is:

1. firmware load/availability check;
2. common platform initialization;
3. defaults and declarative GPIO/P1 setup;
4. bus creation and slot routing;
5. devices, in source order;
6. connections;
7. conditional onboard peripherals;
8. optional native post-hook.

```text
platform = at.common

pci {
    mechanism = config_type_1
    slot 0x00 northbridge     route [none, none, none, none]
    slot 0x09 expansion       route [D, A, B, C]
}

devices {
    add ali.m1541 as northbridge
    add ali.m1543c as southbridge
    add flash.sst_39sf020
    add hwm.w83781d_p5a
    spd sdram { slots = 0b111; max_module = 512 MiB }
}
```

PCI interrupt routes use `A` through `H` and `none`. The compiler converts these to the numeric routing values currently passed to `pci_register_slot()`. Slot roles are a closed enum corresponding to the existing `PCI_CARD_*` classes.

Device names refer to a generated registry keyed by `device_t.internal_name`, not C symbols. Every DSL-visible device must declare a stable, unique ID and a typed parameter schema. Raw `(void *)` parameter integers are a transition-only facility:

```text
add sio.w83977 with { model = tf; firmware = ami; nvr = external }

# Temporary compatibility form; rejected in strict mode.
add sio.w83977 with_raw 0x00000031
```

### Conditions

Only equality, inequality, `and`, `or`, `not`, and presence checks over a whitelist of typed facts are supported:

```text
when selected.sound == internal {
    add sound.ensoniq_es1373_onboard as onboard_sound
    add codec.cs4297
}

when selected.video == internal and onboard.video exists {
    add onboard.video
}
```

Conditions are evaluated in a documented phase and may not change catalogue capabilities. This covers the current internal video/sound/network patterns without creating a general scripting language.

### Connections

Device addition and device wiring are different operations. Each device exposes typed endpoints in its registry metadata:

```text
connections {
    connect southbridge.ac97 -> onboard_sound.pci
    connect onboard_sound.ac97 -> codec.link
    connect southbridge.smbus -> hwm.smbus at 0x2d
}
```

Connections are checked for endpoint existence, direction, bus type, address collisions, cardinality, and cycles where the bus type forbids them. Existing devices that wire themselves internally can omit this section during migration; explicit wiring is the eventual target.

### GPIO and simple register behavior

Simple board straps should be data:

```text
gpio board_straps width 32 {
    reset = 0xffffffff
    read = latched
    writable mask 0x00000030

    field sound_base bits 4..5 {
        on write call sound.relocate_base {
            0b00: 0x220
            0b01: 0x260
            0b10: 0x240
            0b11: 0x280
        }
    }
}
```

`call` is not an arbitrary C function call. It targets a typed command exported by a device, with a schema known to the compiler. Defaults based on machine configuration use ordered mappings:

```text
defaults {
    field fsb_strap = match cpu.fsb {
        .. 50 MHz: 0b00
        .. 60 MHz: 0b01
        else:      0b10
    }
    field audio_present = selected.sound == internal
}
```

If behavior requires arbitrary I/O, timing, hidden state, or calls not modeled as device commands, use a C device or a native hook.

### Advanced board descriptions

Special cases still need bounded, analyzable tools. Schema 1 reserves the following constructs; the authoring toolkit parses them and the compiler must lower them to typed IR or reject them when the target runtime lacks the required capability.

Configuration variants can replace or patch subtrees without duplicating a machine:

```text
variant config.bios {
    default = "award"
    case "award" { firmware bios = award_bios }
    case "ami"   { firmware bios = ami_bios; append devices { add nvr.ami_1995 } }
}
```

Revision and model aliases can apply constrained patches:

```text
revision "1.04" when config.board_revision == "1.04" {
    replace devices.southbridge with via.vt82c586b_rev_b
    set pci.slot[0x07].route = [A, B, C, D]
}
```

Reusable ordered sequences cover initialization patterns that are more specific than a profile:

```text
sequence add_piix4_board(northbridge, superio, flash) {
    add $northbridge
    add intel.piix4
    add $superio
    add $flash
}

devices {
    run add_piix4_board(intel.i430tx, sio.pc87307, flash.intel_bxt)
}
```

Sequences are hygienic compile-time macros: typed parameters, no recursion, no conditions outside the caller's permitted facts, and a configured expansion-depth/operation-count limit.

Declarative register banks cover latch and strap logic without callbacks:

```text
register_bank board_io on isa.io {
    register control at 0x79 width 8 {
        reset = 0xff
        readable mask 0xff
        writable mask 0x30
        field sound_base bits 4..5
    }
    on control.sound_base changed {
        call onboard_sound.relocate_base map {
            0: 0x220
            1: 0x260
            2: 0x240
            3: 0x280
        }
    }
}
```

Small state machines may coordinate reset/power events:

```text
state_machine power_logic initial running {
    state running
    state suspended
    transition running -> suspended on acpi.suspend { emit cpu.suspend }
    transition suspended -> running on power_button { emit cpu.resume }
}
```

States, events, actions, and transitions must all be finite. There are no timers unless a registered device exports a typed timer endpoint; timing-sensitive logic belongs in that device.

Assertions document board invariants and become compile-time checks where possible:

```text
assert pci.slot[0x00].role == northbridge
assert exactly_one(selected.video == internal, selected.video == external)
require device "ali.m1543c" abi >= 2
require emulator.machine_ir >= 1
```

Resource overlays allow a pack to describe ROM alternatives and checksums safely:

```text
resource bios_release {
    path = "roms/machines/example/release.bin"
    size = 256 KiB
    sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}
```

All advanced constructs disappear during normalization. The runtime sees only firmware descriptors and a finite ordered opcode array. Unknown commands, endpoints, ABI requirements, or unlowered constructs are hard compilation errors.

### Native escape hatch

```text
native {
    post_init = "machine.endeavor.post_init.v1"
}
```

Hooks are registered by stable string ID in C, have a declared phase and signature, and are only available in bundled/trusted descriptions. User machine packs cannot name unrestricted symbols. A hook should be considered technical debt: its declaration must explain why a device, connection, typed command, or GPIO rule cannot model it yet.

## Complete examples

See:

- `examples/machines/ali-aladdin-v.86m` for a reusable profile and two ordinary boards;
- `examples/machines/gateway-lucas.86m` for aliases and conditional onboard audio;
- `examples/machines/endeavor-gpio.86m` for declarative GPIO plus a narrow native escape hatch.
- `examples/machines/advanced-special-cases.86m` for variants, sequences, register banks, state machines, resources, and assertions.

The examples are authoring fixtures checked by the toolkit. Runtime-ready pilot descriptions live under `src/machine/dsl` and use a normalized `runtime` block.

## Compiler and runtime architecture

```text
.86m source -> discovery -> parser -> registry resolution -> typed in-memory IR
            -> validation -> runtime executor
                    |
                    +-> diagnostics and optional JSON/tooling output
```

Recommended components:

- `machine_dsl`: parser with source spans and recovery after common syntax errors;
- `machine_schema`: enum, unit, device, parameter, endpoint, and hook registries;
- `machine_ir`: immutable normalized descriptions with no inheritance or implicit defaults;
- `machine_compile`: startup compiler plus offline checker/linter;
- `machine_runtime`: executor that adapts IR operations to existing C APIs;
- `machine_pack`: optional later loader for signed/bundled and user-provided packs.

Runtime-ready files are searched in `machines/<id>.86m`, the source checkout's `src/machine/dsl`, and the selected ROM path's `machines/<id>/machine.86m`. The first match is compiled at boot. Packaging installs the builtin descriptions into `machines/`; no generated C table is required.

Source text never contains C pointers. Stable IDs resolve through a closed runtime registry after parsing. A description is accepted only if its schema version, operation count, device IDs, conditions, PCI roles/routes, firmware data, and phase ordering validate.

## Diagnostics and tooling

The command-line tool should support:

```text
86box-machine check path/to/board.86m
86box-machine fmt path/to/board.86m
86box-machine compile path/to/board.86m --emit-json
86box-machine explain p5a
86box-machine diff-c p5a path/to/p5a.86m
```

Diagnostics must identify the file, line, column, machine, field, offending value, and expected type. Useful lint rules include duplicate PCI locations, missing north/south bridges, unreachable conditions, unused device aliases, implicit raw parameters, missing ROM files in repository builds, and suspicious interrupt routes containing out-of-range values.

## Compatibility and ownership

- `internal_name` remains the persisted identity. Existing configuration files continue to work.
- Save states should record the machine ID plus normalized description hash and DSL ABI. Loading a changed topology should warn or fail according to the existing save-state policy.
- Networked or regression tests should be able to require an exact description hash.
- Bundled descriptions are reviewed like code. User packs are data-only, resource-limited, and cannot invoke native hooks.
- Schema versions are explicit. Additive fields can remain compatible; removals or semantic changes require a migration tool or new major schema.
- ROMs are referenced but never embedded in descriptions. Existing ROM licensing/distribution rules still apply.

## Migration plan

### Phase 0: inventory and golden traces

Add an opt-in trace around existing machine initialization that records firmware operations, common initializer, PCI registrations, device additions/parameters, SPD calls, GPIO defaults, and conditional decisions. Capture representative machines and use these traces as golden tests.

### Phase 1: metadata only

Parse and compile identity, CPU, RAM, buses, capabilities, fixed devices, and aliases. Generate the existing `machine_t` catalogue from DSL while leaving `.init` callbacks in C. This removes the largest table with the smallest behavioral risk.

### Phase 2: generic init recipes

Add firmware, platform, PCI, ordered devices, SPD, and internal peripheral conditions. Migrate a small cohort of near-identical Socket 7/Super Socket 7 boards. Run both C and DSL recipes in trace mode and compare normalized traces in CI.

Suggested pilot: `p5a`, `m579`, `5aa`, and `tx97xv`. They exercise two chipset families, linear ROMs, PCI routing, SPD, raw device parameters, hardware monitoring, and conditional onboard peripherals without demanding a general-purpose interpreter.

### Phase 3: typed parameters and connections

Replace raw parameter words with per-device schemas and expose typed endpoints/commands. Migrate onboard audio/video/network and SMBus/I2C wiring. Reject `with_raw` for new definitions.

### Phase 4: GPIO rules

Implement masks, fields, configuration-derived defaults, and typed device commands. Migrate simple strap logic first. Leave timing/stateful handlers in C devices.

### Phase 5: external packs

After at least one stable schema cycle, add user machine directories, pack manifests, emulator ABI constraints, resource limits, deterministic precedence, and UI error reporting. Do not make runtime loading the first milestone.

### Phase 6: shrink the C surface

Remove migrated `machine_*_init()` functions and table entries only after parity tests pass. Keep native-hook machines mixed with fully declarative machines indefinitely; an all-or-nothing conversion is unnecessary.

## Acceptance criteria for the pilot

- The same machine IDs, names, aliases, CPU choices, RAM bounds, and UI capability flags appear as before.
- BIOS-only availability results match for present and missing ROMs.
- Normalized init traces match the C implementation, including order and parameters.
- At least four pilot machines boot their existing smoke-test workloads.
- Malformed descriptions fail with source-located diagnostics and cannot partially initialize a VM.
- No DSL input can dereference a pointer, load a host path outside permitted ROM roots, invoke an arbitrary native symbol, or allocate unbounded objects.
- Adding a routine board requires only one `.86m` file plus ROM metadata; C changes are necessary only for a new emulated device or reviewed native behavior.

## Open design questions

1. Should builtin descriptions remain one file per board, or be grouped by chipset family? One file per board gives cleaner ownership; family files make profiles easier to discover.
2. Is a device's current `internal_name` stable enough to be its public DSL ID, or should explicit namespaced IDs be added?
3. Should `machine_t` remain the public compatibility façade, or should callers migrate directly to the normalized IR over time?
4. Which connections are already implicit inside chipset devices and should remain so, versus being surfaced as typed endpoints?
5. What is the minimum useful pack format: directory plus manifest, or a single archive with hashes/signature metadata?
6. Which existing GPIO handlers can be reduced to fields and mappings, and which should become proper board devices rather than native hooks?

The most important constraint is deliberate incompleteness: the DSL should make common boards concise and auditable without attempting to express every behavior that C can express.
