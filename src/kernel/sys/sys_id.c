/* CosmoRT — Identity syscalls
 * Per-process POSIX credentials: ruid/euid/suid + rgid/egid/sgid +
 * supplementary groups. Init: root (0/0). fork/exec inherit.
 * setuid/seteuid/setreuid/setresuid implement POSIX rules.
 * DAC enforcement happens in VFS (chmod/chown/open/chown-clear). */

#include "internal.h"
#include "proc/process.h"

#define UID_ROOT 0

static process_t *cred_self(void) { return proc_current(); }

/* Linux security/commoncap.c cap_emulate_setxuid: on uid transitions
 * involving root, narrow the effective/permitted cap sets so that an
 * unprivileged uid actually loses its capabilities. Called after the
 * new r/e/s uids are written, with the prior values passed in.
 * Assumes SECURE_KEEP_CAPS is off (we don't implement securebits). */
static void cap_emulate_setxuid(process_t *p, uint32_t old_ruid,
                                uint32_t old_euid, uint32_t old_suid) {
    int had_root = (old_ruid == UID_ROOT) || (old_euid == UID_ROOT) ||
                   (old_suid == UID_ROOT);
    int has_root = (p->ruid == UID_ROOT) || (p->euid == UID_ROOT) ||
                   (p->suid == UID_ROOT);
    if (had_root && !has_root) {
        p->cap_permitted = 0;
        p->cap_effective = 0;
    }
    if (old_euid != UID_ROOT && p->euid == UID_ROOT)
        p->cap_effective = 0;
    if (old_euid == UID_ROOT && p->euid != UID_ROOT)
        p->cap_effective = 0;
}

long do_getuid(void)  { process_t *p = cred_self(); return p ? (long)p->ruid : 0; }
long do_getgid(void)  { process_t *p = cred_self(); return p ? (long)p->rgid : 0; }
long do_geteuid(void) { process_t *p = cred_self(); return p ? (long)p->euid : 0; }
long do_getegid(void) { process_t *p = cred_self(); return p ? (long)p->egid : 0; }

/* setuid(uid):
 * - privileged (euid==0): set real, effective, saved to uid
 * - unprivileged: only permitted if uid equals ruid or suid → set euid
 *   (real and saved unchanged). Otherwise EPERM. */
long do_setuid(long uid) {
    if (uid < 0) return -EINVAL;
    process_t *p = cred_self();
    if (!p) return -EFAULT;
    uint32_t u = (uint32_t)uid;
    uint32_t old_ruid = p->ruid, old_euid = p->euid, old_suid = p->suid;
    if (p->euid == UID_ROOT) {
        p->ruid = p->euid = p->suid = p->fsuid = u;
        cap_emulate_setxuid(p, old_ruid, old_euid, old_suid);
        return 0;
    }
    if (u == p->ruid || u == p->suid) {
        p->euid = u;
        p->fsuid = u;
        cap_emulate_setxuid(p, old_ruid, old_euid, old_suid);
        return 0;
    }
    return -EPERM;
}

long do_setgid(long gid) {
    if (gid < 0) return -EINVAL;
    process_t *p = cred_self();
    if (!p) return -EFAULT;
    uint32_t g = (uint32_t)gid;
    if (p->euid == UID_ROOT) {
        p->rgid = p->egid = p->sgid = p->fsgid = g;
        return 0;
    }
    if (g == p->rgid || g == p->sgid) {
        p->egid = g;
        p->fsgid = g;
        return 0;
    }
    return -EPERM;
}

/* setreuid(ruid, euid) — -1 means don't change.
 * Privileged: any values accepted.
 * Unprivileged: new_ruid ∈ {cur_ruid, cur_euid} and
 *               new_euid ∈ {cur_ruid, cur_euid, cur_suid}.
 * Also: if ruid was set, or euid was set to something != cur_ruid,
 * then saved uid is set to new_euid. */
long do_setreuid(long ruid, long euid) {
    process_t *p = cred_self();
    if (!p) return -EFAULT;
    uint32_t new_ruid = (ruid == -1) ? p->ruid : (uint32_t)ruid;
    uint32_t new_euid = (euid == -1) ? p->euid : (uint32_t)euid;
    if (p->euid != UID_ROOT) {
        if (ruid != -1 && new_ruid != p->ruid && new_ruid != p->euid)
            return -EPERM;
        if (euid != -1 && new_euid != p->ruid && new_euid != p->euid && new_euid != p->suid)
            return -EPERM;
    }
    uint32_t old_ruid = p->ruid, old_euid = p->euid, old_suid = p->suid;
    int set_saved = (ruid != -1) || (euid != -1 && new_euid != p->ruid);
    p->ruid = new_ruid;
    p->euid = new_euid;
    p->fsuid = new_euid;
    if (set_saved) p->suid = new_euid;
    cap_emulate_setxuid(p, old_ruid, old_euid, old_suid);
    return 0;
}

