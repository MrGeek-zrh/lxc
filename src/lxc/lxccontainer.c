/* liblxcapi
 *
 * Copyright © 2012 Serge Hallyn <serge.hallyn@ubuntu.com>.
 * Copyright © 2012 Canonical Ltd.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.

 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.

 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <cstring>
#include <stdio.h>
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <dirent.h>
#include <sched.h>
#include <arpa/inet.h>
#include <libgen.h>
#include <stdint.h>
#include <grp.h>
#include <sys/syscall.h>

#include <lxc/lxccontainer.h>
#include <lxc/version.h>
#include <lxc/network.h>

#include "config.h"
#include "lxc.h"
#include "state.h"
#include "conf.h"
#include "confile.h"
#include "console.h"
#include "cgroup.h"
#include "commands.h"
#include "log.h"
#include "bdev.h"
#include "utils.h"
#include "attach.h"
#include "monitor.h"
#include "namespace.h"
#include "network.h"
#include "lxclock.h"
#include "sync.h"

#if HAVE_IFADDRS_H
#include <ifaddrs.h>
#else
#include <../include/ifaddrs.h>
#endif

#define MAX_BUFFER 4096

#define NOT_SUPPORTED_ERROR "the requested function %s is not currently supported with unprivileged containers"

/* Define faccessat() if missing from the C library */
#ifndef HAVE_FACCESSAT
static int faccessat(int __fd, const char *__file, int __type, int __flag)
{
#ifdef __NR_faccessat
    return syscall(__NR_faccessat, __fd, __file, __type, __flag);
#else
    errno = ENOSYS;
    return -1;
#endif
}
#endif

lxc_log_define(lxc_container, lxc);

static bool config_file_exists(const char *lxcpath, const char *cname)
{
    /* $lxcpath + '/' + $cname + '/config' + \0 */
    int ret, len = strlen(lxcpath) + strlen(cname) + 9;
    char *fname = alloca(len);

    ret = snprintf(fname, len, "%s/%s/config", lxcpath, cname);
    if (ret < 0 || ret >= len)
        return false;

    return file_exists(fname);
}

/*
 * A few functions to help detect when a container creation failed.
 * If a container creation was killed partway through, then trying
 * to actually start that container could harm the host.  We detect
 * this by creating a 'partial' file under the container directory,
 * and keeping an advisory lock.  When container creation completes,
 * we remove that file.  When we load or try to start a container, if
 * we find that file, without a flock, we remove the container.
 */
static int ongoing_create(struct lxc_container *c)
{
    int len = strlen(c->config_path) + strlen(c->name) + 10;
    char *path = alloca(len);
    int fd, ret;
    struct flock lk;

    ret = snprintf(path, len, "%s/%s/partial", c->config_path, c->name);
    if (ret < 0 || ret >= len) {
        ERROR("Error writing partial pathname");
        return -1;
    }

    if (!file_exists(path))
        return 0;
    fd = open(path, O_RDWR);
    if (fd < 0) {
        // give benefit of the doubt
        SYSERROR("Error opening partial file");
        return 0;
    }
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 0;
    lk.l_pid = -1;
    if (fcntl(fd, F_GETLK, &lk) == 0 && lk.l_pid != -1) {
        // create is still ongoing
        close(fd);
        return 1;
    }
    // create completed but partial is still there.
    close(fd);
    return 2;
}

static int create_partial(struct lxc_container *c)
{
    // $lxcpath + '/' + $name + '/partial' + \0
    int len = strlen(c->config_path) + strlen(c->name) + 10;
    char *path = alloca(len);
    int fd, ret;
    struct flock lk;

    ret = snprintf(path, len, "%s/%s/partial", c->config_path, c->name);
    if (ret < 0 || ret >= len) {
        ERROR("Error writing partial pathname");
        return -1;
    }
    if ((fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0755)) < 0) {
        SYSERROR("Erorr creating partial file");
        return -1;
    }
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 0;
    if (fcntl(fd, F_SETLKW, &lk) < 0) {
        SYSERROR("Error locking partial file %s", path);
        close(fd);
        return -1;
    }

    return fd;
}

static void remove_partial(struct lxc_container *c, int fd)
{
    // $lxcpath + '/' + $name + '/partial' + \0
    int len = strlen(c->config_path) + strlen(c->name) + 10;
    char *path = alloca(len);
    int ret;

    close(fd);
    ret = snprintf(path, len, "%s/%s/partial", c->config_path, c->name);
    if (ret < 0 || ret >= len) {
        ERROR("Error writing partial pathname");
        return;
    }
    if (unlink(path) < 0)
        SYSERROR("Error unlink partial file %s", path);
}

/* LOCKING
 * 1. container_mem_lock(c) protects the struct lxc_container from multiple threads.
 * 2. container_disk_lock(c) protects the on-disk container data - in particular the
 *    container configuration file.
 *    The container_disk_lock also takes the container_mem_lock.
 * 3. thread_mutex protects process data (ex: fd table) from multiple threads.
 * NOTHING mutexes two independent programs with their own struct
 * lxc_container for the same c->name, between API calls.  For instance,
 * c->config_read(); c->start();  Between those calls, data on disk
 * could change (which shouldn't bother the caller unless for instance
 * the rootfs get moved).  c->config_read(); update; c->config_write();
 * Two such updaters could race.  The callers should therefore check their
 * results.  Trying to prevent that would necessarily expose us to deadlocks
 * due to hung callers.  So I prefer to keep the locks only within our own
 * functions, not across functions.
 *
 * If you're going to clone while holding a lxccontainer, increment
 * c->numthreads (under privlock) before forking.  When deleting,
 * decrement numthreads under privlock, then if it hits 0 you can delete.
 * Do not ever use a lxccontainer whose numthreads you did not bump.
 */

static void lxc_container_free(struct lxc_container *c)
{
    if (!c)
        return;

    free(c->configfile);
    c->configfile = NULL;
    free(c->error_string);
    c->error_string = NULL;
    if (c->slock) {
        lxc_putlock(c->slock);
        c->slock = NULL;
    }
    if (c->privlock) {
        lxc_putlock(c->privlock);
        c->privlock = NULL;
    }
    free(c->name);
    c->name = NULL;
    if (c->lxc_conf) {
        lxc_conf_free(c->lxc_conf);
        c->lxc_conf = NULL;
    }
    free(c->config_path);
    c->config_path = NULL;

    free(c);
}

/*
 * Consider the following case:
freer                         |    racing get()er
==================================================================
lxc_container_put()           |   lxc_container_get()
\ lxclock(c->privlock)        |   c->numthreads < 1? (no)
\ c->numthreads = 0           |   \ lxclock(c->privlock) -> waits
\ lxcunlock()                 |   \
\ lxc_container_free()        |   \ lxclock() returns
                              |   \ c->numthreads < 1 -> return 0
\ \ (free stuff)              |
\ \ sem_destroy(privlock)     |

 * When the get()er checks numthreads the first time, one of the following
 * is true:
 * 1. freer has set numthreads = 0.  get() returns 0
 * 2. freer is between lxclock and setting numthreads to 0.  get()er will
 *    sem_wait on privlock, get lxclock after freer() drops it, then see
 *    numthreads is 0 and exit without touching lxclock again..
 * 3. freer has not yet locked privlock.  If get()er runs first, then put()er
 *    will see --numthreads = 1 and not call lxc_container_free().
*/

int lxc_container_get(struct lxc_container *c)
{
    if (!c)
        return 0;

    // if someone else has already started freeing the container, don't
    // try to take the lock, which may be invalid
    if (c->numthreads < 1)
        return 0;

    if (container_mem_lock(c))
        return 0;
    if (c->numthreads < 1) {
        // bail without trying to unlock, bc the privlock is now probably
        // in freed memory
        return 0;
    }
    c->numthreads++;
    container_mem_unlock(c);
    return 1;
}

int lxc_container_put(struct lxc_container *c)
{
    if (!c)
        return -1;
    if (container_mem_lock(c))
        return -1;
    if (--c->numthreads < 1) {
        container_mem_unlock(c);
        lxc_container_free(c);
        return 1;
    }
    container_mem_unlock(c);
    return 0;
}

static bool lxcapi_is_defined(struct lxc_container *c)
{
    struct stat statbuf;
    bool ret = false;
    int statret;

    if (!c)
        return false;

    if (container_mem_lock(c))
        return false;
    if (!c->configfile)
        goto out;
    statret = stat(c->configfile, &statbuf);
    if (statret != 0)
        goto out;
    ret = true;

out:
    container_mem_unlock(c);
    return ret;
}

static const char *lxcapi_state(struct lxc_container *c)
{
    lxc_state_t s;

    if (!c)
        return NULL;
    s = lxc_getstate(c->name, c->config_path);
    return lxc_state2str(s);
}

static bool is_stopped(struct lxc_container *c)
{
    lxc_state_t s;
    s = lxc_getstate(c->name, c->config_path);
    return (s == STOPPED);
}

static bool lxcapi_is_running(struct lxc_container *c)
{
    const char *s;

    if (!c)
        return false;
    s = lxcapi_state(c);
    if (!s || strcmp(s, "STOPPED") == 0)
        return false;
    return true;
}

static bool lxcapi_freeze(struct lxc_container *c)
{
    int ret;
    if (!c)
        return false;

    ret = lxc_freeze(c->name, c->config_path);
    if (ret)
        return false;
    return true;
}

static bool lxcapi_unfreeze(struct lxc_container *c)
{
    int ret;
    if (!c)
        return false;

    ret = lxc_unfreeze(c->name, c->config_path);
    if (ret)
        return false;
    return true;
}

static int lxcapi_console_getfd(struct lxc_container *c, int *ttynum, int *masterfd)
{
    int ttyfd;
    if (!c)
        return -1;

    ttyfd = lxc_console_getfd(c, ttynum, masterfd);
    return ttyfd;
}

static int lxcapi_console(struct lxc_container *c, int ttynum, int stdinfd, int stdoutfd, int stderrfd, int escape)
{
    return lxc_console(c, ttynum, stdinfd, stdoutfd, stderrfd, escape);
}

static pid_t lxcapi_init_pid(struct lxc_container *c)
{
    if (!c)
        return -1;

    return lxc_cmd_get_init_pid(c->name, c->config_path);
}

static bool load_config_locked(struct lxc_container *c, const char *fname)
{
    if (!c->lxc_conf)
        c->lxc_conf = lxc_conf_init();
    if (!c->lxc_conf)
        return false;
    if (lxc_config_read(fname, c->lxc_conf, false) != 0)
        return false;
    return true;
}

static bool lxcapi_load_config(struct lxc_container *c, const char *alt_file)
{
    bool ret = false, need_disklock = false;
    int lret;
    const char *fname;
    if (!c)
        return false;

    fname = c->configfile;
    if (alt_file)
        fname = alt_file;
    if (!fname)
        return false;
    /*
	 * If we're reading something other than the container's config,
	 * we only need to lock the in-memory container.  If loading the
	 * container's config file, take the disk lock.
	 */
    if (strcmp(fname, c->configfile) == 0)
        need_disklock = true;

    if (need_disklock)
        lret = container_disk_lock(c);
    else
        lret = container_mem_lock(c);
    if (lret)
        return false;

    ret = load_config_locked(c, fname);

    if (need_disklock)
        container_disk_unlock(c);
    else
        container_mem_unlock(c);
    return ret;
}

static bool lxcapi_want_daemonize(struct lxc_container *c, bool state)
{
    if (!c || !c->lxc_conf)
        return false;
    if (container_mem_lock(c)) {
        ERROR("Error getting mem lock");
        return false;
    }
    c->daemonize = state;
    container_mem_unlock(c);
    return true;
}

static bool lxcapi_want_close_all_fds(struct lxc_container *c, bool state)
{
    if (!c || !c->lxc_conf)
        return false;
    if (container_mem_lock(c)) {
        ERROR("Error getting mem lock");
        return false;
    }
    c->lxc_conf->close_all_fds = state;
    container_mem_unlock(c);
    return true;
}

static bool lxcapi_wait(struct lxc_container *c, const char *state, int timeout)
{
    int ret;

    if (!c)
        return false;

    ret = lxc_wait(c->name, state, timeout, c->config_path);
    return ret == 0;
}

