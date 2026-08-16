/*
 * QVM: loading QEMU TCG plugins on a client's behalf.
 *
 * The rest of QVM works to make a TCG guest indistinguishable from a KVM one.
 * This is the exception, and the reason the whole exercise is interesting: the
 * guest really is being translated, so QEMU's plugin interface can watch it.
 * A client that would otherwise have no view inside its guest at all gets
 * instruction, block and memory callbacks by asking for a plugin here.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/plugin.h"
#include "qapi/error.h"
#include "exec/tb-flush.h"
#include "qvm/qvm.h"

#include "qvm-internal.h"

static QemuMutex qvm_plugin_lock;
static GPtrArray *qvm_plugin_pending;

static void __attribute__((constructor)) qvm_plugin_init(void)
{
    qemu_mutex_init(&qvm_plugin_lock);
    qvm_plugin_pending = g_ptr_array_new_with_free_func(g_free);
}

/*
 * Both entry points below build QEMU's -plugin option string, so that what a
 * client asks for here means exactly what the same words would mean on a QEMU
 * command line.
 */
static char *qvm_plugin_optstr(const char *path, const char *args)
{
    if (args && *args) {
        return g_strdup_printf("file=%s,%s", path, args);
    }
    return g_strdup_printf("file=%s", path);
}

/*
 * Hand the plugins queued before startup to qemu_init(), which loads them at
 * the point QEMU's own command line would have.
 */
void qvm_plugin_append_args(GPtrArray *argv)
{
    guint i;

    QEMU_LOCK_GUARD(&qvm_plugin_lock);
    for (i = 0; i < qvm_plugin_pending->len; i++) {
        g_ptr_array_add(argv, (char *)"-plugin");
        g_ptr_array_add(argv, g_ptr_array_index(qvm_plugin_pending, i));
    }
}

static int qvm_plugin_load_now(const char *optstr)
{
    QemuPluginList list = QTAILQ_HEAD_INITIALIZER(list);
    Error *err = NULL;
    CPUState *cpu;

    /*
     * qemu_plugin_opt_parse() reports a malformed option by exiting, which is
     * reasonable for a command line and not for a library call.  Nothing here
     * can validate the string more cheaply than QEMU does, so the contract in
     * qvm.h is simply that it is QEMU's syntax.
     */
    qemu_plugin_opt_parse(optstr, &list);

    bql_lock();
    if (qemu_plugin_load_list(&list, &err) != 0) {
        bql_unlock();
        error_report_err(err);
        return qvm_err(EINVAL);
    }

    /*
     * A plugin instruments guest code as it is translated, so anything already
     * translated would run past it unseen.  Throw that away: it costs one
     * retranslation and means a plugin loaded mid-run still observes every
     * instruction the guest executes from now on.
     *
     * The flush itself has to happen on a vCPU, in a context where no other is
     * executing translated code -- this is a client thread, which is neither.
     * Queue it instead, so each vCPU flushes on its way into the next run.
     */
    CPU_FOREACH(cpu) {
        queue_tb_flush(cpu);
    }
    bql_unlock();

    return 0;
}

int qvm_load_plugin(const char *path, const char *args)
{
    g_autofree char *optstr = NULL;

    if (!path || !*path) {
        return qvm_err(EINVAL);
    }

    optstr = qvm_plugin_optstr(path, args);

    qemu_mutex_lock(&qvm_plugin_lock);
    if (!qvm_qemu_is_running()) {
        /*
         * Nothing to load into yet.  Queue it so that it is loaded during
         * startup rather than bolted on afterwards, which is both simpler and
         * what a client that configures before running expects.
         */
        g_ptr_array_add(qvm_plugin_pending, g_steal_pointer(&optstr));
        qemu_mutex_unlock(&qvm_plugin_lock);
        return 0;
    }
    qemu_mutex_unlock(&qvm_plugin_lock);

    return qvm_plugin_load_now(optstr);
}