long do_setregid(long rgid, long egid) {
    process_t *p = cred_self();
    if (!p) return -EFAULT;
    uint32_t new_rgid = (rgid == -1) ? p->rgid : (uint32_t)rgid;
    uint32_t new_egid = (egid == -1) ? p->egid : (uint32_t)egid;
    if (p->euid != UID_ROOT) {
        if (rgid != -1 && new_rgid != p->rgid && new_rgid != p->egid)
            return -EPERM;
        if (egid != -1 && new_egid != p->rgid && new_egid != p->egid && new_egid != p->sgid)
            return -EPERM;
    }
    int set_saved = (rgid != -1) || (egid != -1 && new_egid != p->rgid);
    p->rgid = new_rgid;
    p->egid = new_egid;
    p->fsgid = new_egid;
    if (set_saved) p->sgid = new_egid;
    return 0;
}

long do_setresuid(long ruid, long euid, long suid) {
    process_t *p = cred_self();
    if (!p) return -EFAULT;
    uint32_t new_ruid = (ruid == -1) ? p->ruid : (uint32_t)ruid;
    uint32_t new_euid = (euid == -1) ? p->euid : (uint32_t)euid;
    uint32_t new_suid = (suid == -1) ? p->suid : (uint32_t)suid;
    if (p->euid != UID_ROOT) {
        if (ruid != -1 && new_ruid != p->ruid && new_ruid != p->euid && new_ruid != p->suid)
            return -EPERM;
        if (euid != -1 && new_euid != p->ruid && new_euid != p->euid && new_euid != p->suid)
            return -EPERM;
        if (suid != -1 && new_suid != p->ruid && new_suid != p->euid && new_suid != p->suid)
            return -EPERM;
    }
    uint32_t old_ruid = p->ruid, old_euid = p->euid, old_suid = p->suid;
    p->ruid = new_ruid;
    p->euid = new_euid;
    p->suid = new_suid;
    p->fsuid = new_euid;
    cap_emulate_setxuid(p, old_ruid, old_euid, old_suid);
    return 0;
}

long do_setresgid(long rgid, long egid, long sgid) {
    process_t *p = cred_self();
    if (!p) return -EFAULT;
    uint32_t new_rgid = (rgid == -1) ? p->rgid : (uint32_t)rgid;
    uint32_t new_egid = (egid == -1) ? p->egid : (uint32_t)egid;
    uint32_t new_sgid = (sgid == -1) ? p->sgid : (uint32_t)sgid;
    if (p->euid != UID_ROOT) {
        if (rgid != -1 && new_rgid != p->rgid && new_rgid != p->egid && new_rgid != p->sgid)
            return -EPERM;
        if (egid != -1 && new_egid != p->rgid && new_egid != p->egid && new_egid != p->sgid)
            return -EPERM;
        if (sgid != -1 && new_sgid != p->rgid && new_sgid != p->egid && new_sgid != p->sgid)
            return -EPERM;
    }
    p->rgid = new_rgid;
    p->egid = new_egid;
    p->sgid = new_sgid;
    p->fsgid = new_egid;
    return 0;
}

/* setfsuid/setfsgid return the *previous* value, regardless of success. */
long do_setfsuid(long uid) {
    process_t *p = cred_self();
    if (!p) return 0;
    uint32_t prev = p->fsuid;
    if (uid < 0) return (long)prev;
    uint32_t u = (uint32_t)uid;
    if (p->euid == UID_ROOT || u == p->ruid || u == p->euid ||
        u == p->suid || u == p->fsuid)
        p->fsuid = u;
    return (long)prev;
}

long do_setfsgid(long gid) {
    process_t *p = cred_self();
    if (!p) return 0;
    uint32_t prev = p->fsgid;
    if (gid < 0) return (long)prev;
    uint32_t g = (uint32_t)gid;
    if (p->euid == UID_ROOT || g == p->rgid || g == p->egid ||
        g == p->sgid || g == p->fsgid)
        p->fsgid = g;
    return (long)prev;
}

long do_getresuid(long *ruid, long *euid, long *suid) {
    process_t *p = cred_self();
    if (!p) return -EFAULT;
    long r = (long)p->ruid, e = (long)p->euid, s = (long)p->suid;
    if (ruid && user_ok((uint64_t)ruid, 4)) copy_to_user(ruid, &r, 4);
    if (euid && user_ok((uint64_t)euid, 4)) copy_to_user(euid, &e, 4);
    if (suid && user_ok((uint64_t)suid, 4)) copy_to_user(suid, &s, 4);
    return 0;
}

long do_getresgid(long *rgid, long *egid, long *sgid) {
    process_t *p = cred_self();
    if (!p) return -EFAULT;
    long r = (long)p->rgid, e = (long)p->egid, s = (long)p->sgid;
    if (rgid && user_ok((uint64_t)rgid, 4)) copy_to_user(rgid, &r, 4);
    if (egid && user_ok((uint64_t)egid, 4)) copy_to_user(egid, &e, 4);
    if (sgid && user_ok((uint64_t)sgid, 4)) copy_to_user(sgid, &s, 4);
    return 0;
}