static bool wait_on_daemonized_start(struct lxc_container *c, int pid)
{
    /* we'll probably want to make this timeout configurable? */
    int timeout = 5, ret, status;

    /*
	 * our child is going to fork again, then exit.  reap the
	 * child
	 */
    ret = waitpid(pid, &status, 0);
    if (ret == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        DEBUG("failed waiting for first dual-fork child");
    return lxcapi_wait(c, "RUNNING", timeout);
}

static bool am_single_threaded(void)
{
    struct dirent dirent, *direntp;
    DIR *dir;
    int count = 0;

    dir = opendir("/proc/self/task");
    if (!dir) {
        INFO("failed to open /proc/self/task");
        return false;
    }

    while (!readdir_r(dir, &dirent, &direntp)) {
        if (!direntp)
            break;

        if (!strcmp(direntp->d_name, "."))
            continue;

        if (!strcmp(direntp->d_name, ".."))
            continue;
        if (++count > 1)
            break;
    }
    closedir(dir);
    return count == 1;
}

/*
 * I can't decide if it'd be more convenient for callers if we accept '...',
 * or a null-terminated array (i.e. execl vs execv)
 */
static bool lxcapi_start(struct lxc_container *c, int useinit, char *const argv[])
{
    int ret;
    struct lxc_conf *conf;
    bool daemonize = false;
    FILE *pid_fp = NULL;
    char *default_args[] = {
        "/sbin/init",
        NULL,
    };
    char *init_cmd[2];

    /* container exists */
    if (!c)
        return false;
    /* container has been setup */
    if (!c->lxc_conf)
        return false;

    if ((ret = ongoing_create(c)) < 0) {
        ERROR("Error checking for incomplete creation");
        return false;
    }
    if (ret == 2) {
        ERROR("Error: %s creation was not completed", c->name);
        c->destroy(c);
        return false;
    } else if (ret == 1) {
        ERROR("Error: creation of %s is ongoing", c->name);
        return false;
    }

    /* is this app meant to be run through lxcinit, as in lxc-execute? */
    if (useinit && !argv)
        return false;

    if (container_mem_lock(c))
        return false;
    conf = c->lxc_conf;
    daemonize = c->daemonize;
    container_mem_unlock(c);

    if (useinit) {
        ret = lxc_execute(c->name, argv, 1, conf, c->config_path);
        return ret == 0 ? true : false;
    }

    if (!argv) {
        if (conf->init_cmd) {
            init_cmd[0] = conf->init_cmd;
            init_cmd[1] = NULL;
            argv = init_cmd;
        } else
            argv = default_args;
    }

    /*
	* say, I'm not sure - what locks do we want here?  Any?
	* Is liblxc's locking enough here to protect the on disk
	* container?  We don't want to exclude things like lxc_info
	* while container is running...
	*/
    if (daemonize) {
        lxc_monitord_spawn(c->config_path);

        pid_t pid = fork();
        if (pid < 0)
            return false;

        if (pid != 0) {
            /* Set to NULL because we don't want father unlink
			 * the PID file, child will do the free and unlink.
			 */
            c->pidfile = NULL;
            return wait_on_daemonized_start(c, pid);
        }

        /* second fork to be reparented by init */
        pid = fork();
        if (pid < 0) {
            SYSERROR("Error doing dual-fork");
            return false;
        }
        if (pid != 0)
            exit(0);
        /* like daemon(), chdir to / and redirect 0,1,2 to /dev/null */
        if (chdir("/")) {
            SYSERROR("Error chdir()ing to /.");
            return false;
        }
        lxc_check_inherited(conf, true, -1);
        close(0);
        close(1);
        close(2);
        open("/dev/zero", O_RDONLY);
        open("/dev/null", O_RDWR);
        open("/dev/null", O_RDWR);
        setsid();
    } else {
        if (!am_single_threaded()) {
            ERROR("Cannot start non-daemonized container when threaded");
            return false;
        }
    }

    /* We need to write PID file after daeminize, so we always
	 * write the right PID.
	 */
    if (c->pidfile) {
        pid_fp = fopen(c->pidfile, "w");
        if (pid_fp == NULL) {
            SYSERROR("Failed to create pidfile '%s' for '%s'", c->pidfile, c->name);
            return false;
        }

        if (fprintf(pid_fp, "%d\n", getpid()) < 0) {
            SYSERROR("Failed to write '%s'", c->pidfile);
            fclose(pid_fp);
            pid_fp = NULL;
            return false;
        }

        fclose(pid_fp);
        pid_fp = NULL;
    }

reboot:
    conf->reboot = 0;

    if (lxc_check_inherited(conf, daemonize, -1)) {
        ERROR("Inherited fds found");
        ret = 1;
        goto out;
    }

    ret = lxc_start(c->name, argv, conf, c->config_path);
    c->error_num = ret;

    if (conf->reboot) {
        INFO("container requested reboot");
        conf->reboot = 0;
        goto reboot;
    }

out:
    if (c->pidfile) {
        unlink(c->pidfile);
        free(c->pidfile);
        c->pidfile = NULL;
    }

    if (daemonize)
        exit(ret == 0 ? true : false);
    else
        return (ret == 0 ? true : false);
}

/*
 * note there MUST be an ending NULL
 */
static bool lxcapi_startl(struct lxc_container *c, int useinit, ...)
{
    va_list ap;
    char **inargs = NULL;
    bool bret = false;

    /* container exists */
    if (!c)
        return false;

    va_start(ap, useinit);
    inargs = lxc_va_arg_list_to_argv(ap, 0, 1);
    va_end(ap);

    if (!inargs) {
        ERROR("Memory allocation error.");
        goto out;
    }

    /* pass NULL if no arguments were supplied */
    bret = lxcapi_start(c, useinit, *inargs ? inargs : NULL);

out:
    if (inargs) {
        char **arg;
        for (arg = inargs; *arg; arg++)
            free(*arg);
        free(inargs);
    }

    return bret;
}

static bool lxcapi_stop(struct lxc_container *c)
{
    int ret;

    if (!c)
        return false;

    ret = lxc_cmd_stop(c->name, c->config_path);

    return ret == 0;
}

static int do_create_container_dir(const char *path, struct lxc_conf *conf)
{
    int ret = -1, lasterr;
    char *p = alloca(strlen(path) + 1);
    mode_t mask = umask(0002);
    ret = mkdir(path, 0770);
    lasterr = errno;
    umask(mask);
    errno = lasterr;
    if (ret) {
        if (errno == EEXIST)
            ret = 0;
        else {
            SYSERROR("failed to create container path %s", path);
            return -1;
        }
    }
    strcpy(p, path);
    if (!lxc_list_empty(&conf->id_map) && chown_mapped_root(p, conf) != 0) {
        ERROR("Failed to chown container dir");
        ret = -1;
    }
    return ret;
}

/*
 * create the standard expected container dir
 */
static bool create_container_dir(struct lxc_container *c)
{
    char *s;
    int len, ret;

    len = strlen(c->config_path) + strlen(c->name) + 2;
    s = malloc(len);
    if (!s)
        return false;
    ret = snprintf(s, len, "%s/%s", c->config_path, c->name);
    if (ret < 0 || ret >= len) {
        free(s);
        return false;
    }
    ret = do_create_container_dir(s, c->lxc_conf);
    free(s);
    return ret == 0;
}

static const char *lxcapi_get_config_path(struct lxc_container *c);
static bool lxcapi_set_config_item(struct lxc_container *c, const char *key, const char *v);

/*
 * do_bdev_create: thin wrapper around bdev_create().  Like bdev_create(),
 * it returns a mounted bdev on success, NULL on error.
 */
static struct bdev *do_bdev_create(struct lxc_container *c, const char *type, struct bdev_specs *specs)
{
    char *dest;
    size_t len;
    struct bdev *bdev;
    int ret;

    /* rootfs.path or lxcpath/lxcname/rootfs */
    if (c->lxc_conf->rootfs.path && access(c->lxc_conf->rootfs.path, F_OK) == 0) {
        const char *rpath = c->lxc_conf->rootfs.path;
        len = strlen(rpath) + 1;
        dest = alloca(len);
        ret = snprintf(dest, len, "%s", rpath);
    } else {
        const char *lxcpath = lxcapi_get_config_path(c);
        len = strlen(c->name) + strlen(lxcpath) + 9;
        dest = alloca(len);
        ret = snprintf(dest, len, "%s/%s/rootfs", lxcpath, c->name);
    }
    if (ret < 0 || ret >= len)
        return NULL;

    bdev = bdev_create(dest, type, c->name, specs);
    if (!bdev) {
        ERROR("Failed to create backing store type %s", type);
        return NULL;
    }

    lxcapi_set_config_item(c, "lxc.rootfs", bdev->src);

    /* if we are not root, chown the rootfs dir to root in the
	 * target uidmap */

    if (geteuid() != 0 || (c->lxc_conf && !lxc_list_empty(&c->lxc_conf->id_map))) {
        if (chown_mapped_root(bdev->dest, c->lxc_conf) < 0) {
            ERROR("Error chowning %s to container root", bdev->dest);
            suggest_default_idmap();
            bdev_put(bdev);
            return NULL;
        }
    }

    return bdev;
}

static char *lxcbasename(char *path)
{
    char *p = path + strlen(path) - 1;
    while (*p != '/' && p > path)
        p--;
    return p;
}

static bool create_run_template(struct lxc_container *c, char *tpath, bool quiet, char *const argv[])
{
    pid_t pid;

    if (!tpath)
        return true;

    pid = fork();
    if (pid < 0) {
        SYSERROR("failed to fork task for container creation template");
        return false;
    }

    if (pid == 0) { // child
        char *patharg, *namearg, *rootfsarg, *src;
        struct bdev *bdev = NULL;
        int i;
        int ret, len, nargs = 0;
        char **newargv;
        struct lxc_conf *conf = c->lxc_conf;

        if (quiet) {
            close(0);
            close(1);
            close(2);
            open("/dev/zero", O_RDONLY);
            open("/dev/null", O_RDWR);
            open("/dev/null", O_RDWR);
        }

        src = c->lxc_conf->rootfs.path;
        /*
		 * for an overlay create, what the user wants is the template to fill
		 * in what will become the readonly lower layer.  So don't mount for
		 * the template
		 */
        if (strncmp(src, "overlayfs:", 10) == 0)
            src = overlay_getlower(src + 10);
        if (strncmp(src, "aufs:", 5) == 0)
            src = overlay_getlower(src + 5);

        bdev = bdev_init(c->lxc_conf, src, c->lxc_conf->rootfs.mount, NULL);
        if (!bdev) {
            ERROR("Error opening rootfs");
            exit(1);
        }

        if (geteuid() == 0) {
            if (unshare(CLONE_NEWNS) < 0) {
                ERROR("error unsharing mounts");
                exit(1);
            }
            if (detect_shared_rootfs()) {
                if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL)) {
                    SYSERROR("Failed to make / rslave to run template");
                    ERROR("Continuing...");
                }
            }
        }
        if (strcmp(bdev->type, "dir") && strcmp(bdev->type, "btrfs")) {
            if (geteuid() != 0) {
                ERROR("non-root users can only create btrfs and directory-backed containers");
                exit(1);
            }
            if (bdev->ops->mount(bdev) < 0) {
                ERROR("Error mounting rootfs");
                exit(1);
            }
        } else { // TODO come up with a better way here!
            free(bdev->dest);
            bdev->dest = strdup(bdev->src);
        }

        /*
		 * create our new array, pre-pend the template name and
		 * base args
		 */
        if (argv)
            for (nargs = 0; argv[nargs]; nargs++)
                ;
        nargs += 4; // template, path, rootfs and name args

        newargv = malloc(nargs * sizeof(*newargv));
        if (!newargv)
            exit(1);
        newargv[0] = lxcbasename(tpath);

        len = strlen(c->config_path) + strlen(c->name) + strlen("--path=") + 2;
        patharg = malloc(len);
        if (!patharg)
            exit(1);
        ret = snprintf(patharg, len, "--path=%s/%s", c->config_path, c->name);
        if (ret < 0 || ret >= len)
            exit(1);
        newargv[1] = patharg;
        len = strlen("--name=") + strlen(c->name) + 1;
        namearg = malloc(len);
        if (!namearg)
            exit(1);
        ret = snprintf(namearg, len, "--name=%s", c->name);
        if (ret < 0 || ret >= len)
            exit(1);
        newargv[2] = namearg;

        len = strlen("--rootfs=") + 1 + strlen(bdev->dest);
        rootfsarg = malloc(len);
        if (!rootfsarg)
            exit(1);
        ret = snprintf(rootfsarg, len, "--rootfs=%s", bdev->dest);
        if (ret < 0 || ret >= len)
            exit(1);
        newargv[3] = rootfsarg;

        /* add passed-in args */
        if (argv)
            for (i = 4; i < nargs; i++)
                newargv[i] = argv[i - 4];

        /* add trailing NULL */
        nargs++;
        newargv = realloc(newargv, nargs * sizeof(*newargv));
        if (!newargv)
            exit(1);
        newargv[nargs - 1] = NULL;

        /*
		 * If we're running the template in a mapped userns, then
		 * we prepend the template command with:
		 * lxc-usernsexec <-m map1> ... <-m mapn> --
		 * and we append "--mapped-uid x", where x is the mapped uid
		 * for our geteuid()
		 */
        if (!lxc_list_empty(&conf->id_map)) {
            int n2args = 1;
            char txtuid[20];
            char txtgid[20];
            char **n2 = malloc(n2args * sizeof(*n2));
            struct lxc_list *it;
            struct id_map *map;

            if (!n2) {
                SYSERROR("out of memory");
                exit(1);
            }
            newargv[0] = tpath;
            tpath = "lxc-usernsexec";
            n2[0] = "lxc-usernsexec";
            lxc_list_for_each(it, &conf->id_map)
            {
                map = it->elem;
                n2args += 2;
                n2 = realloc(n2, n2args * sizeof(char *));
                if (!n2)
                    exit(1);
                n2[n2args - 2] = "-m";
                n2[n2args - 1] = malloc(200);
                if (!n2[n2args - 1])
                    exit(1);
                ret = snprintf(n2[n2args - 1], 200, "%c:%lu:%lu:%lu", map->idtype == ID_TYPE_UID ? 'u' : 'g', map->nsid,
                               map->hostid, map->range);
                if (ret < 0 || ret >= 200)
                    exit(1);
            }
            int hostid_mapped = mapped_hostid(geteuid(), conf, ID_TYPE_UID);
            int extraargs = hostid_mapped >= 0 ? 1 : 3;
            n2 = realloc(n2, (nargs + n2args + extraargs) * sizeof(char *));
            if (!n2)
                exit(1);
            if (hostid_mapped < 0) {
                hostid_mapped = find_unmapped_nsuid(conf, ID_TYPE_UID);
                n2[n2args++] = "-m";
                if (hostid_mapped < 0) {
                    ERROR("Could not find free uid to map");
                    exit(1);
                }
                n2[n2args++] = malloc(200);
                if (!n2[n2args - 1]) {
                    SYSERROR("out of memory");
                    exit(1);
                }
                ret = snprintf(n2[n2args - 1], 200, "u:%d:%d:1", hostid_mapped, geteuid());
                if (ret < 0 || ret >= 200) {
                    ERROR("string too long");
                    exit(1);
                }
            }
            int hostgid_mapped = mapped_hostid(getegid(), conf, ID_TYPE_GID);
            extraargs = hostgid_mapped >= 0 ? 1 : 3;
            n2 = realloc(n2, (nargs + n2args + extraargs) * sizeof(char *));
            if (!n2)
                exit(1);
            if (hostgid_mapped < 0) {
                hostgid_mapped = find_unmapped_nsuid(conf, ID_TYPE_GID);
                n2[n2args++] = "-m";
                if (hostgid_mapped < 0) {
                    ERROR("Could not find free uid to map");
                    exit(1);
                }
                n2[n2args++] = malloc(200);
                if (!n2[n2args - 1]) {
                    SYSERROR("out of memory");
                    exit(1);
                }
                ret = snprintf(n2[n2args - 1], 200, "g:%d:%d:1", hostgid_mapped, getegid());
                if (ret < 0 || ret >= 200) {
                    ERROR("string too long");
                    exit(1);
                }
            }
            n2[n2args++] = "--";
            for (i = 0; i < nargs; i++)
                n2[i + n2args] = newargv[i];
            n2args += nargs;
            // Finally add "--mapped-uid $uid" to tell template what to chown
            // cached images to
            n2args += 4;
            n2 = realloc(n2, n2args * sizeof(char *));
            if (!n2) {
                SYSERROR("out of memory");
                exit(1);
            }
            // note n2[n2args-1] is NULL
            n2[n2args - 5] = "--mapped-uid";
            snprintf(txtuid, 20, "%d", hostid_mapped);
            n2[n2args - 4] = txtuid;
            n2[n2args - 3] = "--mapped-gid";
            snprintf(txtgid, 20, "%d", hostgid_mapped);
            n2[n2args - 2] = txtgid;
            n2[n2args - 1] = NULL;
            free(newargv);
            newargv = n2;
        }
        /* execute */
        execvp(tpath, newargv);
        SYSERROR("failed to execute template %s", tpath);
        exit(1);
    }

    if (wait_for_pid(pid) != 0) {
        ERROR("container creation template for %s failed", c->name);
        return false;
    }

    return true;
}

static bool prepend_lxc_header(char *path, const char *t, char *const argv[])
{
    long flen;
    char *contents;
    FILE *f;
    int ret = -1;
#if HAVE_LIBGNUTLS
    int i;
    unsigned char md_value[SHA_DIGEST_LENGTH];
    char *tpath;
#endif

    f = fopen(path, "r");
    if (f == NULL)
        return false;

    if (fseek(f, 0, SEEK_END) < 0)
        goto out_error;
    if ((flen = ftell(f)) < 0)
        goto out_error;
    if (fseek(f, 0, SEEK_SET) < 0)
        goto out_error;
    if ((contents = malloc(flen + 1)) == NULL)
        goto out_error;
    if (fread(contents, 1, flen, f) != flen)
        goto out_free_contents;

    contents[flen] = '\0';
    ret = fclose(f);
    f = NULL;
    if (ret < 0)
        goto out_free_contents;

#if HAVE_LIBGNUTLS
    tpath = get_template_path(t);
    if (!tpath) {
        ERROR("bad template: %s", t);
        goto out_free_contents;
    }

    ret = sha1sum_file(tpath, md_value);
    if (ret < 0) {
        ERROR("Error getting sha1sum of %s", tpath);
        free(tpath);
        goto out_free_contents;
    }
    free(tpath);
#endif

    f = fopen(path, "w");
    if (f == NULL) {
        SYSERROR("reopening config for writing");
        free(contents);
        return false;
    }
    fprintf(f, "# Template used to create this container: %s\n", t);
    if (argv) {
        fprintf(f, "# Parameters passed to the template:");
        while (*argv) {
            fprintf(f, " %s", *argv);
            argv++;
        }
        fprintf(f, "\n");
    }
#if HAVE_LIBGNUTLS
    fprintf(f, "# Template script checksum (SHA-1): ");
    for (i = 0; i < SHA_DIGEST_LENGTH; i++)
        fprintf(f, "%02x", md_value[i]);
    fprintf(f, "\n");
#endif
    fprintf(f, "# For additional config options, please look at lxc.container.conf(5)\n");
    if (fwrite(contents, 1, flen, f) != flen) {
        SYSERROR("Writing original contents");
        free(contents);
        fclose(f);
        return false;
    }
    ret = 0;
out_free_contents:
    free(contents);
out_error:
    if (f) {
        int newret;
        newret = fclose(f);
        if (ret == 0)
            ret = newret;
    }
    if (ret < 0) {
        SYSERROR("Error prepending header");
        return false;
    }
    return true;
}

