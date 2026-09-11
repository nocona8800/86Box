/* Runtime executor for build-time compiled .86m descriptions. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/chipset.h>
#include <86box/flash.h>
#include <86box/hwm.h>
#include <86box/machine.h>
#include <86box/machine_dsl.h>
#include <86box/mem.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/network.h>
#include <86box/pci.h>
#include <86box/path.h>
#include <86box/rom.h>
#include <86box/plat.h>
#include <86box/sio.h>
#include <86box/sound.h>
#include <86box/spd.h>
#include <86box/video.h>

#define MACHINE_IR_MAX_OPS 256
#define MACHINE_IR_LINE_SIZE 1024

typedef struct machine_ir_source_t {
    machine_ir_desc_t desc;
    machine_ir_op_t   ops[MACHINE_IR_MAX_OPS];
    char              firmware_file[512];
} machine_ir_source_t;

typedef struct machine_ir_device_registry_t {
    const char     *id;
    const device_t *device;
} machine_ir_device_registry_t;

static const machine_ir_device_registry_t machine_ir_devices[] = {
    { "ali.m1541",             &ali1541_device },
    { "ali.m1543c",            &ali1543c_device },
    { "flash.sst_39sf020",     &sst_flash_39sf020_device },
    { "flash.sst_29ee010",     &sst_flash_29ee010_device },
    { "hwm.w83781d_p5a",       &w83781d_p5a_device },
    { "intel.i430tx",          &i430tx_device },
    { "intel.piix4",           &piix4_device },
    { "sio.pc87307",           &pc87307_device },
    { "flash.intel_bxt",       &intel_flash_bxt_device },
    { NULL, NULL }
};

static const device_t *
machine_ir_device_from_id(const char *id)
{
    for (const machine_ir_device_registry_t *entry = machine_ir_devices; entry->id; entry++)
        if (!strcmp(entry->id, id))
            return entry->device;
    return NULL;
}

static char *
machine_ir_trim(char *line)
{
    char *end;
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')
        line++;
    end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = 0;
    return line;
}

static void
machine_ir_strip_comment(char *line)
{
    int quoted = 0, escaped = 0;
    for (char *p = line; *p; p++) {
        if (escaped) {
            escaped = 0;
        } else if (*p == '\\' && quoted) {
            escaped = 1;
        } else if (*p == '"') {
            quoted = !quoted;
        } else if (*p == '#' && !quoted) {
            *p = 0;
            return;
        }
    }
}

static int
machine_ir_route_value(const char *value)
{
    if (!strcmp(value, "none"))
        return 0;
    if (value[0] >= 'A' && value[0] <= 'H' && !value[1])
        return value[0] - 'A' + 1;
    return -1;
}

static int
machine_ir_pci_role(const char *role)
{
    static const struct { const char *name; int value; } roles[] = {
        { "northbridge", PCI_CARD_NORTHBRIDGE }, { "agp_bridge", PCI_CARD_AGPBRIDGE },
        { "southbridge", PCI_CARD_SOUTHBRIDGE }, { "southbridge_ide", PCI_CARD_SOUTHBRIDGE_IDE },
        { "southbridge_pmu", PCI_CARD_SOUTHBRIDGE_PMU }, { "southbridge_usb", PCI_CARD_SOUTHBRIDGE_USB },
        { "expansion", PCI_CARD_NORMAL }, { "video", PCI_CARD_VIDEO }, { "sound", PCI_CARD_SOUND },
        { "network", PCI_CARD_NETWORK }, { "scsi", PCI_CARD_SCSI }, { "ide", PCI_CARD_IDE },
        { "bridge", PCI_CARD_BRIDGE }, { NULL, 0 }
    };
    for (int i = 0; roles[i].name; i++)
        if (!strcmp(roles[i].name, role))
            return roles[i].value;
    return -1;
}

static int
machine_ir_add_op(machine_ir_source_t *source, machine_ir_op_t **op, char *error, size_t error_size)
{
    if (source->desc.op_count >= MACHINE_IR_MAX_OPS) {
        snprintf(error, error_size, "too many operations (maximum %u)", MACHINE_IR_MAX_OPS);
        return 0;
    }
    *op = &source->ops[source->desc.op_count++];
    memset(*op, 0, sizeof(**op));
    (*op)->condition = MACHINE_IR_ALWAYS;
    return 1;
}

static int
machine_ir_parse_runtime_line(machine_ir_source_t *source, const char *line,
                              unsigned line_no, char *error, size_t error_size)
{
    machine_ir_op_t *op;
    char id[128], role[64], a[16], b[16], c[16], d[16], condition[64] = "always", capture[64] = "discard";
    unsigned card, slot_mask, max_module;
    unsigned long long params = 0;

    if (sscanf(line, "firmware linear \"%511[^\"]\" %x %u %x", source->firmware_file,
               &source->desc.firmware.address, &source->desc.firmware.size, &source->desc.firmware.flags) == 4) {
        source->desc.firmware.kind = MACHINE_IR_FIRMWARE_LINEAR;
        source->desc.firmware.file[0] = source->firmware_file;
        return 1;
    }
    if (!strcmp(line, "platform at.common")) {
        source->desc.platform = MACHINE_IR_PLATFORM_AT_COMMON;
        return 1;
    }
    if (!strcmp(line, "pci config_type_1")) {
        if (!machine_ir_add_op(source, &op, error, error_size)) return 0;
        op->opcode = MACHINE_IR_PCI_INIT;
        op->data.pci_mechanism = PCI_CONFIG_TYPE_1;
        return 1;
    }
    if (sscanf(line, "slot %x %63s route [ %15[^,], %15[^,], %15[^,], %15[^]]]",
               &card, role, a, b, c, d) == 6) {
        int role_value = machine_ir_pci_role(role);
        int routes[4] = { machine_ir_route_value(machine_ir_trim(a)), machine_ir_route_value(machine_ir_trim(b)),
                          machine_ir_route_value(machine_ir_trim(c)), machine_ir_route_value(machine_ir_trim(d)) };
        if (card > 0xff || role_value < 0 || routes[0] < 0 || routes[1] < 0 || routes[2] < 0 || routes[3] < 0)
            goto invalid;
        if (!machine_ir_add_op(source, &op, error, error_size)) return 0;
        op->opcode = MACHINE_IR_PCI_SLOT;
        op->data.pci_slot.card = card;
        op->data.pci_slot.role = role_value;
        for (int i = 0; i < 4; i++) op->data.pci_slot.route[i] = routes[i];
        return 1;
    }
    if (sscanf(line, "spd sdram %x %u", &slot_mask, &max_module) == 2) {
        if (slot_mask > 0xff || max_module > 0xffff) goto invalid;
        if (!machine_ir_add_op(source, &op, error, error_size)) return 0;
        op->opcode = MACHINE_IR_SPD_REGISTER;
        op->data.spd.type = SPD_TYPE_SDRAM;
        op->data.spd.slot_mask = slot_mask;
        op->data.spd.max_module_mb = max_module;
        return 1;
    }
    if (sscanf(line, "gpio default %x", &card) == 1 || sscanf(line, "gpio acpi_default %x", &card) == 1) {
        if (!machine_ir_add_op(source, &op, error, error_size)) return 0;
        op->opcode = strstr(line, "acpi_default") ? MACHINE_IR_GPIO_ACPI_DEFAULT : MACHINE_IR_GPIO_DEFAULT;
        op->data.gpio_default = card;
        return 1;
    }
    {
        if (sscanf(line, "device %127s", id) == 1) {
            const device_t *device = NULL;
            machine_ir_device_source_t source_kind = MACHINE_IR_DEVICE_FIXED;
            const char *part;
            if ((part = strstr(line, " params ")) != NULL && sscanf(part, " params %llx", &params) != 1)
                goto invalid;
            if ((part = strstr(line, " when ")) != NULL && sscanf(part, " when %63s", condition) != 1)
                goto invalid;
            if ((part = strstr(line, " capture ")) != NULL && sscanf(part, " capture %63s", capture) != 1)
                goto invalid;
            if (!strcmp(id, "onboard.sound")) source_kind = MACHINE_IR_DEVICE_ONBOARD_SOUND;
            else if (!strcmp(id, "onboard.video")) source_kind = MACHINE_IR_DEVICE_ONBOARD_VIDEO;
            else if (!strcmp(id, "onboard.network")) source_kind = MACHINE_IR_DEVICE_ONBOARD_NETWORK;
            else device = machine_ir_device_from_id(id);
            if (source_kind == MACHINE_IR_DEVICE_FIXED && !device) goto invalid;
            if (!machine_ir_add_op(source, &op, error, error_size)) return 0;
            op->opcode = MACHINE_IR_DEVICE_ADD;
            op->data.device.device = device;
            op->data.device.params = (uintptr_t) params;
            op->data.device.source = source_kind;
            if (!strcmp(condition, "sound.internal")) op->condition = MACHINE_IR_SOUND_INTERNAL;
            else if (!strcmp(condition, "video.internal")) op->condition = MACHINE_IR_VIDEO_INTERNAL;
            else if (!strcmp(condition, "network.internal")) op->condition = MACHINE_IR_NETWORK_INTERNAL;
            else if (strcmp(condition, "always")) goto invalid;
            if (!strcmp(capture, "machine_snd")) op->data.device.result = MACHINE_IR_RESULT_MACHINE_SOUND;
            else if (strcmp(capture, "discard")) goto invalid;
            return 1;
        }
    }

invalid:
    snprintf(error, error_size, "line %u: invalid or unsupported runtime statement: %s", line_no, line);
    return 0;
}

static FILE *
machine_ir_open_source(const char *machine_id, char *path, size_t path_size)
{
    FILE *file;
    char relative[256];
    snprintf(relative, sizeof(relative), "machines/%s.86m", machine_id);
    path_append_filename(path, usr_path, relative);
    file = plat_fopen(path, "rb");
    if (file) return file;
    path_append_filename(path, exe_path, relative);
    file = plat_fopen(path, "rb");
    if (file) return file;
    snprintf(path, path_size, "src/machine/dsl/%s.86m", machine_id);
    file = plat_fopen(path, "rb");
    if (file) return file;
    snprintf(path, path_size, "roms/machines/%s/machine.86m", machine_id);
    return rom_fopen(path, "rb");
}

static int
machine_ir_parse_file(FILE *file, const char *machine_id, machine_ir_source_t *source,
                      char *error, size_t error_size)
{
    char line_buffer[MACHINE_IR_LINE_SIZE];
    unsigned line_no = 0;
    int schema_seen = 0, machine_seen = 0, runtime_depth = 0, brace_depth = 0;

    memset(source, 0, sizeof(*source));
    source->desc.abi = MACHINE_IR_ABI;
    source->desc.id = machine_id;
    source->desc.ops = source->ops;

    while (fgets(line_buffer, sizeof(line_buffer), file)) {
        char *line;
        int opens = 0, closes = 0;
        line_no++;
        if (!strchr(line_buffer, '\n') && !feof(file)) {
            snprintf(error, error_size, "line %u exceeds %u bytes", line_no, MACHINE_IR_LINE_SIZE - 1);
            return 0;
        }
        machine_ir_strip_comment(line_buffer);
        line = machine_ir_trim(line_buffer);
        if (!*line) continue;
        for (char *p = line; *p; p++) { if (*p == '{') opens++; else if (*p == '}') closes++; }
        if (!schema_seen) {
            unsigned schema;
            if (sscanf(line, "schema %u", &schema) != 1 || schema != 1) {
                snprintf(error, error_size, "line %u: expected 'schema 1'", line_no);
                return 0;
            }
            schema_seen = 1;
            continue;
        }
        if (!machine_seen) {
            char parsed_id[128];
            if (sscanf(line, "machine %127s {", parsed_id) == 1) {
                char *quote = strchr(parsed_id, '{'); if (quote) *quote = 0;
                if (parsed_id[0] == '"') {
                    memmove(parsed_id, parsed_id + 1, strlen(parsed_id));
                    quote = strchr(parsed_id, '"'); if (quote) *quote = 0;
                }
                if (strcmp(parsed_id, machine_id)) {
                    snprintf(error, error_size, "line %u: machine id '%s' does not match '%s'", line_no, parsed_id, machine_id);
                    return 0;
                }
                machine_seen = 1;
                brace_depth = opens - closes;
                continue;
            }
        }
        if (runtime_depth) {
            if (closes && brace_depth == runtime_depth) {
                runtime_depth = 0;
            } else if (!machine_ir_parse_runtime_line(source, line, line_no, error, error_size)) {
                return 0;
            }
        } else if (!strncmp(line, "runtime", 7) && strchr(line, '{')) {
            runtime_depth = brace_depth + 1;
        }
        brace_depth += opens - closes;
        if (brace_depth < 0) {
            snprintf(error, error_size, "line %u: unmatched '}'", line_no);
            return 0;
        }
    }
    if (brace_depth != 0 || runtime_depth != 0) {
        snprintf(error, error_size, "unterminated block in machine description for '%s'", machine_id);
        return 0;
    }
    if (!schema_seen || !machine_seen || !source->desc.op_count || source->desc.firmware.kind == MACHINE_IR_FIRMWARE_NONE) {
        snprintf(error, error_size, "incomplete machine description for '%s'", machine_id);
        return 0;
    }
    return machine_ir_validate(&source->desc, error, error_size);
}

static int
machine_ir_condition_matches(machine_ir_condition_t condition)
{
    switch (condition) {
        case MACHINE_IR_ALWAYS:
            return 1;
        case MACHINE_IR_SOUND_INTERNAL:
            return sound_card_current[0] == SOUND_INTERNAL;
        case MACHINE_IR_VIDEO_INTERNAL:
            return gfxcard[0] == VID_INTERNAL;
        case MACHINE_IR_NETWORK_INTERNAL:
            return net_cards_conf[0].device_num == NET_INTERNAL;
        default:
            return 0;
    }
}

static const device_t *
machine_ir_resolve_device(const machine_ir_device_t *device)
{
    switch (device->source) {
        case MACHINE_IR_DEVICE_FIXED:
            return device->device;
        case MACHINE_IR_DEVICE_ONBOARD_SOUND:
            return machine_get_snd_device(machine);
        case MACHINE_IR_DEVICE_ONBOARD_VIDEO:
            return machine_get_vid_device(machine);
        case MACHINE_IR_DEVICE_ONBOARD_NETWORK:
            return machine_get_net_device(machine);
        default:
            return NULL;
    }
}

int
machine_ir_validate(const machine_ir_desc_t *desc, char *error, size_t error_size)
{
#define IR_ERROR(...) do { if (error && error_size) snprintf(error, error_size, __VA_ARGS__); return 0; } while (0)
    int saw_pci = 0;

    if (!desc)
        IR_ERROR("null machine IR");
    if (desc->abi != MACHINE_IR_ABI)
        IR_ERROR("machine '%s' uses IR ABI %u, expected %u", desc->id ? desc->id : "?", desc->abi, MACHINE_IR_ABI);
    if (!desc->id || !desc->id[0])
        IR_ERROR("machine IR has no stable id");
    if (desc->firmware.kind == MACHINE_IR_FIRMWARE_LINEAR) {
        if (!desc->firmware.file[0] || !desc->firmware.file[0][0])
            IR_ERROR("machine '%s' has an empty firmware path", desc->id);
        if (!desc->firmware.size)
            IR_ERROR("machine '%s' has zero-sized firmware", desc->id);
    }
    if (desc->op_count && !desc->ops)
        IR_ERROR("machine '%s' has an operation count but no operations", desc->id);

    for (size_t i = 0; i < desc->op_count; i++) {
        const machine_ir_op_t *op = &desc->ops[i];
        if (op->condition < MACHINE_IR_ALWAYS || op->condition > MACHINE_IR_NETWORK_INTERNAL)
            IR_ERROR("machine '%s' operation %u has an invalid condition", desc->id, (unsigned) i);
        switch (op->opcode) {
            case MACHINE_IR_PCI_INIT:
                if (saw_pci)
                    IR_ERROR("machine '%s' initializes PCI more than once", desc->id);
                saw_pci = 1;
                break;
            case MACHINE_IR_PCI_SLOT:
                if (!saw_pci)
                    IR_ERROR("machine '%s' registers PCI slot 0x%02x before PCI initialization", desc->id, op->data.pci_slot.card);
                break;
            case MACHINE_IR_DEVICE_ADD:
                if ((op->data.device.source == MACHINE_IR_DEVICE_FIXED) && !op->data.device.device)
                    IR_ERROR("machine '%s' operation %u has a null fixed device", desc->id, (unsigned) i);
                break;
            case MACHINE_IR_SPD_REGISTER:
                if (!op->data.spd.slot_mask || !op->data.spd.max_module_mb)
                    IR_ERROR("machine '%s' operation %u has invalid SPD data", desc->id, (unsigned) i);
                break;
            case MACHINE_IR_GPIO_DEFAULT:
            case MACHINE_IR_GPIO_ACPI_DEFAULT:
                break;
            case MACHINE_IR_NATIVE_HOOK:
                if (!op->data.native_hook)
                    IR_ERROR("machine '%s' operation %u has a null native hook", desc->id, (unsigned) i);
                break;
            default:
                IR_ERROR("machine '%s' operation %u has unknown opcode %u", desc->id, (unsigned) i, (unsigned) op->opcode);
        }
    }
    if (error && error_size)
        error[0] = 0;
    return 1;
#undef IR_ERROR
}

int
machine_ir_execute(const machine_t *model, const machine_ir_desc_t *desc)
{
    char error[256];
    int  ret = 1;

    if (!machine_ir_validate(desc, error, sizeof(error))) {
        pclog("Machine DSL: %s\n", error);
        return 0;
    }

    switch (desc->firmware.kind) {
        case MACHINE_IR_FIRMWARE_NONE:
            break;
        case MACHINE_IR_FIRMWARE_LINEAR:
            ret = bios_load_linear(desc->firmware.file[0], desc->firmware.address,
                                   desc->firmware.size, desc->firmware.flags);
            break;
        default:
            pclog("Machine DSL: machine '%s' has unsupported firmware kind %u\n",
                  desc->id, (unsigned) desc->firmware.kind);
            return 0;
    }

    if (bios_only || !ret)
        return ret;

    if (desc->platform == MACHINE_IR_PLATFORM_AT_COMMON)
        machine_at_common_init(model);
    else if (desc->platform != MACHINE_IR_PLATFORM_NONE) {
        pclog("Machine DSL: machine '%s' has unsupported platform %u\n",
              desc->id, (unsigned) desc->platform);
        return 0;
    }

    for (size_t i = 0; i < desc->op_count; i++) {
        const machine_ir_op_t *op = &desc->ops[i];
        if (!machine_ir_condition_matches(op->condition))
            continue;

        switch (op->opcode) {
            case MACHINE_IR_PCI_INIT:
                pci_init(op->data.pci_mechanism);
                break;
            case MACHINE_IR_PCI_SLOT:
                pci_register_slot(op->data.pci_slot.card, op->data.pci_slot.role,
                                  op->data.pci_slot.route[0], op->data.pci_slot.route[1],
                                  op->data.pci_slot.route[2], op->data.pci_slot.route[3]);
                break;
            case MACHINE_IR_DEVICE_ADD: {
                const device_t *device = machine_ir_resolve_device(&op->data.device);
                if (device) {
                    void *priv = op->data.device.params
                               ? device_add_params(device, (void *) op->data.device.params)
                               : device_add(device);
                    if (op->data.device.result == MACHINE_IR_RESULT_MACHINE_SOUND)
                        machine_snd = priv;
                }
                break;
            }
            case MACHINE_IR_SPD_REGISTER:
                spd_register(op->data.spd.type, op->data.spd.slot_mask,
                             op->data.spd.max_module_mb);
                break;
            case MACHINE_IR_GPIO_DEFAULT:
                machine_set_gpio_default(op->data.gpio_default);
                break;
            case MACHINE_IR_GPIO_ACPI_DEFAULT:
                machine_set_gpio_acpi_default(op->data.gpio_default);
                break;
            case MACHINE_IR_NATIVE_HOOK:
                if (!op->data.native_hook(model))
                    return 0;
                break;
            default:
                return 0; /* validation should make this unreachable */
        }
    }

    return ret;
}

int
machine_ir_execute_file(const machine_t *model, const char *machine_id)
{
    machine_ir_source_t source;
    char path[512], error[512];
    FILE *file = machine_ir_open_source(machine_id, path, sizeof(path));

    if (!file) {
        pclog("Machine DSL: no description found for '%s' (searched machines/, src/machine/dsl/, and ROM paths)\n",
              machine_id);
        return 0;
    }
    if (!machine_ir_parse_file(file, machine_id, &source, error, sizeof(error))) {
        fclose(file);
        pclog("Machine DSL: %s: %s\n", path, error);
        return 0;
    }
    fclose(file);
    return machine_ir_execute(model, &source.desc);
}
