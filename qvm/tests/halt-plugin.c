/*
 * A plugin that stops the guest.
 *
 * Counts instructions and calls qemu_plugin_halt_vcpu() once it has seen
 * enough.  The point is not the counting -- a client can already bound a run
 * with qvm_vcpu_set_insn_budget() -- but that the decision is made inside the
 * plugin, from whatever it observes.  Until qemu_plugin_halt_vcpu() existed
 * the only way for a plugin to stop a guest was exit(), which for a library
 * like QVM would take the client's process down with it.
 *
 * Arguments: insns=N   halt after N instructions have executed
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 */

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t halt_after;
static uint64_t executed;
static bool halted;

static void insn_exec(unsigned int cpu_index, void *udata)
{
    if (halted) {
        return;
    }

    if (++executed >= halt_after) {
        halted = true;
        g_autofree char *msg =
            g_strdup_printf("halt-plugin: stopping after %" PRIu64
                            " instructions\n", executed);
        qemu_plugin_outs(msg);
        qemu_plugin_halt_vcpu(cpu_index);
    }
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec,
                                               QEMU_PLUGIN_CB_NO_REGS, NULL);
    }
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    halt_after = 1000;

    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) tokens = g_strsplit(argv[i], "=", 2);

        if (g_strcmp0(tokens[0], "insns") == 0) {
            halt_after = g_ascii_strtoull(tokens[1], NULL, 10);
        } else {
            fprintf(stderr, "halt-plugin: unknown option %s\n", argv[i]);
            return -1;
        }
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