static void lxcapi_clear_config(struct lxc_container *c)
{
    if (c) {
        if (c->lxc_conf) {
            lxc_conf_free(c->lxc_conf);
            c->lxc_conf = NULL;
        }
    }
}

static bool lxcapi_destroy(struct lxc_container *c);
static bool container_destroy(struct lxc_container *c);
static bool get_snappath_dir(struct lxc_container *c, char *snappath);
/*
 * lxcapi_create:
 * create a container with the given parameters.
 * @c: container to be created.  It has the lxcpath, name, and a starting
 *     configuration already set
 * @t: the template to execute to instantiate the root filesystem and
 *     adjust the configuration.
 * @bdevtype: backing store type to use.  If NULL, dir will be used.
 * @specs: additional parameters for the backing store, i.e. LVM vg to
 *         use.
 *
 * @argv: the arguments to pass to the template, terminated by NULL.  If no
 * arguments, you can just pass NULL.
 */
static bool lxcapi_create(struct lxc_container *c, const char *t, const char *bdevtype, struct bdev_specs *specs,
                          int flags, char *const argv[])
{
    bool ret = false;
    pid_t pid;
    char *tpath = NULL;
    int partial_fd;

    if (!c)
        return false;

    if (t) {
        tpath = get_template_path(t);
        if (!tpath) {
            ERROR("bad template: %s", t);
            goto out;
        }
    }

    /*
	 * If a template is passed in, and the rootfs already is defined in
	 * the container config and exists, then * caller is trying to create
	 * an existing container.  Return an error, but do NOT delete the
	 * container.
	 */
    if (lxcapi_is_defined(c) && c->lxc_conf && c->lxc_conf->rootfs.path &&
        access(c->lxc_conf->rootfs.path, F_OK) == 0 && tpath) {
        ERROR("Container %s:%s already exists", c->config_path, c->name);
        goto free_tpath;
    }

    if (!c->lxc_conf) {
        if (!c->load_config(c, lxc_global_config_value("lxc.default_config"))) {
            ERROR("Error loading default configuration file %s", lxc_global_config_value("lxc.default_config"));
            goto free_tpath;
        }
    }

    if (!create_container_dir(c))
        goto free_tpath;

    /*
	 * either template or rootfs.path should be set.
	 * if both template and rootfs.path are set, template is setup as rootfs.path.
	 * container is already created if we have a config and rootfs.path is accessible
	 */
    if (!c->lxc_conf->rootfs.path && !tpath)
        /* no template passed in and rootfs does not exist: error */
        goto out;
    if (c->lxc_conf->rootfs.path && access(c->lxc_conf->rootfs.path, F_OK) != 0)
        /* rootfs passed into configuration, but does not exist: error */
        goto out;
    if (lxcapi_is_defined(c) && c->lxc_conf->rootfs.path && !tpath) {
        /* Rootfs already existed, user just wanted to save the
		 * loaded configuration */
        ret = true;
        goto out;
    }

    /* Mark that this container is being created */
    if ((partial_fd = create_partial(c)) < 0)
        goto out;

    /* no need to get disk lock bc we have the partial locked */

    /*
	 * Create the backing store
	 * Note we can't do this in the same task as we use to execute the
	 * template because of the way zfs works.
	 * After you 'zfs create', zfs mounts the fs only in the initial
	 * namespace.
	 */
    pid = fork();
    if (pid < 0) {
        SYSERROR("failed to fork task for container creation template");
        goto out_unlock;
    }

    if (pid == 0) { // child
        struct bdev *bdev = NULL;

        if (!(bdev = do_bdev_create(c, bdevtype, specs))) {
            ERROR("Error creating backing store type %s for %s", bdevtype ? bdevtype : "(none)", c->name);
            exit(1);
        }

        /* save config file again to store the new rootfs location */
        if (!c->save_config(c, NULL)) {
            ERROR("failed to save starting configuration for %s", c->name);
            // parent task won't see bdev in config so we delete it
            bdev->ops->umount(bdev);
            bdev->ops->destroy(bdev);
            exit(1);
        }
        exit(0);
    }
    if (wait_for_pid(pid) != 0)
        goto out_unlock;

    /* reload config to get the rootfs */
    lxc_conf_free(c->lxc_conf);
    c->lxc_conf = NULL;
    if (!load_config_locked(c, c->configfile))
        goto out_unlock;

    if (!create_run_template(c, tpath, !!(flags & LXC_CREATE_QUIET), argv))
        goto out_unlock;

    // now clear out the lxc_conf we have, reload from the created
    // container
    lxcapi_clear_config(c);

    if (t) {
        if (!prepend_lxc_header(c->configfile, tpath, argv)) {
            ERROR("Error prepending header to configuration file");
            goto out_unlock;
        }
    }
    ret = load_config_locked(c, c->configfile);

out_unlock:
    if (partial_fd >= 0)
        remove_partial(c, partial_fd);
out:
    if (!ret)
        container_destroy(c);
free_tpath:
    free(tpath);
    return ret;
}

static bool lxcapi_reboot(struct lxc_container *c)
{
    pid_t pid;

    if (!c)
        return false;
    if (!c->is_running(c))
        return false;
    pid = c->init_pid(c);
    if (pid <= 0)
        return false;
    if (kill(pid, SIGINT) < 0)
        return false;
    return true;
}

static bool lxcapi_shutdown(struct lxc_container *c, int timeout)
{
    bool retv;
    pid_t pid;
    int haltsignal = SIGPWR;

    if (!c)
        return false;

    if (!c->is_running(c))
        return true;
    pid = c->init_pid(c);
    if (pid <= 0)
        return true;
    if (c->lxc_conf && c->lxc_conf->haltsignal)
        haltsignal = c->lxc_conf->haltsignal;
    kill(pid, haltsignal);
    retv = c->wait(c, "STOPPED", timeout);
    return retv;
}

static bool lxcapi_createl(struct lxc_container *c, const char *t, const char *bdevtype, struct bdev_specs *specs,
                           int flags, ...)
{
    bool bret = false;
    char **args = NULL;
    va_list ap;

    if (!c)
        return false;

    /*
	 * since we're going to wait for create to finish, I don't think we
	 * need to get a copy of the arguments.
	 */
    va_start(ap, flags);
    args = lxc_va_arg_list_to_argv(ap, 0, 0);
    va_end(ap);
    if (!args) {
        ERROR("Memory allocation error.");
        goto out;
    }

    bret = c->create(c, t, bdevtype, specs, flags, args);

out:
    free(args);
    return bret;
}

static void do_clear_unexp_config_line(struct lxc_conf *conf, const char *key)
{
    if (strcmp(key, "lxc.cgroup") == 0)
        clear_unexp_config_line(conf, key, true);
    else if (strcmp(key, "lxc.network") == 0)
        clear_unexp_config_line(conf, key, true);
    else if (strcmp(key, "lxc.hook") == 0)
        clear_unexp_config_line(conf, key, true);
    else
        clear_unexp_config_line(conf, key, false);
    if (!do_append_unexp_config_line(conf, key, ""))
        WARN("Error clearing configuration for %s", key);
}

static bool lxcapi_clear_config_item(struct lxc_container *c, const char *key)
{
    int ret;

    if (!c || !c->lxc_conf)
        return false;
    if (container_mem_lock(c))
        return false;
    ret = lxc_clear_config_item(c->lxc_conf, key);
    if (!ret)
        do_clear_unexp_config_line(c->lxc_conf, key);
    container_mem_unlock(c);
    return ret == 0;
}

static inline bool enter_net_ns(struct lxc_container *c)
{
    pid_t pid = c->init_pid(c);

    if ((geteuid() != 0 || (c->lxc_conf && !lxc_list_empty(&c->lxc_conf->id_map))) &&
        access("/proc/self/ns/user", F_OK) == 0) {
        if (!switch_to_ns(pid, "user"))
            return false;
    }
    return switch_to_ns(pid, "net");
}

// used by qsort and bsearch functions for comparing names
static inline int string_cmp(char **first, char **second)
{
    return strcmp(*first, *second);
}

// used by qsort and bsearch functions for comparing container names
static inline int container_cmp(struct lxc_container **first, struct lxc_container **second)
{
    return strcmp((*first)->name, (*second)->name);
}

static bool add_to_array(char ***names, char *cname, int pos)
{
    char **newnames = realloc(*names, (pos + 1) * sizeof(char *));
    if (!newnames) {
        ERROR("Out of memory");
        return false;
    }

    *names = newnames;
    newnames[pos] = strdup(cname);
    if (!newnames[pos])
        return false;

    // sort the arrray as we will use binary search on it
    qsort(newnames, pos + 1, sizeof(char *), (int (*)(const void *, const void *))string_cmp);

    return true;
}

static bool add_to_clist(struct lxc_container ***list, struct lxc_container *c, int pos, bool sort)
{
    struct lxc_container **newlist = realloc(*list, (pos + 1) * sizeof(struct lxc_container *));
    if (!newlist) {
        ERROR("Out of memory");
        return false;
    }

    *list = newlist;
    newlist[pos] = c;

    // sort the arrray as we will use binary search on it
    if (sort)
        qsort(newlist, pos + 1, sizeof(struct lxc_container *), (int (*)(const void *, const void *))container_cmp);

    return true;
}

static char **get_from_array(char ***names, char *cname, int size)
{
    return (char **)bsearch(&cname, *names, size, sizeof(char *), (int (*)(const void *, const void *))string_cmp);
}

static bool array_contains(char ***names, char *cname, int size)
{
    if (get_from_array(names, cname, size) != NULL)
        return true;
    return false;
}

static bool remove_from_array(char ***names, char *cname, int size)
{
    char **result = get_from_array(names, cname, size);
    if (result != NULL) {
        free(result);
        return true;
    }
    return false;
}

static char **lxcapi_get_interfaces(struct lxc_container *c)
{
    pid_t pid;
    int i, count = 0, pipefd[2];
    char **interfaces = NULL;
    char interface[IFNAMSIZ];

    if (pipe(pipefd) < 0) {
        SYSERROR("pipe failed");
        return NULL;
    }

    pid = fork();
    if (pid < 0) {
        SYSERROR("failed to fork task to get interfaces information");
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) { // child
        int ret = 1, nbytes;
        struct ifaddrs *interfaceArray = NULL, *tempIfAddr = NULL;

        /* close the read-end of the pipe */
        close(pipefd[0]);

        if (!enter_net_ns(c)) {
            SYSERROR("failed to enter namespace");
            goto out;
        }

        /* Grab the list of interfaces */
        if (getifaddrs(&interfaceArray)) {
            SYSERROR("failed to get interfaces list");
            goto out;
        }

        /* Iterate through the interfaces */
        for (tempIfAddr = interfaceArray; tempIfAddr != NULL; tempIfAddr = tempIfAddr->ifa_next) {
            nbytes = write(pipefd[1], tempIfAddr->ifa_name, IFNAMSIZ);
            if (nbytes < 0) {
                ERROR("write failed");
                goto out;
            }
            count++;
        }
        ret = 0;

    out:
        if (interfaceArray)
            freeifaddrs(interfaceArray);

        /* close the write-end of the pipe, thus sending EOF to the reader */
        close(pipefd[1]);
        exit(ret);
    }

    /* close the write-end of the pipe */
    close(pipefd[1]);

    while (read(pipefd[0], &interface, IFNAMSIZ) == IFNAMSIZ) {
        if (array_contains(&interfaces, interface, count))
            continue;

        if (!add_to_array(&interfaces, interface, count))
            ERROR("PARENT: add_to_array failed");
        count++;
    }

    if (wait_for_pid(pid) != 0) {
        for (i = 0; i < count; i++)
            free(interfaces[i]);
        free(interfaces);
        interfaces = NULL;
    }

    /* close the read-end of the pipe */
    close(pipefd[0]);

    /* Append NULL to the array */
    if (interfaces)
        interfaces = (char **)lxc_append_null_to_array((void **)interfaces, count);

    return interfaces;
}

static char **lxcapi_get_ips(struct lxc_container *c, const char *interface, const char *family, int scope)
{
    pid_t pid;
    int i, count = 0, pipefd[2];
    char **addresses = NULL;
    char address[INET6_ADDRSTRLEN];

    if (pipe(pipefd) < 0) {
        SYSERROR("pipe failed");
        return NULL;
    }

    pid = fork();
    if (pid < 0) {
        SYSERROR("failed to fork task to get container ips");
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) { // child
        int ret = 1, nbytes;
        struct ifaddrs *interfaceArray = NULL, *tempIfAddr = NULL;
        char addressOutputBuffer[INET6_ADDRSTRLEN];
        void *tempAddrPtr = NULL;
        char *address = NULL;

        /* close the read-end of the pipe */
        close(pipefd[0]);

        if (!enter_net_ns(c)) {
            SYSERROR("failed to enter namespace");
            goto out;
        }

        /* Grab the list of interfaces */
        if (getifaddrs(&interfaceArray)) {
            SYSERROR("failed to get interfaces list");
            goto out;
        }

        /* Iterate through the interfaces */
        for (tempIfAddr = interfaceArray; tempIfAddr != NULL; tempIfAddr = tempIfAddr->ifa_next) {
            if (tempIfAddr->ifa_addr == NULL)
                continue;

            if (tempIfAddr->ifa_addr->sa_family == AF_INET) {
                if (family && strcmp(family, "inet"))
                    continue;
                tempAddrPtr = &((struct sockaddr_in *)tempIfAddr->ifa_addr)->sin_addr;
            } else {
                if (family && strcmp(family, "inet6"))
                    continue;

                if (((struct sockaddr_in6 *)tempIfAddr->ifa_addr)->sin6_scope_id != scope)
                    continue;

                tempAddrPtr = &((struct sockaddr_in6 *)tempIfAddr->ifa_addr)->sin6_addr;
            }

            if (interface && strcmp(interface, tempIfAddr->ifa_name))
                continue;
            else if (!interface && strcmp("lo", tempIfAddr->ifa_name) == 0)
                continue;

            address = (char *)inet_ntop(tempIfAddr->ifa_addr->sa_family, tempAddrPtr, addressOutputBuffer,
                                        sizeof(addressOutputBuffer));
            if (!address)
                continue;

            nbytes = write(pipefd[1], address, INET6_ADDRSTRLEN);
            if (nbytes < 0) {
                ERROR("write failed");
                goto out;
            }
            count++;
        }
        ret = 0;

    out:
        if (interfaceArray)
            freeifaddrs(interfaceArray);

        /* close the write-end of the pipe, thus sending EOF to the reader */
        close(pipefd[1]);
        exit(ret);
    }

    /* close the write-end of the pipe */
    close(pipefd[1]);

    while (read(pipefd[0], &address, INET6_ADDRSTRLEN) == INET6_ADDRSTRLEN) {
        if (!add_to_array(&addresses, address, count))
            ERROR("PARENT: add_to_array failed");
        count++;
    }

    if (wait_for_pid(pid) != 0) {
        for (i = 0; i < count; i++)
            free(addresses[i]);
        free(addresses);
        addresses = NULL;
    }

    /* close the read-end of the pipe */
    close(pipefd[0]);

    /* Append NULL to the array */
    if (addresses)
        addresses = (char **)lxc_append_null_to_array((void **)addresses, count);

    return addresses;
}

