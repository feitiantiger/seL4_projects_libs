/*
 * Copyright 2017, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

/*vm exits related with ept violations*/

#include <stdio.h>
#include <stdlib.h>

#include <sel4/sel4.h>

#include <sel4vm/guest_memory.h>

#include "sel4vm/debug.h"
#include "sel4vm/vmm.h"
#include "sel4vm/platform/vmcs.h"
#include "sel4vm/processor/decode.h"

#include "vm.h"
#include "guest_state.h"

#define EPT_VIOL_READ(qual) ((qual) & BIT(0))
#define EPT_VIOL_WRITE(qual) ((qual) & BIT(1))
#define EPT_VIOL_FETCH(qual) ((qual) & BIT(2))

void print_ept_violation(vm_vcpu_t *vcpu) {
    /* Read linear address that guest is trying to access. */
    unsigned int linear_address;
    vmm_vmcs_read(vcpu->vcpu.cptr, VMX_DATA_GUEST_LINEAR_ADDRESS, &linear_address);
    printf(COLOUR_R "!!!!!!!! ALERT :: GUEST OS PAGE FAULT !!!!!!!!\n");
    printf("    Guest OS VMExit due to EPT Violation:\n");
    printf("        Linear address 0x%x.\n", linear_address);
    printf("        Guest-Physical address 0x%x.\n", vmm_guest_exit_get_physical(vcpu->vcpu_arch.guest_state));
    printf("        Instruction pointer 0x%x.\n", vmm_guest_state_get_eip(vcpu->vcpu_arch.guest_state));
    printf("    This is most likely due to a bug or misconfiguration.\n" COLOUR_RESET);
}

static int unhandled_memory_fault(vm_t *vm, vm_vcpu_t *vcpu, uint32_t guest_phys, size_t size) {
    memory_fault_result_t fault_result = vm->mem.unhandled_mem_fault_handler(vm, vcpu, guest_phys, size,
            vm->mem.unhandled_mem_fault_cookie);
    switch(fault_result) {
        case FAULT_ERROR:
            print_ept_violation(vcpu);
            return -1;
        case FAULT_HANDLED:
        case FAULT_IGNORE:
            vmm_guest_exit_next_instruction(vcpu->vcpu_arch.guest_state, vcpu->vcpu.cptr);
            return 0;
    }
    return -1;
}

/* Handling EPT violation VMExit Events. */
int vmm_ept_violation_handler(vm_vcpu_t *vcpu) {
    int err;
    uintptr_t guest_phys = vmm_guest_exit_get_physical(vcpu->vcpu_arch.guest_state);
    unsigned int qualification = vmm_guest_exit_get_qualification(vcpu->vcpu_arch.guest_state);

    int read = EPT_VIOL_READ(qualification);
    int write = EPT_VIOL_WRITE(qualification);
    int fetch = EPT_VIOL_FETCH(qualification);
    if (read && write) {
        /* Indicates a fault while walking EPT */
        return VM_EXIT_HANDLE_ERROR;
    }
    if (fetch) {
        /* This is not MMIO */
        return VM_EXIT_HANDLE_ERROR;
    }

    int reg;
    uint32_t imm;
    int size;
    vmm_decode_ept_violation(vcpu, &reg, &imm, &size);
    if (size != 4) {
        ZF_LOGE("Currently don't support non-32 bit accesses");
        return VM_EXIT_HANDLE_ERROR;
    }
    if (reg < 0) {
        ZF_LOGE("Invalid reg while decoding ept violation");
        return VM_EXIT_HANDLE_ERROR;
    }

    memory_fault_result_t fault_result = vm_memory_handle_fault(vcpu->vm, vcpu, guest_phys, size);
    switch(fault_result) {
        case FAULT_ERROR:
            print_ept_violation(vcpu);
            return -1;
        case FAULT_HANDLED:
        case FAULT_IGNORE:
            vmm_guest_exit_next_instruction(vcpu->vcpu_arch.guest_state, vcpu->vcpu.cptr);
            return VM_EXIT_HANDLED;
        case FAULT_UNHANDLED:
            if (vcpu->vm->mem.unhandled_mem_fault_handler) {
                err = unhandled_memory_fault(vcpu->vm, vcpu, guest_phys, size);
                if (err) {
                    return -1;
                }
                return 0;
            }
    }
    ZF_LOGE("Failed to handle ept fault");
    print_ept_violation(vcpu);
    return -1;
}
