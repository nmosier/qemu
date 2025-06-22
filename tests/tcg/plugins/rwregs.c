#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static struct qemu_plugin_register *first_reg;
static GByteArray *buf;

static void insn_cb(unsigned int cpu_index, void *udata)
{
    qemu_plugin_read_register(first_reg, buf);
    qemu_plugin_write_register(first_reg, buf);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_cb,
                                              QEMU_PLUGIN_CB_RW_REGS, NULL);
    }
}

static void vcpu_init(qemu_plugin_id_t id, unsigned int vcpu_index)
{
    g_autoptr(GArray) regs = qemu_plugin_get_registers();

    if (regs->len > 0) {
        qemu_plugin_reg_descriptor *rd = &g_array_index(regs,
                    qemu_plugin_reg_descriptor, 0);
        first_reg = rd->handle;
        buf = g_byte_array_new();
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    return 0;
}