static int lxcapi_get_config_item(struct lxc_container *c, const char *key, char *retv, int inlen)
{
    int ret;

    if (!c || !c->lxc_conf)
        return -1;
    if (container_mem_lock(c))
        return -1;
    ret = lxc_get_config_item(c->lxc_conf, key, retv, inlen);
    container_mem_unlock(c);
    return ret;
}

static char *lxcapi_get_running_config_item(struct lxc_container *c, const char *key)
{
    char *ret;

    if (!c || !c->lxc_conf)
        return NULL;
    if (container_mem_lock(c))
        return NULL;
    ret = lxc_cmd_get_config_item(c->name, key, c->get_config_path(c));
    container_mem_unlock(c);
    return ret;
}

static int lxcapi_get_keys(struct lxc_container *c, const char *key, char *retv, int inlen)
{
    if (!key)
        return lxc_listconfigs(retv, inlen);
    /*
	 * Support 'lxc.network.<idx>', i.e. 'lxc.network.0'
	 * This is an intelligent result to show which keys are valid given
	 * the type of nic it is
	 */
    if (!c || !c->lxc_conf)
        return -1;
    if (container_mem_lock(c))
        return -1;
    int ret = -1;
    if (strncmp(key, "lxc.network.", 12) == 0)
        ret = lxc_list_nicconfigs(c->lxc_conf, key, retv, inlen);
    container_mem_unlock(c);
    return ret;
}

static bool lxcapi_save_config(struct lxc_container *c, const char *alt_file)
{
    FILE *fout;
    bool ret = false, need_disklock = false;
    int lret;

    if (!alt_file)
        alt_file = c->configfile;
    if (!alt_file)
        return false; // should we write to stdout if no file is specified?

    // If we haven't yet loaded a config, load the stock config
    if (!c->lxc_conf) {
        if (!c->load_config(c, lxc_global_config_value("lxc.default_config"))) {
            ERROR("Error loading default configuration file %s while saving %s",
                  lxc_global_config_value("lxc.default_config"), c->name);
            return false;
        }
    }

    if (!create_container_dir(c))
        return false;

    /*
	 * If we're writing to the container's config file, take the
	 * disk lock.  Otherwise just take the memlock to protect the
	 * struct lxc_container while we're traversing it.
	 */
    if (strcmp(c->configfile, alt_file) == 0)
        need_disklock = true;

    if (need_disklock)
        lret = container_disk_lock(c);
    else
        lret = container_mem_lock(c);

    if (lret)
        return false;

    fout = fopen(alt_file, "w");
    if (!fout)
        goto out;
    write_config(fout, c->lxc_conf);
    fclose(fout);
    ret = true;

out:
    if (need_disklock)
        container_disk_unlock(c);
    else
        container_mem_unlock(c);
    return ret;
}

static bool mod_rdep(struct lxc_container *c, bool inc)
{
    char path[MAXPATHLEN];
    int ret, v = 0;
    FILE *f;
    bool bret = false;

    if (container_disk_lock(c))
        return false;
    ret = snprintf(path, MAXPATHLEN, "%s/%s/lxc_snapshots", c->config_path, c->name);
    if (ret < 0 || ret > MAXPATHLEN)
        goto out;
    f = fopen(path, "r");
    if (f) {
        ret = fscanf(f, "%d", &v);
        fclose(f);
        if (ret != 1) {
            ERROR("Corrupted file %s", path);
            goto out;
        }
    }
    v += inc ? 1 : -1;
    f = fopen(path, "w");
    if (!f)
        goto out;
    if (fprintf(f, "%d\n", v) < 0) {
        ERROR("Error writing new snapshots value");
        fclose(f);
        goto out;
    }
    ret = fclose(f);
    if (ret != 0) {
        SYSERROR("Error writing to or closing snapshots file");
        goto out;
    }

    bret = true;

out:
    container_disk_unlock(c);
    return bret;
}

static void strip_newline(char *p)
{
    size_t len = strlen(p);
    if (len < 1)
        return;
    if (p[len - 1] == '\n')
        p[len - 1] = '\0';
}

static void mod_all_rdeps(struct lxc_container *c, bool inc)
{
    struct lxc_container *p;
    char *lxcpath = NULL, *lxcname = NULL, path[MAXPATHLEN];
    size_t pathlen = 0, namelen = 0;
    FILE *f;
    int ret;

    ret = snprintf(path, MAXPATHLEN, "%s/%s/lxc_rdepends", c->config_path, c->name);
    if (ret < 0 || ret >= MAXPATHLEN) {
        ERROR("Path name too long");
        return;
    }
    f = fopen(path, "r");
    if (f == NULL)
        return;
    while (getline(&lxcpath, &pathlen, f) != -1) {
        if (getline(&lxcname, &namelen, f) == -1) {
            ERROR("badly formatted file %s", path);
            goto out;
        }
        strip_newline(lxcpath);
        strip_newline(lxcname);
        if ((p = lxc_container_new(lxcname, lxcpath)) == NULL) {
            ERROR("Unable to find dependent container %s:%s", lxcpath, lxcname);
            continue;
        }
        if (!mod_rdep(p, inc))
            ERROR("Failed to increase numsnapshots for %s:%s", lxcpath, lxcname);
        lxc_container_put(p);
    }
out:
    free(lxcpath);
    free(lxcname);
    fclose(f);
}

static bool has_fs_snapshots(struct lxc_container *c)
{
    char path[MAXPATHLEN];
    int ret, v;
    FILE *f;
    bool bret = false;

    ret = snprintf(path, MAXPATHLEN, "%s/%s/lxc_snapshots", c->config_path, c->name);
    if (ret < 0 || ret > MAXPATHLEN)
        goto out;
    f = fopen(path, "r");
    if (!f)
        goto out;
    ret = fscanf(f, "%d", &v);
    fclose(f);
    if (ret != 1)
        goto out;
    bret = v != 0;

out:
    return bret;
}

static bool has_snapshots(struct lxc_container *c)
{
    char path[MAXPATHLEN];
    struct dirent dirent, *direntp;
    int count = 0;
    DIR *dir;

    if (!get_snappath_dir(c, path))
        return false;
    dir = opendir(path);
    if (!dir)
        return false;
    while (!readdir_r(dir, &dirent, &direntp)) {
        if (!direntp)
            break;

        if (!strcmp(direntp->d_name, "."))
            continue;

        if (!strcmp(direntp->d_name, ".."))
            continue;
        count++;
        break;
    }
    closedir(dir);
    return count > 0;
}

static int lxc_rmdir_onedev_wrapper(void *data)
{
    char *arg = (char *)data;
    return lxc_rmdir_onedev(arg, "snaps");
}

static int do_bdev_destroy(struct lxc_conf *conf)
{
    struct bdev *r;
    int ret = 0;

    r = bdev_init(conf, conf->rootfs.path, conf->rootfs.mount, NULL);
    if (!r)
        return -1;

    if (r->ops->destroy(r) < 0)
        ret = -1;
    bdev_put(r);
    return ret;
}

static int bdev_destroy_wrapper(void *data)
{
    struct lxc_conf *conf = data;

    if (setgid(0) < 0) {
        ERROR("Failed to setgid to 0");
        return -1;
    }
    if (setgroups(0, NULL) < 0)
        WARN("Failed to clear groups");
    if (setuid(0) < 0) {
        ERROR("Failed to setuid to 0");
        return -1;
    }
    return do_bdev_destroy(conf);
}

static bool container_destroy(struct lxc_container *c)
{
    bool bret = false;
    int ret;

    if (!c || !lxcapi_is_defined(c))
        return false;

    if (container_disk_lock(c))
        return false;

    if (!is_stopped(c)) {
        // we should queue some sort of error - in c->error_string?
        ERROR("container %s is not stopped", c->name);
        goto out;
    }

    if (c->lxc_conf && c->lxc_conf->rootfs.path && c->lxc_conf->rootfs.mount) {
        if (am_unpriv())
            ret = userns_exec_1(c->lxc_conf, bdev_destroy_wrapper, c->lxc_conf);
        else
            ret = do_bdev_destroy(c->lxc_conf);
        if (ret < 0) {
            ERROR("Error destroying rootfs for %s", c->name);
            goto out;
        }
    }

    mod_all_rdeps(c, false);

    const char *p1 = lxcapi_get_config_path(c);
    char *path = alloca(strlen(p1) + strlen(c->name) + 2);
    sprintf(path, "%s/%s", p1, c->name);
    if (am_unpriv())
        ret = userns_exec_1(c->lxc_conf, lxc_rmdir_onedev_wrapper, path);
    else
        ret = lxc_rmdir_onedev(path, "snaps");
    if (ret < 0) {
        ERROR("Error destroying container directory for %s", c->name);
        goto out;
    }
    bret = true;

out:
    container_disk_unlock(c);
    return bret;
}

static bool lxcapi_destroy(struct lxc_container *c)
{
    if (!c || !lxcapi_is_defined(c))
        return false;
    if (has_snapshots(c)) {
        ERROR("Container %s has snapshots;  not removing", c->name);
        return false;
    }

    if (has_fs_snapshots(c)) {
        ERROR("container %s has snapshots on its rootfs", c->name);
        return false;
    }

    return container_destroy(c);
}

static bool lxcapi_snapshot_destroy_all(struct lxc_container *c);

static bool lxcapi_destroy_with_snapshots(struct lxc_container *c)
{
    if (!c || !lxcapi_is_defined(c))
        return false;
    if (!lxcapi_snapshot_destroy_all(c)) {
        ERROR("Error deleting all snapshots");
        return false;
    }
    return lxcapi_destroy(c);
}

static bool set_config_item_locked(struct lxc_container *c, const char *key, const char *v)
{
    struct lxc_config_t *config;

    if (!c->lxc_conf)
        c->lxc_conf = lxc_conf_init();
    if (!c->lxc_conf)
        return false;
    config = lxc_getconfig(key);
    if (!config)
        return false;
    if (config->cb(key, v, c->lxc_conf) != 0)
        return false;
    return do_append_unexp_config_line(c->lxc_conf, key, v);
}

static bool lxcapi_set_config_item(struct lxc_container *c, const char *key, const char *v)
{
    bool b = false;

    if (!c)
        return false;

    if (container_mem_lock(c))
        return false;

    b = set_config_item_locked(c, key, v);

    container_mem_unlock(c);
    return b;
}

static char *lxcapi_config_file_name(struct lxc_container *c)
{
    if (!c || !c->configfile)
        return NULL;
    return strdup(c->configfile);
}

static const char *lxcapi_get_config_path(struct lxc_container *c)
{
    if (!c || !c->config_path)
        return NULL;
    return (const char *)(c->config_path);
}

/*
 * not for export
 * Just recalculate the c->configfile based on the
 * c->config_path, which must be set.
 * The lxc_container must be locked or not yet public.
 */
static bool set_config_filename(struct lxc_container *c)
{
    char *newpath;
    int len, ret;

    if (!c->config_path)
        return false;

    /* $lxc_path + "/" + c->name + "/" + "config" + '\0' */
    len = strlen(c->config_path) + strlen(c->name) + strlen("config") + 3;
    newpath = malloc(len);
    if (!newpath)
        return false;

    ret = snprintf(newpath, len, "%s/%s/config", c->config_path, c->name);
    if (ret < 0 || ret >= len) {
        fprintf(stderr, "Error printing out config file name\n");
        free(newpath);
        return false;
    }

    free(c->configfile);
    c->configfile = newpath;

    return true;
}

static bool lxcapi_set_config_path(struct lxc_container *c, const char *path)
{
    char *p;
    bool b = false;
    char *oldpath = NULL;

    if (!c)
        return b;

    if (container_mem_lock(c))
        return b;

    p = strdup(path);
    if (!p) {
        ERROR("Out of memory setting new lxc path");
        goto err;
    }

    b = true;
    if (c->config_path)
        oldpath = c->config_path;
    c->config_path = p;

    /* Since we've changed the config path, we have to change the
	 * config file name too */
    if (!set_config_filename(c)) {
        ERROR("Out of memory setting new config filename");
        b = false;
        free(c->config_path);
        c->config_path = oldpath;
        oldpath = NULL;
    }
err:
    free(oldpath);
    container_mem_unlock(c);
    return b;
}

static bool lxcapi_set_cgroup_item(struct lxc_container *c, const char *subsys, const char *value)
{
    int ret;

    if (!c)
        return false;

    if (is_stopped(c))
        return false;

    if (container_disk_lock(c))
        return false;

    ret = lxc_cgroup_set(subsys, value, c->name, c->config_path);

    container_disk_unlock(c);
    return ret == 0;
}

static int lxcapi_get_cgroup_item(struct lxc_container *c, const char *subsys, char *retv, int inlen)
{
    int ret;

    if (!c)
        return -1;

    if (is_stopped(c))
        return -1;

    if (container_disk_lock(c))
        return -1;

    ret = lxc_cgroup_get(subsys, retv, inlen, c->name, c->config_path);

    container_disk_unlock(c);
    return ret;
}

const char *lxc_get_global_config_item(const char *key)
{
    return lxc_global_config_value(key);
}

const char *lxc_get_version(void)
{
    return LXC_VERSION;
}

