#include <ahci/ahci.h>
#include <efi/boot.h>
#include <efi/efi.h>
#include <efi/fbc.h>
#include <elf.h>
#include <errno.h>
#include <fs/fatfs/fatfs.h>
#include <fs/partition.h>
#include <fs/vfs/devfs.h>
#include <fs/vfs/vfs.h>
#include <krlibc.h>
#include <mm/frame.h>
#include <mm/uaccess.h>
#include <pci/pci.h>
#include <proto.hpp>
#include <ps2/keyboard.h>
#include <rtc.h>
#include <stdint.h>
#include <syscall/syscall.h>
#include <task/pcb.h>
#include <user/user.h>
#include "../build_settings.h"

void init_process_xtttp(pcb_t pcb)
{
    // pcb->xtttp_stc->input    = (char *)malloc(1024 * sizeof(char));
    memset(pcb->xtttp_stc->output, 0, sizeof(char) * 1024);
    // pcb->xtttp_stc->output   = (char *)malloc(1024 * sizeof(char));
    memset(pcb->xtttp_stc->sink.input, 0, sizeof(pcb->xtttp_stc->sink.input));
    pcb->xtttp_stc->sink.input_flags = 0;
}

int check_terminal_init_status()
{
    tcb_t current = get_current_task();
    // Leopard services (mountd/getty/oobe) are started by PID1 rather than from a
    // terminal, and the XACT 2026v5 CRT probes this for every process. Walk the
    // parent chain so a child of a shell still sees the owning getty.
    if (current == NULL || current->parent_group == NULL) return 0;

    pcb_t process = current->parent_group->parent_task;
    while (process != NULL && process != kernel_group) {
        if (process->xtttp_stc != NULL && process->xtttp_stc->is_shell) return 1;
        process = process->parent_task;
    }
    return 0;
}

int mark_process_is_terminal()
{
    tcb_t current = get_current_task();
    if (current == NULL || current->parent_group == NULL || current->parent_group->xtttp_stc == NULL)
        return -EINVAL;

    // A nested getty/terminal must not steal the existing console's focus.
    pcb_t ancestor = current->parent_group->parent_task;
    while (ancestor != NULL && ancestor != kernel_group) {
        if (ancestor->xtttp_stc != NULL && ancestor->xtttp_stc->is_shell)
            return -EALREADY;
        ancestor = ancestor->parent_task;
    }

    write_serial_fmt("Marking Terminal...\n");
    init_process_xtttp(current->parent_group);
    current->parent_group->xtttp_stc->is_shell = true;
    int sid = input_sink_register(&current->parent_group->xtttp_stc->sink);
    if (sid >= 0)
        input_sink_set_focus((uint32_t)sid);
    return 0;
}

int check_input_waiting_status()
{
    tcb_t current = get_current_task();
    if (current == NULL || current->parent_group == NULL || current->parent_group->xtttp_stc == NULL ||
        !current->parent_group->xtttp_stc->sink.wait_for_input)
        return 0;

    int status = XTTTP_INPUT_STATUS_WAITING;
    if ((current->parent_group->xtttp_stc->sink.input_flags & XAPI_INPUT_NO_ECHO) != 0)
        status |= XTTTP_INPUT_STATUS_NO_ECHO;
    return status;
}

int read_terminal_app_output_buffer(char *str, size_t capacity)
{
    tcb_t current = get_current_task();
    if (str == NULL || current == NULL || current->parent_group == NULL || current->parent_group->xtttp_stc == NULL)
        return -EFAULT;
    if (capacity == 0)
        return -EINVAL;

    char output[1024];
    memset(output, 0, sizeof(output));
    if (current->parent_group->xtttp_stc->output_lock)
        return copy_to_user_pagedir(current->parent_group->pagedir, str, output, 1) ? 0 : -EFAULT;

    strncpy(output, current->parent_group->xtttp_stc->output, sizeof(output) - 1);
    output[sizeof(output) - 1] = '\0';
    size_t len = strnlen(output, sizeof(output) - 1);
    if (len >= capacity)
        len = capacity - 1;
    output[len] = '\0';
    return copy_to_user_pagedir(current->parent_group->pagedir, str, output, len + 1) ? 0 : -EFAULT;
}

int write_terminal_app_output_buffer(const char *str)
{
    tcb_t current = get_current_task();
    if (str == NULL || current == NULL || current->parent_group == NULL || current->parent_group->xtttp_stc == NULL)
        return -EFAULT;

    xtttp_dtt *xtttp = current->parent_group->xtttp_stc;
    size_t cur = strnlen(xtttp->sink.input, sizeof(xtttp->sink.input));
    if (cur >= sizeof(xtttp->sink.input)) cur = 0;
    size_t space = sizeof(xtttp->sink.input) - cur - 1;
    size_t len = 0;
    while (len < space)
    {
        char ch;
        if (!copy_from_user_pagedir(current->parent_group->pagedir, &ch, str + len, 1)) return -EFAULT;
        if (ch == '\0') break;
        xtttp->sink.input[cur + len++] = ch;
    }
    xtttp->sink.input[cur + len] = '\0';
    current->parent_group->xtttp_stc->sink.input_lock = true;
    return 0;
}

void terminal_finish_app_output()
{
    tcb_t current = get_current_task();
    current->parent_group->xtttp_stc->output_lock = true;
}
