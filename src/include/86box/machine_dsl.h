/*
 * 86Box machine description DSL - normalized runtime IR.
 *
 * The parser/compiler lives in tools/machine-dsl.  The emulator consumes only
 * this small, validated representation; it never interprets source text.
 */
#ifndef EMU_MACHINE_DSL_H
#define EMU_MACHINE_DSL_H

#include <stddef.h>
#include <stdint.h>

struct _device_;
struct _machine_;

typedef enum machine_ir_condition_t {
    MACHINE_IR_ALWAYS = 0,
    MACHINE_IR_SOUND_INTERNAL,
    MACHINE_IR_VIDEO_INTERNAL,
    MACHINE_IR_NETWORK_INTERNAL
} machine_ir_condition_t;

typedef enum machine_ir_device_source_t {
    MACHINE_IR_DEVICE_FIXED = 0,
    MACHINE_IR_DEVICE_ONBOARD_SOUND,
    MACHINE_IR_DEVICE_ONBOARD_VIDEO,
    MACHINE_IR_DEVICE_ONBOARD_NETWORK
} machine_ir_device_source_t;

typedef enum machine_ir_result_t {
    MACHINE_IR_RESULT_DISCARD = 0,
    MACHINE_IR_RESULT_MACHINE_SOUND
} machine_ir_result_t;

typedef enum machine_ir_opcode_t {
    MACHINE_IR_PCI_INIT = 0,
    MACHINE_IR_PCI_SLOT,
    MACHINE_IR_DEVICE_ADD,
    MACHINE_IR_SPD_REGISTER,
    MACHINE_IR_GPIO_DEFAULT,
    MACHINE_IR_GPIO_ACPI_DEFAULT,
    MACHINE_IR_NATIVE_HOOK
} machine_ir_opcode_t;

typedef int (*machine_ir_native_hook_t)(const struct _machine_ *model);

typedef struct machine_ir_pci_slot_t {
    uint8_t card;
    uint8_t role;
    uint8_t route[4];
} machine_ir_pci_slot_t;

typedef struct machine_ir_device_t {
    const struct _device_      *device;
    uintptr_t                   params;
    machine_ir_device_source_t  source;
    machine_ir_result_t         result;
} machine_ir_device_t;

typedef struct machine_ir_spd_t {
    uint8_t  type;
    uint8_t  slot_mask;
    uint16_t max_module_mb;
} machine_ir_spd_t;

typedef struct machine_ir_op_t {
    machine_ir_opcode_t    opcode;
    machine_ir_condition_t condition;
    union {
        int                      pci_mechanism;
        machine_ir_pci_slot_t    pci_slot;
        machine_ir_device_t      device;
        machine_ir_spd_t         spd;
        uint32_t                 gpio_default;
        machine_ir_native_hook_t native_hook;
    } data;
} machine_ir_op_t;

typedef enum machine_ir_firmware_kind_t {
    MACHINE_IR_FIRMWARE_NONE = 0,
    MACHINE_IR_FIRMWARE_LINEAR
} machine_ir_firmware_kind_t;

typedef struct machine_ir_firmware_t {
    machine_ir_firmware_kind_t kind;
    const char                *file[4];
    uint32_t                   address;
    uint32_t                   size;
    uint32_t                   flags;
} machine_ir_firmware_t;

typedef enum machine_ir_platform_t {
    MACHINE_IR_PLATFORM_NONE = 0,
    MACHINE_IR_PLATFORM_AT_COMMON
} machine_ir_platform_t;

typedef struct machine_ir_desc_t {
    uint32_t                    abi;
    const char                 *id;
    machine_ir_firmware_t       firmware;
    machine_ir_platform_t       platform;
    const machine_ir_op_t      *ops;
    size_t                      op_count;
} machine_ir_desc_t;

#define MACHINE_IR_ABI 1
#define MACHINE_IR_COUNT(a) (sizeof(a) / sizeof((a)[0]))

int machine_ir_execute(const struct _machine_ *model, const machine_ir_desc_t *desc);
int machine_ir_validate(const machine_ir_desc_t *desc, char *error, size_t error_size);
int machine_ir_execute_file(const struct _machine_ *model, const char *machine_id);
int machine_dsl_register_directory(const char *directory);
int machine_dsl_dynamic_init(const struct _machine_ *model);

#endif /* EMU_MACHINE_DSL_H */