static int copy_file(const char *old, const char *new)
{
    int in, out;
    ssize_t len, ret;
    char buf[8096];
    struct stat sbuf;

    if (file_exists(new)) {
        ERROR("copy destination %s exists", new);
        return -1;
    }
    ret = stat(old, &sbuf);
    if (ret < 0) {
        INFO("Error stat'ing %s", old);
        return -1;
    }

    in = open(old, O_RDONLY);
    if (in < 0) {
        SYSERROR("Error opening original file %s", old);
        return -1;
    }
    out = open(new, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (out < 0) {
        SYSERROR("Error opening new file %s", new);
        close(in);
        return -1;
    }

    while (1) {
        len = read(in, buf, 8096);
        if (len < 0) {
            SYSERROR("Error reading old file %s", old);
            goto err;
        }
        if (len == 0)
            break;
        ret = write(out, buf, len);
        if (ret < len) { // should we retry?
            SYSERROR("Error: write to new file %s was interrupted", new);
            goto err;
        }
    }
    close(in);
    close(out);

    // we set mode, but not owner/group
    ret = chmod(new, sbuf.st_mode);
    if (ret) {
        SYSERROR("Error setting mode on %s", new);
        return -1;
    }

    return 0;

err:
    close(in);
    close(out);
    return -1;
}

static int copyhooks(struct lxc_container *oldc, struct lxc_container *c)
{
    int i, len, ret;
    struct lxc_list *it;
    char *cpath;

    len = strlen(oldc->config_path) + strlen(oldc->name) + 3;
    cpath = alloca(len);
    ret = snprintf(cpath, len, "%s/%s/", oldc->config_path, oldc->name);
    if (ret < 0 || ret >= len)
        return -1;

    for (i = 0; i < NUM_LXC_HOOKS; i++) {
        lxc_list_for_each(it, &c->lxc_conf->hooks[i])
        {
            char *hookname = it->elem;
            char *fname = strrchr(hookname, '/');
            char tmppath[MAXPATHLEN];
            if (!fname) // relative path - we don't support, but maybe we should
                return 0;
            if (strncmp(hookname, cpath, len - 1) != 0) {
                // this hook is public - ignore
                continue;
            }
            // copy the script, and change the entry in confile
            ret = snprintf(tmppath, MAXPATHLEN, "%s/%s/%s", c->config_path, c->name, fname + 1);
            if (ret < 0 || ret >= MAXPATHLEN)
                return -1;
            ret = copy_file(it->elem, tmppath);
            if (ret < 0)
                return -1;
            free(it->elem);
            it->elem = strdup(tmppath);
            if (!it->elem) {
                ERROR("out of memory copying hook path");
                return -1;
            }
        }
    }

    if (!clone_update_unexp_hooks(c->lxc_conf, oldc->config_path, c->config_path, oldc->name, c->name)) {
        ERROR("Error saving new hooks in clone");
        return -1;
    }
    c->save_config(c, NULL);
    return 0;
}

static int copy_fstab(struct lxc_container *oldc, struct lxc_container *c)
{
    char newpath[MAXPATHLEN];
    char *oldpath = oldc->lxc_conf->fstab;
    int ret;

    if (!oldpath)
        return 0;

    clear_unexp_config_line(c->lxc_conf, "lxc.mount", false);

    char *p = strrchr(oldpath, '/');
    if (!p)
        return -1;
    ret = snprintf(newpath, MAXPATHLEN, "%s/%s%s", c->config_path, c->name, p);
    if (ret < 0 || ret >= MAXPATHLEN) {
        ERROR("error printing new path for %s", oldpath);
        return -1;
    }
    if (file_exists(newpath)) {
        ERROR("error: fstab file %s exists", newpath);
        return -1;
    }

    if (copy_file(oldpath, newpath) < 0) {
        ERROR("error: copying %s to %s", oldpath, newpath);
        return -1;
    }
    free(c->lxc_conf->fstab);
    c->lxc_conf->fstab = strdup(newpath);
    if (!c->lxc_conf->fstab) {
        ERROR("error: allocating pathname");
        return -1;
    }
    if (!do_append_unexp_config_line(c->lxc_conf, "lxc.mount", newpath)) {
        ERROR("error saving new lxctab");
        return -1;
    }

    return 0;
}

static void copy_rdepends(struct lxc_container *c, struct lxc_container *c0)
{
    char path0[MAXPATHLEN], path1[MAXPATHLEN];
    int ret;

    ret = snprintf(path0, MAXPATHLEN, "%s/%s/lxc_rdepends", c0->config_path, c0->name);
    if (ret < 0 || ret >= MAXPATHLEN) {
        WARN("Error copying reverse dependencies");
        return;
    }
    ret = snprintf(path1, MAXPATHLEN, "%s/%s/lxc_rdepends", c->config_path, c->name);
    if (ret < 0 || ret >= MAXPATHLEN) {
        WARN("Error copying reverse dependencies");
        return;
    }
    if (copy_file(path0, path1) < 0) {
        INFO("Error copying reverse dependencies");
        return;
    }
}

static bool add_rdepends(struct lxc_container *c, struct lxc_container *c0)
{
    int ret;
    char path[MAXPATHLEN];
    FILE *f;
    bool bret;

    ret = snprintf(path, MAXPATHLEN, "%s/%s/lxc_rdepends", c->config_path, c->name);
    if (ret < 0 || ret >= MAXPATHLEN)
        return false;
    f = fopen(path, "a");
    if (!f)
        return false;
    bret = true;
    // if anything goes wrong, just return an error
    if (fprintf(f, "%s\n%s\n", c0->config_path, c0->name) < 0)
        bret = false;
    if (fclose(f) != 0)
        bret = false;
    return bret;
}

static int copy_storage(struct lxc_container *c0, struct lxc_container *c, const char *newtype, int flags,
                        const char *bdevdata, uint64_t newsize)
{
    struct bdev *bdev;
    int need_rdep;

    bdev = bdev_copy(c0, c->name, c->config_path, newtype, flags, bdevdata, newsize, &need_rdep);
    if (!bdev) {
        ERROR("Error copying storage");
        return -1;
    }
    free(c->lxc_conf->rootfs.path);
    c->lxc_conf->rootfs.path = strdup(bdev->src);
    bdev_put(bdev);
    if (!c->lxc_conf->rootfs.path) {
        ERROR("Out of memory while setting storage path");
        return -1;
    }
    // We will simply append a new lxc.rootfs entry to the unexpanded config
    clear_unexp_config_line(c->lxc_conf, "lxc.rootfs", false);
    if (!do_append_unexp_config_line(c->lxc_conf, "lxc.rootfs", c->lxc_conf->rootfs.path)) {
        ERROR("Error saving new rootfs to cloend config");
        return -1;
    }
    if (flags & LXC_CLONE_SNAPSHOT)
        copy_rdepends(c, c0);
    if (need_rdep) {
        if (!add_rdepends(c, c0))
            WARN("Error adding reverse dependency from %s to %s", c->name, c0->name);
    }

    mod_all_rdeps(c, true);

    return 0;
}

struct clone_update_data {
    struct lxc_container *c0;
    struct lxc_container *c1;
    int flags;
    char **hookargs;
};

static int clone_update_rootfs(struct clone_update_data *data)
{
    struct lxc_container *c0 = data->c0;
    struct lxc_container *c = data->c1;
    int flags = data->flags;
    char **hookargs = data->hookargs;
    int ret = -1;
    char path[MAXPATHLEN];
    struct bdev *bdev;
    FILE *fout;
    struct lxc_conf *conf = c->lxc_conf;

    /* update hostname in rootfs */
    /* we're going to mount, so run in a clean namespace to simplify cleanup */

    if (setgid(0) < 0) {
        ERROR("Failed to setgid to 0");
        return -1;
    }
    if (setuid(0) < 0) {
        ERROR("Failed to setuid to 0");
        return -1;
    }
    if (setgroups(0, NULL) < 0)
        WARN("Failed to clear groups");

    if (unshare(CLONE_NEWNS) < 0)
        return -1;
    bdev = bdev_init(c->lxc_conf, c->lxc_conf->rootfs.path, c->lxc_conf->rootfs.mount, NULL);
    if (!bdev)
        return -1;
    if (strcmp(bdev->type, "dir") != 0) {
        if (unshare(CLONE_NEWNS) < 0) {
            ERROR("error unsharing mounts");
            bdev_put(bdev);
            return -1;
        }
        if (detect_shared_rootfs()) {
            if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL)) {
                SYSERROR("Failed to make / rslave");
                ERROR("Continuing...");
            }
        }
        if (bdev->ops->mount(bdev) < 0) {
            bdev_put(bdev);
            return -1;
        }
    } else { // TODO come up with a better way
        free(bdev->dest);
        bdev->dest = strdup(bdev->src);
    }

    if (!lxc_list_empty(&conf->hooks[LXCHOOK_CLONE])) {
        /* Start of environment variable setup for hooks */
        if (setenv("LXC_SRC_NAME", c0->name, 1)) {
            SYSERROR("failed to set environment variable for source container name");
        }
        if (setenv("LXC_NAME", c->name, 1)) {
            SYSERROR("failed to set environment variable for container name");
        }
        if (setenv("LXC_CONFIG_FILE", conf->rcfile, 1)) {
            SYSERROR("failed to set environment variable for config path");
        }
        if (setenv("LXC_ROOTFS_MOUNT", bdev->dest, 1)) {
            SYSERROR("failed to set environment variable for rootfs mount");
        }
        if (setenv("LXC_ROOTFS_PATH", conf->rootfs.path, 1)) {
            SYSERROR("failed to set environment variable for rootfs mount");
        }

        if (run_lxc_hooks(c->name, "clone", conf, c->get_config_path(c), hookargs)) {
            ERROR("Error executing clone hook for %s", c->name);
            bdev_put(bdev);
            return -1;
        }
    }

    if (!(flags & LXC_CLONE_KEEPNAME)) {
        ret = snprintf(path, MAXPATHLEN, "%s/etc/hostname", bdev->dest);
        bdev_put(bdev);

        if (ret < 0 || ret >= MAXPATHLEN)
            return -1;
        if (!file_exists(path))
            return 0;
        if (!(fout = fopen(path, "w"))) {
            SYSERROR("unable to open %s: ignoring", path);
            return 0;
        }
        if (fprintf(fout, "%s", c->name) < 0) {
            fclose(fout);
            return -1;
        }
        if (fclose(fout) < 0)
            return -1;
    } else
        bdev_put(bdev);

    return 0;
}

static int clone_update_rootfs_wrapper(void *data)
{
    struct clone_update_data *arg = (struct clone_update_data *)data;
    return clone_update_rootfs(arg);
}

/*
 * We want to support:
sudo lxc-clone -o o1 -n n1 -s -L|-fssize fssize -v|--vgname vgname \
        -p|--lvprefix lvprefix -t|--fstype fstype  -B backingstore

-s [ implies overlayfs]
-s -B overlayfs
-s -B aufs

only rootfs gets converted (copied/snapshotted) on clone.
*/

static int create_file_dirname(char *path, struct lxc_conf *conf)
{
    char *p = strrchr(path, '/');
    int ret = -1;

    if (!p)
        return -1;
    *p = '\0';
    ret = do_create_container_dir(path, conf);
    *p = '/';
    return ret;
}

static struct lxc_container *lxcapi_clone(struct lxc_container *c, const char *newname, const char *lxcpath, int flags,
                                          const char *bdevtype, const char *bdevdata, uint64_t newsize, char **hookargs)
{
    struct lxc_container *c2 = NULL;
    char newpath[MAXPATHLEN];
    int ret, storage_copied = 0;
    char *origroot = NULL;
    struct clone_update_data data;
    FILE *fout;
    pid_t pid;

    if (!c || !c->is_defined(c))
        return NULL;

    if (container_mem_lock(c))
        return NULL;

    if (!is_stopped(c)) {
        ERROR("error: Original container (%s) is running", c->name);
        goto out;
    }

    // Make sure the container doesn't yet exist.
    if (!newname)
        newname = c->name;
    if (!lxcpath)
        lxcpath = c->get_config_path(c);
    ret = snprintf(newpath, MAXPATHLEN, "%s/%s/config", lxcpath, newname);
    if (ret < 0 || ret >= MAXPATHLEN) {
        SYSERROR("clone: failed making config pathname");
        goto out;
    }
    if (file_exists(newpath)) {
        ERROR("error: clone: %s exists", newpath);
        goto out;
    }

    ret = create_file_dirname(newpath, c->lxc_conf);
    if (ret < 0 && errno != EEXIST) {
        ERROR("Error creating container dir for %s", newpath);
        goto out;
    }

    // copy the configuration, tweak it as needed,
    if (c->lxc_conf->rootfs.path) {
        origroot = c->lxc_conf->rootfs.path;
        c->lxc_conf->rootfs.path = NULL;
    }
    fout = fopen(newpath, "w");
    if (!fout) {
        SYSERROR("open %s", newpath);
        goto out;
    }
    write_config(fout, c->lxc_conf);
    fclose(fout);
    c->lxc_conf->rootfs.path = origroot;

    sprintf(newpath, "%s/%s/rootfs", lxcpath, newname);
    if (mkdir(newpath, 0755) < 0) {
        SYSERROR("error creating %s", newpath);
        goto out;
    }

    if (am_unpriv()) {
        if (chown_mapped_root(newpath, c->lxc_conf) < 0) {
            ERROR("Error chowning %s to container root", newpath);
            goto out;
        }
    }

    c2 = lxc_container_new(newname, lxcpath);
    if (!c2) {
        ERROR("clone: failed to create new container (%s %s)", newname, lxcpath);
        goto out;
    }

    // copy/snapshot rootfs's
    ret = copy_storage(c, c2, bdevtype, flags, bdevdata, newsize);
    if (ret < 0)
        goto out;

    clear_unexp_config_line(c2->lxc_conf, "lxc.utsname", false);

    // update utsname
    if (!set_config_item_locked(c2, "lxc.utsname", newname)) {
        ERROR("Error setting new hostname");
        goto out;
    }

    // copy hooks
    ret = copyhooks(c, c2);
    if (ret < 0) {
        ERROR("error copying hooks");
        goto out;
    }

    if (copy_fstab(c, c2) < 0) {
        ERROR("error copying fstab");
        goto out;
    }

    // update macaddrs
    if (!(flags & LXC_CLONE_KEEPMACADDR)) {
        if (!network_new_hwaddrs(c2->lxc_conf)) {
            ERROR("Error updating mac addresses");
            goto out;
        }
    }

    // We've now successfully created c2's storage, so clear it out if we
    // fail after this
    storage_copied = 1;

    if (!c2->save_config(c2, NULL))
        goto out;

    if ((pid = fork()) < 0) {
        SYSERROR("fork");
        goto out;
    }
    if (pid > 0) {
        ret = wait_for_pid(pid);
        if (ret)
            goto out;
        container_mem_unlock(c);
        return c2;
    }
    data.c0 = c;
    data.c1 = c2;
    data.flags = flags;
    data.hookargs = hookargs;
    if (am_unpriv())
        ret = userns_exec_1(c->lxc_conf, clone_update_rootfs_wrapper, &data);
    else
        ret = clone_update_rootfs(&data);
    if (ret < 0)
        exit(1);

    container_mem_unlock(c);
    exit(0);

out:
    container_mem_unlock(c);
    if (c2) {
        if (!storage_copied)
            c2->lxc_conf->rootfs.path = NULL;
        c2->destroy(c2);
        lxc_container_put(c2);
    }

    return NULL;
}

static bool lxcapi_rename(struct lxc_container *c, const char *newname)
{
    struct bdev *bdev;
    struct lxc_container *newc;

    if (!c || !c->name || !c->config_path || !c->lxc_conf)
        return false;

    if (has_fs_snapshots(c) || has_snapshots(c)) {
        ERROR("Renaming a container with snapshots is not supported");
        return false;
    }
    bdev = bdev_init(c->lxc_conf, c->lxc_conf->rootfs.path, c->lxc_conf->rootfs.mount, NULL);
    if (!bdev) {
        ERROR("Failed to find original backing store type");
        return false;
    }

    newc = lxcapi_clone(c, newname, c->config_path, LXC_CLONE_KEEPMACADDR, NULL, bdev->type, 0, NULL);
    bdev_put(bdev);
    if (!newc) {
        lxc_container_put(newc);
        return false;
    }

    if (newc && lxcapi_is_defined(newc))
        lxc_container_put(newc);

    if (!container_destroy(c)) {
        ERROR("Could not destroy existing container %s", c->name);
        return false;
    }
    return true;
}

static int lxcapi_attach(struct lxc_container *c, lxc_attach_exec_t exec_function, void *exec_payload,
                         lxc_attach_options_t *options, pid_t *attached_process)
{
    if (!c)
        return -1;

    return lxc_attach(c->name, c->config_path, exec_function, exec_payload, options, attached_process);
}

static int lxcapi_attach_run_wait(struct lxc_container *c, lxc_attach_options_t *options, const char *program,
                                  const char *const argv[])
{
    lxc_attach_command_t command;
    pid_t pid;
    int r;

    if (!c)
        return -1;

    command.program = (char *)program;
    command.argv = (char **)argv;
    r = lxc_attach(c->name, c->config_path, lxc_attach_run_command, &command, options, &pid);
    if (r < 0) {
        ERROR("ups");
        return r;
    }
    return lxc_wait_for_pid_status(pid);
}

static int get_next_index(const char *lxcpath, char *cname)
{
    char *fname;
    struct stat sb;
    int i = 0, ret;

    fname = alloca(strlen(lxcpath) + 20);
    while (1) {
        sprintf(fname, "%s/snap%d", lxcpath, i);
        ret = stat(fname, &sb);
        if (ret != 0)
            return i;
        i++;
    }
}

static bool get_snappath_dir(struct lxc_container *c, char *snappath)
{
    int ret;
    /*
	 * If the old style snapshot path exists, use it
	 * /var/lib/lxc -> /var/lib/lxcsnaps
	 */
    ret = snprintf(snappath, MAXPATHLEN, "%ssnaps", c->config_path);
    if (ret < 0 || ret >= MAXPATHLEN)
        return false;
    if (dir_exists(snappath)) {
        ret = snprintf(snappath, MAXPATHLEN, "%ssnaps/%s", c->config_path, c->name);
        if (ret < 0 || ret >= MAXPATHLEN)
            return false;
        return true;
    }

    /*
	 * Use the new style path
	 * /var/lib/lxc -> /var/lib/lxc + c->name + /snaps + \0
	 */
    ret = snprintf(snappath, MAXPATHLEN, "%s/%s/snaps", c->config_path, c->name);
    if (ret < 0 || ret >= MAXPATHLEN)
        return false;
    return true;
}

static int lxcapi_snapshot(struct lxc_container *c, const char *commentfile)
{
    int i, flags, ret;
    struct lxc_container *c2;
    char snappath[MAXPATHLEN], newname[20];

    if (!c || !lxcapi_is_defined(c))
        return -1;

    if (!bdev_can_backup(c->lxc_conf)) {
        ERROR("%s's backing store cannot be backed up.", c->name);
        ERROR("Your container must use another backing store type.");
        return -1;
    }

    if (!get_snappath_dir(c, snappath))
        return -1;

    i = get_next_index(snappath, c->name);

    if (mkdir_p(snappath, 0755) < 0) {
        ERROR("Failed to create snapshot directory %s", snappath);
        return -1;
    }

    ret = snprintf(newname, 20, "snap%d", i);
    if (ret < 0 || ret >= 20)
        return -1;

    /*
	 * We pass LXC_CLONE_SNAPSHOT to make sure that a rdepends file entry is
	 * created in the original container
	 */
    flags = LXC_CLONE_SNAPSHOT | LXC_CLONE_KEEPMACADDR | LXC_CLONE_KEEPNAME | LXC_CLONE_KEEPBDEVTYPE |
            LXC_CLONE_MAYBE_SNAPSHOT;
    if (bdev_is_dir(c->lxc_conf, c->lxc_conf->rootfs.path)) {
        ERROR("Snapshot of directory-backed container requested.");
        ERROR("Making a copy-clone.  If you do want snapshots, then");
        ERROR("please create an aufs or overlayfs clone first, snapshot that");
        ERROR("and keep the original container pristine.");
        flags &= ~LXC_CLONE_SNAPSHOT | LXC_CLONE_MAYBE_SNAPSHOT;
    }
    c2 = c->clone(c, newname, snappath, flags, NULL, NULL, 0, NULL);
    if (!c2) {
        ERROR("clone of %s:%s failed", c->config_path, c->name);
        return -1;
    }

    lxc_container_put(c2);

    // Now write down the creation time
    time_t timer;
    char buffer[25];
    struct tm *tm_info;
    FILE *f;

    time(&timer);
    tm_info = localtime(&timer);

    strftime(buffer, 25, "%Y:%m:%d %H:%M:%S", tm_info);

    char *dfnam = alloca(strlen(snappath) + strlen(newname) + 5);
    sprintf(dfnam, "%s/%s/ts", snappath, newname);
    f = fopen(dfnam, "w");
    if (!f) {
        ERROR("Failed to open %s", dfnam);
        return -1;
    }
    if (fprintf(f, "%s", buffer) < 0) {
        SYSERROR("Writing timestamp");
        fclose(f);
        return -1;
    }
    ret = fclose(f);
    if (ret != 0) {
        SYSERROR("Writing timestamp");
        return -1;
    }

    if (commentfile) {
        // $p / $name / comment \0
        int len = strlen(snappath) + strlen(newname) + 10;
        char *path = alloca(len);
        sprintf(path, "%s/%s/comment", snappath, newname);
        return copy_file(commentfile, path) < 0 ? -1 : i;
    }

    return i;
}

static void lxcsnap_free(struct lxc_snapshot *s)
{
    free(s->name);
    free(s->comment_pathname);
    free(s->timestamp);
    free(s->lxcpath);
}

static char *get_snapcomment_path(char *snappath, char *name)
{
    // $snappath/$name/comment
    int ret, len = strlen(snappath) + strlen(name) + 10;
    char *s = malloc(len);

    if (s) {
        ret = snprintf(s, len, "%s/%s/comment", snappath, name);
        if (ret < 0 || ret >= len) {
            free(s);
            s = NULL;
        }
    }
    return s;
}

static char *get_timestamp(char *snappath, char *name)
{
    char path[MAXPATHLEN], *s = NULL;
    int ret, len;
    FILE *fin;

    ret = snprintf(path, MAXPATHLEN, "%s/%s/ts", snappath, name);
    if (ret < 0 || ret >= MAXPATHLEN)
        return NULL;
    fin = fopen(path, "r");
    if (!fin)
        return NULL;
    (void)fseek(fin, 0, SEEK_END);
    len = ftell(fin);
    (void)fseek(fin, 0, SEEK_SET);
    if (len > 0) {
        s = malloc(len + 1);
        if (s) {
            s[len] = '\0';
            if (fread(s, 1, len, fin) != len) {
                SYSERROR("reading timestamp");
                free(s);
                s = NULL;
            }
        }
    }
    fclose(fin);
    return s;
}

static int lxcapi_snapshot_list(struct lxc_container *c, struct lxc_snapshot **ret_snaps)
{
    char snappath[MAXPATHLEN], path2[MAXPATHLEN];
    int count = 0, ret;
    struct dirent dirent, *direntp;
    struct lxc_snapshot *snaps = NULL, *nsnaps;
    DIR *dir;

    if (!c || !lxcapi_is_defined(c))
        return -1;

    if (!get_snappath_dir(c, snappath)) {
        ERROR("path name too long");
        return -1;
    }
    dir = opendir(snappath);
    if (!dir) {
        INFO("failed to open %s - assuming no snapshots", snappath);
        return 0;
    }

    while (!readdir_r(dir, &dirent, &direntp)) {
        if (!direntp)
            break;

        if (!strcmp(direntp->d_name, "."))
            continue;

        if (!strcmp(direntp->d_name, ".."))
            continue;

        ret = snprintf(path2, MAXPATHLEN, "%s/%s/config", snappath, direntp->d_name);
        if (ret < 0 || ret >= MAXPATHLEN) {
            ERROR("pathname too long");
            goto out_free;
        }
        if (!file_exists(path2))
            continue;
        nsnaps = realloc(snaps, (count + 1) * sizeof(*snaps));
        if (!nsnaps) {
            SYSERROR("Out of memory");
            goto out_free;
        }
        snaps = nsnaps;
        snaps[count].free = lxcsnap_free;
        snaps[count].name = strdup(direntp->d_name);
        if (!snaps[count].name)
            goto out_free;
        snaps[count].lxcpath = strdup(snappath);
        if (!snaps[count].lxcpath) {
            free(snaps[count].name);
            goto out_free;
        }
        snaps[count].comment_pathname = get_snapcomment_path(snappath, direntp->d_name);
        snaps[count].timestamp = get_timestamp(snappath, direntp->d_name);
        count++;
    }

    if (closedir(dir))
        WARN("failed to close directory");

    *ret_snaps = snaps;
    return count;

out_free:
    if (snaps) {
        int i;
        for (i = 0; i < count; i++)
            lxcsnap_free(&snaps[i]);
        free(snaps);
    }
    if (closedir(dir))
        WARN("failed to close directory");
    return -1;
}

static bool lxcapi_snapshot_restore(struct lxc_container *c, const char *snapname, const char *newname)
{
    char clonelxcpath[MAXPATHLEN];
    int flags = 0;
    struct lxc_container *snap, *rest;
    struct bdev *bdev;
    bool b = false;

    if (!c || !c->name || !c->config_path)
        return false;

    if (has_fs_snapshots(c)) {
        ERROR("container rootfs has dependent snapshots");
        return false;
    }

    bdev = bdev_init(c->lxc_conf, c->lxc_conf->rootfs.path, c->lxc_conf->rootfs.mount, NULL);
    if (!bdev) {
        ERROR("Failed to find original backing store type");
        return false;
    }

    if (!newname)
        newname = c->name;

    if (!get_snappath_dir(c, clonelxcpath)) {
        bdev_put(bdev);
        return false;
    }
    // how should we lock this?

    snap = lxc_container_new(snapname, clonelxcpath);
    if (!snap || !lxcapi_is_defined(snap)) {
        ERROR("Could not open snapshot %s", snapname);
        if (snap)
            lxc_container_put(snap);
        bdev_put(bdev);
        return false;
    }

    if (strcmp(c->name, newname) == 0) {
        if (!container_destroy(c)) {
            ERROR("Could not destroy existing container %s", newname);
            lxc_container_put(snap);
            bdev_put(bdev);
            return false;
        }
    }

    if (strcmp(bdev->type, "dir") != 0 && strcmp(bdev->type, "loop") != 0)
        flags = LXC_CLONE_SNAPSHOT | LXC_CLONE_MAYBE_SNAPSHOT;
    rest = lxcapi_clone(snap, newname, c->config_path, flags, bdev->type, NULL, 0, NULL);
    bdev_put(bdev);
    if (rest && lxcapi_is_defined(rest))
        b = true;
    if (rest)
        lxc_container_put(rest);
    lxc_container_put(snap);
    return b;
}

static bool do_snapshot_destroy(const char *snapname, const char *clonelxcpath)
{
    struct lxc_container *snap = NULL;
    bool bret = false;

    snap = lxc_container_new(snapname, clonelxcpath);
    if (!snap) {
        ERROR("Could not find snapshot %s", snapname);
        goto err;
    }

    if (!lxcapi_destroy(snap)) {
        ERROR("Could not destroy snapshot %s", snapname);
        goto err;
    }
    bret = true;

err:
    if (snap)
        lxc_container_put(snap);
    return bret;
}

static bool remove_all_snapshots(const char *path)
{
    DIR *dir;
    struct dirent dirent, *direntp;
    bool bret = true;

    dir = opendir(path);
    if (!dir) {
        SYSERROR("opendir on snapshot path %s", path);
        return false;
    }
    while (!readdir_r(dir, &dirent, &direntp)) {
        if (!direntp)
            break;
        if (!strcmp(direntp->d_name, "."))
            continue;
        if (!strcmp(direntp->d_name, ".."))
            continue;
        if (!do_snapshot_destroy(direntp->d_name, path)) {
            bret = false;
            continue;
        }
    }

    closedir(dir);

    if (rmdir(path))
        SYSERROR("Error removing directory %s", path);

    return bret;
}

static bool lxcapi_snapshot_destroy(struct lxc_container *c, const char *snapname)
{
    char clonelxcpath[MAXPATHLEN];

    if (!c || !c->name || !c->config_path || !snapname)
        return false;

    if (!get_snappath_dir(c, clonelxcpath))
        return false;

    return do_snapshot_destroy(snapname, clonelxcpath);
}

static bool lxcapi_snapshot_destroy_all(struct lxc_container *c)
{
    char clonelxcpath[MAXPATHLEN];

    if (!c || !c->name || !c->config_path)
        return false;

    if (!get_snappath_dir(c, clonelxcpath))
        return false;

    return remove_all_snapshots(clonelxcpath);
}

static bool lxcapi_may_control(struct lxc_container *c)
{
    return lxc_try_cmd(c->name, c->config_path) == 0;
}

static bool do_add_remove_node(pid_t init_pid, const char *path, bool add, struct stat *st)
{
    char chrootpath[MAXPATHLEN];
    char *directory_path = NULL;
    pid_t pid;
    int ret;

    if ((pid = fork()) < 0) {
        SYSERROR("failed to fork a child helper");
        return false;
    }
    if (pid) {
        if (wait_for_pid(pid) != 0) {
            ERROR("Failed to create note in guest");
            return false;
        }
        return true;
    }

    /* prepare the path */
    ret = snprintf(chrootpath, MAXPATHLEN, "/proc/%d/root", init_pid);
    if (ret < 0 || ret >= MAXPATHLEN)
        return false;

    if (chroot(chrootpath) < 0)
        exit(1);
    if (chdir("/") < 0)
        exit(1);
    /* remove path if it exists */
    if (faccessat(AT_FDCWD, path, F_OK, AT_SYMLINK_NOFOLLOW) == 0) {
        if (unlink(path) < 0) {
            ERROR("unlink failed");
            exit(1);
        }
    }
    if (!add)
        exit(0);

    /* create any missing directories */
    directory_path = dirname(strdup(path));
    if (mkdir_p(directory_path, 0755) < 0 && errno != EEXIST) {
        ERROR("failed to create directory");
        exit(1);
    }

    /* create the device node */
    if (mknod(path, st->st_mode, st->st_rdev) < 0) {
        ERROR("mknod failed");
        exit(1);
    }

    exit(0);
}

static bool add_remove_device_node(struct lxc_container *c, const char *src_path, const char *dest_path, bool add)
{
    int ret;
    struct stat st;
    char value[MAX_BUFFER];
    const char *p;

    /* make sure container is running */
    if (!c->is_running(c)) {
        ERROR("container is not running");
        return false;
    }

    /* use src_path if dest_path is NULL otherwise use dest_path */
    p = dest_path ? dest_path : src_path;

    /* make sure we can access p */
    if (access(p, F_OK) < 0 || stat(p, &st) < 0)
        return false;

    /* continue if path is character device or block device */
    if (S_ISCHR(st.st_mode))
        ret = snprintf(value, MAX_BUFFER, "c %d:%d rwm", major(st.st_rdev), minor(st.st_rdev));
    else if (S_ISBLK(st.st_mode))
        ret = snprintf(value, MAX_BUFFER, "b %d:%d rwm", major(st.st_rdev), minor(st.st_rdev));
    else
        return false;

    /* check snprintf return code */
    if (ret < 0 || ret >= MAX_BUFFER)
        return false;

    if (!do_add_remove_node(c->init_pid(c), p, add, &st))
        return false;

    /* add or remove device to/from cgroup access list */
    if (add) {
        if (!c->set_cgroup_item(c, "devices.allow", value)) {
            ERROR("set_cgroup_item failed while adding the device node");
            return false;
        }
    } else {
        if (!c->set_cgroup_item(c, "devices.deny", value)) {
            ERROR("set_cgroup_item failed while removing the device node");
            return false;
        }
    }

    return true;
}

static bool lxcapi_add_device_node(struct lxc_container *c, const char *src_path, const char *dest_path)
{
    if (am_unpriv()) {
        ERROR(NOT_SUPPORTED_ERROR, __FUNCTION__);
        return false;
    }
    return add_remove_device_node(c, src_path, dest_path, true);
}

static bool lxcapi_remove_device_node(struct lxc_container *c, const char *src_path, const char *dest_path)
{
    if (am_unpriv()) {
        ERROR(NOT_SUPPORTED_ERROR, __FUNCTION__);
        return false;
    }
    return add_remove_device_node(c, src_path, dest_path, false);
}

static bool lxcapi_attach_interface(struct lxc_container *c, const char *ifname, const char *dst_ifname)
{
    int ret = 0;
    if (am_unpriv()) {
        ERROR(NOT_SUPPORTED_ERROR, __FUNCTION__);
        return false;
    }

    if (!ifname) {
        ERROR("No source interface name given");
        return false;
    }

    ret = lxc_netdev_isup(ifname);

    if (ret > 0) {
        /* netdev of ifname is up. */
        ret = lxc_netdev_down(ifname);
        if (ret)
            goto err;
    }

    ret = lxc_netdev_move_by_name(ifname, c->init_pid(c), dst_ifname);
    if (ret)
        goto err;

    return true;

err:
    return false;
}

static bool lxcapi_detach_interface(struct lxc_container *c, const char *ifname, const char *dst_ifname)
{
    pid_t pid, pid_outside;

    if (am_unpriv()) {
        ERROR(NOT_SUPPORTED_ERROR, __FUNCTION__);
        return false;
    }

    if (!ifname) {
        ERROR("No source interface name given");
        return false;
    }

    pid_outside = getpid();
    pid = fork();
    if (pid < 0) {
        ERROR("failed to fork task to get interfaces information");
        return false;
    }

    if (pid == 0) { // child
        int ret = 0;
        if (!enter_net_ns(c)) {
            ERROR("failed to enter namespace");
            exit(-1);
        }

        ret = lxc_netdev_isup(ifname);
        if (ret < 0)
            exit(ret);

        /* netdev of ifname is up. */
        if (ret) {
            ret = lxc_netdev_down(ifname);
            if (ret)
                exit(ret);
        }

        ret = lxc_netdev_move_by_name(ifname, pid_outside, dst_ifname);

        /* -EINVAL means there is no netdev named as ifanme. */
        if (ret == -EINVAL) {
            ERROR("No network device named as %s.", ifname);
        }
        exit(ret);
    }

    if (wait_for_pid(pid) != 0)
        return false;

    return true;
}

struct criu_opts {
    /* The type of criu invocation, one of "dump" or "restore" */
    char *action;

    /* The directory to pass to criu */
    char *directory;

    /* The container to dump */
    struct lxc_container *c;

    /* Enable criu verbose mode? */
    bool verbose;

    /* dump: stop the container or not after dumping? */
    bool stop;

    /* restore: the file to write the init process' pid into */
    char *pidfile;
    const char *cgroup_path;
};

/**
 * 执行 CRIU 命令。
 *
 * @param opts 包含 CRIU 命令选项的结构体。
 */
static void exec_criu(struct criu_opts *opts)
{
    char **argv, log[PATH_MAX], buf[257];
    int static_args = 14, argc = 0, i, ret;
    int netnr = 0;
    struct lxc_list *it;

    /* 构建 CRIU 命令行参数数组。
      * 命令行通常如下：
      * criu $(action) --tcp-established --file-locks --link-remap --force-irmap \
      * --manage-cgroups action-script foo.sh -D $(directory) \
      * -o $(directory)/$(action).log
      * +1 表示最后的 NULL */

    // 根据不同的操作（dump 或 restore），增加静态参数数量
    if (strcmp(opts->action, "dump") == 0) {
        /* -t pid 参数 */
        static_args += 2;

        /* --leave-running 参数（如果 stop 为 false） */
        if (!opts->stop)
            static_args++;
    } else if (strcmp(opts->action, "restore") == 0) {
        /* restore 操作的额外参数 */
        static_args += 8;
    } else {
        return; // 如果不是 dump 或 restore 操作，直接返回
    }

    // 如果 verbose 为 true，增加一个参数
    if (opts->verbose)
        static_args++;

    // 构建日志文件路径
    ret = snprintf(log, PATH_MAX, "%s/%s.log", opts->directory, opts->action);
    if (ret < 0 || ret >= PATH_MAX) {
        ERROR("logfile name too long\n");
        return;
    }

    // 分配 argv 数组
    argv = malloc(static_args * sizeof(*argv));
    if (!argv)
        return;

    // 将 argv 数组初始化为 NULL
    memset(argv, 0, static_args * sizeof(*argv));

    // 宏定义，用于添加参数到 argv 数组
#define DECLARE_ARG(arg)                         \
    do {                                         \
        if (arg == NULL) {                       \
            ERROR("Got NULL argument for criu"); \
            goto err;                            \
        }                                        \
        argv[argc++] = strdup(arg);              \
        if (!argv[argc - 1])                     \
            goto err;                            \
    } while (0)

    // 添加 criu 命令到 argv 数组
    argv[argc++] = on_path("criu", NULL);
    if (!argv[argc - 1]) {
        ERROR("Couldn't find criu binary\n");
        goto err;
    }

    // 添加公共参数到 argv 数组
    DECLARE_ARG(opts->action);
    DECLARE_ARG("--tcp-established");
    DECLARE_ARG("--file-locks");
    DECLARE_ARG("--link-remap");
    DECLARE_ARG("--force-irmap");
    DECLARE_ARG("--manage-cgroups");
    DECLARE_ARG("--action-script");
    DECLARE_ARG(DATADIR "/lxc/lxc-restore-net");
    DECLARE_ARG("-D");
    DECLARE_ARG(opts->directory);
    DECLARE_ARG("-o");
    DECLARE_ARG(log);

    // 如果 verbose 为 true，添加详细日志参数
    if (opts->verbose)
        DECLARE_ARG("-vvvvvv");

    // 根据不同的操作，添加不同的参数
    if (strcmp(opts->action, "dump") == 0) {
        char pid[32];

        // 获取容器的初始 PID
        if (sprintf(pid, "%d", lxcapi_init_pid(opts->c)) < 0)
            goto err;

        // 添加 dump 操作的特定参数
        DECLARE_ARG("-t");
        DECLARE_ARG(pid);
        if (!opts->stop)
            DECLARE_ARG("--leave-running");
    } else if (strcmp(opts->action, "restore") == 0) {
        // 添加 restore 操作的特定参数
        DECLARE_ARG("--root");
        DECLARE_ARG(opts->c->lxc_conf->rootfs.mount);
        DECLARE_ARG("--restore-detached");
        DECLARE_ARG("--restore-sibling");
        DECLARE_ARG("--pidfile");
        DECLARE_ARG(opts->pidfile);
        DECLARE_ARG("--cgroup-root");
        DECLARE_ARG(opts->cgroup_path);

        // 遍历容器的网络配置，添加网络相关参数
        lxc_list_for_each(it, &opts->c->lxc_conf->network)
        {
            char eth[128], *veth;
            void *m;
            struct lxc_netdev *n = it->elem;

            // 设置以太网接口名称
            if (n->name) {
                if (strlen(n->name) >= sizeof(eth))
                    goto err;
                strncpy(eth, n->name, sizeof(eth));
            } else {
                sprintf(eth, "eth%d", netnr);
            }

            // 获取虚拟以太网设备对的名称
            veth = n->priv.veth_attr.pair;

            // 格式化以太网接口和虚拟以太网设备对的名称
            ret = snprintf(buf, sizeof(buf), "%s=%s", eth, veth);
            if (ret < 0 || ret >= sizeof(buf))
                goto err;

            // 重新分配 argv 数组以增加新的参数
            m = realloc(argv, (argc + 1 + 2) * sizeof(*argv));
            if (!m)
                goto err;
            argv = m;

            // 添加 --veth-pair 参数及其值
            DECLARE_ARG("--veth-pair");
            DECLARE_ARG(buf);

            // 确保参数数组以 NULL 结尾
            argv[argc] = NULL;

            // 增加网络接口计数器
            netnr++;
        }
    }

    // 重置网络接口计数器，设置环境变量用于网络恢复
    netnr = 0;
    lxc_list_for_each(it, &opts->c->lxc_conf->network)
    {
        struct lxc_netdev *n = it->elem;
        char veth[128];

        /*
          * 这里我们设置一些参数，lxc-restore-net 将会检查这些参数
          * 以找出正确的网络来恢复。
          */
        snprintf(buf, sizeof(buf), "LXC_CRIU_BRIDGE%d", netnr);
        if (setenv(buf, n->link, 1))
            goto err;

        if (strcmp("restore", opts->action) == 0)
            strncpy(veth, n->priv.veth_attr.pair, sizeof(veth));
        else {
            char *tmp;
            ret = snprintf(buf, sizeof(buf), "lxc.network.%d.veth.pair", netnr);
            if (ret < 0 || ret >= sizeof(buf))
                goto err;
            tmp = lxcapi_get_running_config_item(opts->c, buf);
            strncpy(veth, tmp, sizeof(veth));
            free(tmp);
        }

        snprintf(buf, sizeof(buf), "LXC_CRIU_VETH%d", netnr);
        if (setenv(buf, veth, 1))
            goto err;

        netnr++;
    }

    // 执行 CRIU 命令
#undef DECLARE_ARG
    execv(argv[0], argv);
err:
    // 发生错误时，释放已分配的内存
    for (i = 0; argv[i]; i++)
        free(argv[i]);
    free(argv);
}

/* Check and make sure the container has a configuration that we know CRIU can
 * dump. */
static bool criu_ok(struct lxc_container *c)
{
    struct lxc_list *it;
    bool found_deny_rule = false;

    if (geteuid()) {
        ERROR("Must be root to checkpoint\n");
        return false;
    }

    /* We only know how to restore containers with veth networks. */
    lxc_list_for_each(it, &c->lxc_conf->network)
    {
        struct lxc_netdev *n = it->elem;
        if (n->type != LXC_NET_VETH && n->type != LXC_NET_NONE) {
            ERROR("Found network that is not VETH or NONE\n");
            return false;
        }
    }

    // These requirements come from http://criu.org/LXC
    if (c->lxc_conf->console.path && strcmp(c->lxc_conf->console.path, "none") != 0) {
        ERROR("lxc.console must be none\n");
        return false;
    }

    if (c->lxc_conf->tty != 0) {
        ERROR("lxc.tty must be 0\n");
        return false;
    }

    lxc_list_for_each(it, &c->lxc_conf->cgroup)
    {
        struct lxc_cgroup *cg = it->elem;
        if (strcmp(cg->subsystem, "devices.deny") == 0 && strcmp(cg->value, "c 5:1 rwm") == 0) {
            found_deny_rule = true;
            break;
        }
    }

    if (!found_deny_rule) {
        ERROR("couldn't find devices.deny = c 5:1 rwm");
        return false;
    }

    return true;
}

static bool dump_net_info(struct lxc_container *c, char *directory)
{
    int netnr;
    struct lxc_list *it;

    netnr = 0;
    lxc_list_for_each(it, &c->lxc_conf->network)
    {
        char *veth = NULL, *bridge = NULL, veth_path[PATH_MAX], eth[128];
        struct lxc_netdev *n = it->elem;
        bool has_error = true;
        int pret;

        pret = snprintf(veth_path, PATH_MAX, "lxc.network.%d.veth.pair", netnr);
        if (pret < 0 || pret >= PATH_MAX)
            goto out;

        veth = lxcapi_get_running_config_item(c, veth_path);
        if (!veth) {
            /* criu_ok() checks that all interfaces are
			 * LXC_NET{VETH,NONE}, and VETHs should have this
			 * config */
            assert(n->type == LXC_NET_NONE);
            break;
        }

        bridge = lxcapi_get_running_config_item(c, veth_path);
        if (!bridge)
            goto out;

        pret = snprintf(veth_path, PATH_MAX, "%s/veth%d", directory, netnr);
        if (pret < 0 || pret >= PATH_MAX || print_to_file(veth_path, veth) < 0)
            goto out;

        if (n->name) {
            if (strlen(n->name) >= 128)
                goto out;
            strncpy(eth, n->name, 128);
        } else
            sprintf(eth, "eth%d", netnr);

        has_error = false;
    out:
        free(veth);
        free(bridge);
        if (has_error)
            return false;
    }

    return true;
}

/**
 * 检查点运行中的容器。
 *
 * @param c 要检查点的容器。
 * @param directory 保存检查点数据的目录。
 * @param stop 如果为 true，容器将在检查点之后停止。
 * @param verbose 如果为 true，在检查点过程中启用详细日志记录。
 * @return 成功返回 true，失败返回 false。
 */
static bool lxcapi_checkpoint(struct lxc_container *c, char *directory, bool stop, bool verbose)
{
    pid_t pid;
    int status;

    // 检查容器是否支持 CRIU（用户空间的检查点/恢复）
    if (!criu_ok(c))
        return false;

    // 尝试创建保存检查点数据的目录，权限为 0700
    if (mkdir(directory, 0700) < 0 && errno != EEXIST)
        return false;

    // 将网络信息转储到指定目录
    if (!dump_net_info(c, directory))
        return false;

    // fork 当前进程以创建用于检查点的子进程
    pid = fork();
    if (pid < 0)
        return false; // fork 失败

    if (pid == 0) {
        // 子进程：准备并执行用于检查点的 CRIU 命令

        struct criu_opts os;
        os.action = "dump"; // 将操作设置为 "dump" 以进行检查点
        os.directory = directory; // 设置检查点数据的目录
        os.c = c; // 传递容器对象
        os.stop = stop; // 传递停止标志
        os.verbose = verbose; // 传递详细标志

        // 执行 CRIU 命令；如果 exec_criu() 返回，则发生错误
        exec_criu(&os);
        exit(1); // 如果 exec_criu() 返回，则以错误状态退出
    } else {
        // 父进程：等待子进程完成

        pid_t w = waitpid(pid, &status, 0); // 等待子进程
        if (w == -1) {
            perror("waitpid"); // 如果 waitpid() 失败，打印错误消息
            return false;
        }

        // 检查子进程是否正常退出
        if (WIFEXITED(status)) {
            // 如果子进程以状态 0（成功）退出，则返回 true，否则返回 false
            return !WEXITSTATUS(status);
        }

        // 如果子进程未正常退出，则返回 false
        return false;
    }
}

static bool restore_net_info(struct lxc_container *c)
{
    struct lxc_list *it;
    bool has_error = true;

    if (container_mem_lock(c))
        return false;

    lxc_list_for_each(it, &c->lxc_conf->network)
    {
        struct lxc_netdev *netdev = it->elem;
        char template[IFNAMSIZ];
        snprintf(template, sizeof(template), "vethXXXXXX");

        if (!netdev->priv.veth_attr.pair)
            netdev->priv.veth_attr.pair = lxc_mkifname(template);

        if (!netdev->priv.veth_attr.pair)
            goto out_unlock;
    }

    has_error = false;

out_unlock:
    container_mem_unlock(c);
    return !has_error;
}

static bool lxcapi_restore(struct lxc_container *c, char *directory, bool verbose)
{
    pid_t pid;
    struct lxc_rootfs *rootfs;
    char pidfile[L_tmpnam];
    struct lxc_handler *handler;
    bool has_error = true;

    if (!criu_ok(c))
        return false;

    if (geteuid()) {
        ERROR("Must be root to restore\n");
        return false;
    }

    if (!tmpnam(pidfile))
        return false;

    handler = lxc_init(c->name, c->lxc_conf, c->config_path);
    if (!handler)
        return false;

    if (!cgroup_init(handler)) {
        ERROR("failed initing cgroups");
        goto out_fini_handler;
    }

    if (!cgroup_create(handler)) {
        ERROR("failed creating groups");
        goto out_fini_handler;
    }

    if (!restore_net_info(c)) {
        ERROR("failed restoring network info");
        goto out_fini_handler;
    }

    pid = fork();
    if (pid < 0)
        goto out_fini_handler;

    if (pid == 0) {
        struct criu_opts os;

        if (unshare(CLONE_NEWNS))
            exit(1);

        /* CRIU needs the lxc root bind mounted so that it is the root of some
		 * mount. */
        rootfs = &c->lxc_conf->rootfs;

        if (rootfs_is_blockdev(c->lxc_conf)) {
            if (do_rootfs_setup(c->lxc_conf, c->name, c->config_path) < 0)
                exit(1);
        } else {
            if (mkdir(rootfs->mount, 0755) < 0 && errno != EEXIST)
                exit(1);

            if (mount(rootfs->path, rootfs->mount, NULL, MS_BIND, NULL) < 0) {
                rmdir(rootfs->mount);
                exit(1);
            }
        }

        os.action = "restore";
        os.directory = directory;
        os.c = c;
        os.pidfile = pidfile;
        os.verbose = verbose;
        os.cgroup_path = cgroup_canonical_path(handler);

        /* exec_criu() returning is an error */
        exec_criu(&os);
        umount(rootfs->mount);
        rmdir(rootfs->mount);
        exit(1);
    } else {
        int status;

        pid_t w = waitpid(pid, &status, 0);

        if (w == -1) {
            perror("waitpid");
            goto out_fini_handler;
        }

        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status)) {
                goto out_fini_handler;
            } else {
                int ret;
                FILE *f = fopen(pidfile, "r");
                if (!f) {
                    perror("reading pidfile");
                    ERROR("couldn't read restore's init pidfile %s\n", pidfile);
                    goto out_fini_handler;
                }

                ret = fscanf(f, "%d", (int *)&handler->pid);
                fclose(f);
                if (ret != 1) {
                    ERROR("reading restore pid failed");
                    goto out_fini_handler;
                }

                if (lxc_set_state(c->name, handler, RUNNING))
                    goto out_fini_handler;
            }
        } else {
            ERROR("CRIU was killed with signal %d\n", WTERMSIG(status));
            goto out_fini_handler;
        }

        if (lxc_poll(c->name, handler)) {
            lxc_abort(c->name, handler);
            goto out_fini_handler;
        }
    }

    has_error = false;

out_fini_handler:
    lxc_fini(c->name, handler);
    return !has_error;
}

static int lxcapi_attach_run_waitl(struct lxc_container *c, lxc_attach_options_t *options, const char *program,
                                   const char *arg, ...)
{
    va_list ap;
    const char **argv;
    int ret;

    if (!c)
        return -1;

    va_start(ap, arg);
    argv = lxc_va_arg_list_to_argv_const(ap, 1);
    va_end(ap);

    if (!argv) {
        ERROR("Memory allocation error.");
        return -1;
    }
    argv[0] = arg;

    ret = lxcapi_attach_run_wait(c, options, program, (const char *const *)argv);
    free((void *)argv);
    return ret;
}

struct lxc_container *lxc_container_new(const char *name, const char *configpath)
{
    struct lxc_container *c;

    if (!name)
        return NULL;

    c = malloc(sizeof(*c));
    if (!c) {
        fprintf(stderr, "failed to malloc lxc_container\n");
        return NULL;
    }
    memset(c, 0, sizeof(*c));

    if (configpath)
        c->config_path = strdup(configpath);
    else
        c->config_path = strdup(lxc_global_config_value("lxc.lxcpath"));

    if (!c->config_path) {
        fprintf(stderr, "Out of memory\n");
        goto err;
    }

    remove_trailing_slashes(c->config_path);
    c->name = malloc(strlen(name) + 1);
    if (!c->name) {
        fprintf(stderr, "Error allocating lxc_container name\n");
        goto err;
    }
    strcpy(c->name, name);

    c->numthreads = 1;
    if (!(c->slock = lxc_newlock(c->config_path, name))) {
        fprintf(stderr, "failed to create lock\n");
        goto err;
    }

    if (!(c->privlock = lxc_newlock(NULL, NULL))) {
        fprintf(stderr, "failed to alloc privlock\n");
        goto err;
    }

    if (!set_config_filename(c)) {
        fprintf(stderr, "Error allocating config file pathname\n");
        goto err;
    }

    if (file_exists(c->configfile) && !lxcapi_load_config(c, NULL))
        goto err;

    if (ongoing_create(c) == 2) {
        ERROR("Error: %s creation was not completed", c->name);
        container_destroy(c);
        lxcapi_clear_config(c);
    }
    c->daemonize = true;
    c->pidfile = NULL;

    // assign the member functions
    c->is_defined = lxcapi_is_defined;
    c->state = lxcapi_state;
    c->is_running = lxcapi_is_running;
    c->freeze = lxcapi_freeze;
    c->unfreeze = lxcapi_unfreeze;
    c->console = lxcapi_console;
    c->console_getfd = lxcapi_console_getfd;
    c->init_pid = lxcapi_init_pid;
    c->load_config = lxcapi_load_config;
    c->want_daemonize = lxcapi_want_daemonize;
    c->want_close_all_fds = lxcapi_want_close_all_fds;
    c->start = lxcapi_start;
    c->startl = lxcapi_startl;
    c->stop = lxcapi_stop;
    c->config_file_name = lxcapi_config_file_name;
    c->wait = lxcapi_wait;
    c->set_config_item = lxcapi_set_config_item;
    c->destroy = lxcapi_destroy;
    c->destroy_with_snapshots = lxcapi_destroy_with_snapshots;
    c->rename = lxcapi_rename;
    c->save_config = lxcapi_save_config;
    c->get_keys = lxcapi_get_keys;
    c->create = lxcapi_create;
    c->createl = lxcapi_createl;
    c->shutdown = lxcapi_shutdown;
    c->reboot = lxcapi_reboot;
    c->clear_config = lxcapi_clear_config;
    c->clear_config_item = lxcapi_clear_config_item;
    c->get_config_item = lxcapi_get_config_item;
    c->get_running_config_item = lxcapi_get_running_config_item;
    c->get_cgroup_item = lxcapi_get_cgroup_item;
    c->set_cgroup_item = lxcapi_set_cgroup_item;
    c->get_config_path = lxcapi_get_config_path;
    c->set_config_path = lxcapi_set_config_path;
    c->clone = lxcapi_clone;
    c->get_interfaces = lxcapi_get_interfaces;
    c->get_ips = lxcapi_get_ips;
    c->attach = lxcapi_attach;
    c->attach_run_wait = lxcapi_attach_run_wait;
    c->attach_run_waitl = lxcapi_attach_run_waitl;
    c->snapshot = lxcapi_snapshot;
    c->snapshot_list = lxcapi_snapshot_list;
    c->snapshot_restore = lxcapi_snapshot_restore;
    c->snapshot_destroy = lxcapi_snapshot_destroy;
    c->snapshot_destroy_all = lxcapi_snapshot_destroy_all;
    c->may_control = lxcapi_may_control;
    c->add_device_node = lxcapi_add_device_node;
    c->remove_device_node = lxcapi_remove_device_node;
    c->attach_interface = lxcapi_attach_interface;
    c->detach_interface = lxcapi_detach_interface;
    c->checkpoint = lxcapi_checkpoint;
    c->restore = lxcapi_restore;

    /* we'll allow the caller to update these later */
    if (lxc_log_init(NULL, "none", NULL, "lxc_container", 0, c->config_path)) {
        fprintf(stderr, "failed to open log\n");
        goto err;
    }

    return c;

err:
    lxc_container_free(c);
    return NULL;
}

int lxc_get_wait_states(const char **states)
{
    int i;

    if (states)
        for (i = 0; i < MAX_STATE; i++)
            states[i] = lxc_state2str(i);
    return MAX_STATE;
}

/*
 * These next two could probably be done smarter with reusing a common function
 * with different iterators and tests...
 */
int list_defined_containers(const char *lxcpath, char ***names, struct lxc_container ***cret)
{
    DIR *dir;
    int i, cfound = 0, nfound = 0;
    struct dirent dirent, *direntp;
    struct lxc_container *c;

    if (!lxcpath)
        lxcpath = lxc_global_config_value("lxc.lxcpath");

    dir = opendir(lxcpath);
    if (!dir) {
        SYSERROR("opendir on lxcpath");
        return -1;
    }

    if (cret)
        *cret = NULL;
    if (names)
        *names = NULL;

    while (!readdir_r(dir, &dirent, &direntp)) {
        if (!direntp)
            break;
        if (!strcmp(direntp->d_name, "."))
            continue;
        if (!strcmp(direntp->d_name, ".."))
            continue;

        if (!config_file_exists(lxcpath, direntp->d_name))
            continue;

        if (names) {
            if (!add_to_array(names, direntp->d_name, cfound))
                goto free_bad;
        }
        cfound++;

        if (!cret) {
            nfound++;
            continue;
        }

        c = lxc_container_new(direntp->d_name, lxcpath);
        if (!c) {
            INFO("Container %s:%s has a config but could not be loaded", lxcpath, direntp->d_name);
            if (names)
                if (!remove_from_array(names, direntp->d_name, cfound--))
                    goto free_bad;
            continue;
        }
        if (!lxcapi_is_defined(c)) {
            INFO("Container %s:%s has a config but is not defined", lxcpath, direntp->d_name);
            if (names)
                if (!remove_from_array(names, direntp->d_name, cfound--))
                    goto free_bad;
            lxc_container_put(c);
            continue;
        }

        if (!add_to_clist(cret, c, nfound, true)) {
            lxc_container_put(c);
            goto free_bad;
        }
        nfound++;
    }

    closedir(dir);
    return nfound;

free_bad:
    if (names && *names) {
        for (i = 0; i < cfound; i++)
            free((*names)[i]);
        free(*names);
    }
    if (cret && *cret) {
        for (i = 0; i < nfound; i++)
            lxc_container_put((*cret)[i]);
        free(*cret);
    }
    closedir(dir);
    return -1;
}

int list_active_containers(const char *lxcpath, char ***nret, struct lxc_container ***cret)
{
    int i, ret = -1, cret_cnt = 0, ct_name_cnt = 0;
    int lxcpath_len;
    char *line = NULL;
    char **ct_name = NULL;
    size_t len = 0;
    struct lxc_container *c;
    bool is_hashed;

    if (!lxcpath)
        lxcpath = lxc_global_config_value("lxc.lxcpath");
    lxcpath_len = strlen(lxcpath);

    if (cret)
        *cret = NULL;
    if (nret)
        *nret = NULL;

    FILE *f = fopen("/proc/net/unix", "r");
    if (!f)
        return -1;

    while (getline(&line, &len, f) != -1) {
        char *p = strrchr(line, ' '), *p2;
        if (!p)
            continue;
        p++;
        if (*p != 0x40)
            continue;
        p++;

        is_hashed = false;
        if (strncmp(p, lxcpath, lxcpath_len) == 0) {
            p += lxcpath_len;
        } else if (strncmp(p, "lxc/", 4) == 0) {
            p += 4;
            is_hashed = true;
        } else {
            continue;
        }

        while (*p == '/')
            p++;

        // Now p is the start of lxc_name
        p2 = strchr(p, '/');
        if (!p2 || strncmp(p2, "/command", 8) != 0)
            continue;
        *p2 = '\0';

        if (is_hashed) {
            if (strncmp(lxcpath, lxc_cmd_get_lxcpath(p), lxcpath_len) != 0)
                continue;
            p = lxc_cmd_get_name(p);
        }

        if (array_contains(&ct_name, p, ct_name_cnt))
            continue;

        if (!add_to_array(&ct_name, p, ct_name_cnt))
            goto free_cret_list;

        ct_name_cnt++;

        if (!cret)
            continue;

        c = lxc_container_new(p, lxcpath);
        if (!c) {
            INFO("Container %s:%s is running but could not be loaded", lxcpath, p);
            remove_from_array(&ct_name, p, ct_name_cnt--);
            continue;
        }

        /*
		 * If this is an anonymous container, then is_defined *can*
		 * return false.  So we don't do that check.  Count on the
		 * fact that the command socket exists.
		 */

        if (!add_to_clist(cret, c, cret_cnt, true)) {
            lxc_container_put(c);
            goto free_cret_list;
        }
        cret_cnt++;
    }

    assert(!nret || !cret || cret_cnt == ct_name_cnt);
    ret = ct_name_cnt;
    if (nret)
        *nret = ct_name;
    else
        goto free_ct_name;
    goto out;

free_cret_list:
    if (cret && *cret) {
        for (i = 0; i < cret_cnt; i++)
            lxc_container_put((*cret)[i]);
        free(*cret);
    }

free_ct_name:
    if (ct_name) {
        for (i = 0; i < ct_name_cnt; i++)
            free(ct_name[i]);
        free(ct_name);
    }

out:
    free(line);

    fclose(f);
    return ret;
}

int list_all_containers(const char *lxcpath, char ***nret, struct lxc_container ***cret)
{
    int i, ret, active_cnt, ct_cnt, ct_list_cnt;
    char **active_name;
    char **ct_name;
    struct lxc_container **ct_list = NULL;

    ct_cnt = list_defined_containers(lxcpath, &ct_name, NULL);
    if (ct_cnt < 0)
        return ct_cnt;

    active_cnt = list_active_containers(lxcpath, &active_name, NULL);
    if (active_cnt < 0) {
        ret = active_cnt;
        goto free_ct_name;
    }

    for (i = 0; i < active_cnt; i++) {
        if (!array_contains(&ct_name, active_name[i], ct_cnt)) {
            if (!add_to_array(&ct_name, active_name[i], ct_cnt)) {
                ret = -1;
                goto free_active_name;
            }
            ct_cnt++;
        }
        free(active_name[i]);
        active_name[i] = NULL;
    }
    free(active_name);
    active_name = NULL;
    active_cnt = 0;

    for (i = 0, ct_list_cnt = 0; i < ct_cnt && cret; i++) {
        struct lxc_container *c;

        c = lxc_container_new(ct_name[i], lxcpath);
        if (!c) {
            WARN("Container %s:%s could not be loaded", lxcpath, ct_name[i]);
            remove_from_array(&ct_name, ct_name[i], ct_cnt--);
            continue;
        }

        if (!add_to_clist(&ct_list, c, ct_list_cnt, false)) {
            lxc_container_put(c);
            ret = -1;
            goto free_ct_list;
        }
        ct_list_cnt++;
    }

    if (cret)
        *cret = ct_list;

    if (nret)
        *nret = ct_name;
    else {
        ret = ct_cnt;
        goto free_ct_name;
    }
    return ct_cnt;

free_ct_list:
    for (i = 0; i < ct_list_cnt; i++) {
        lxc_container_put(ct_list[i]);
    }
    free(ct_list);

free_active_name:
    for (i = 0; i < active_cnt; i++) {
        free(active_name[i]);
    }
    free(active_name);

free_ct_name:
    for (i = 0; i < ct_cnt; i++) {
        free(ct_name[i]);
    }
    free(ct_name);
    return ret;
}
