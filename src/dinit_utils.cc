#include <cstring>

#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "dinit-userservd.hh"

bool dinit_boot(session &sess, bool disabled) {
    print_dbg("dinit: boot wait");
    auto pid = fork();
    if (pid < 0) {
        print_err("dinit: fork failed (%s)", strerror(errno));
        /* unrecoverable */
        return false;
    }
    if (pid != 0) {
        /* parent process */
        sess.start_pid = pid;
        return true;
    }
    if (disabled) {
        /* if dinit is not managed, simply succeed immediately */
        exit(0);
        return true;
    }
    /* child process */
    if (getuid() == 0) {
        if (setgid(sess.gid) != 0) {
            print_err("dinit: failed to set gid (%s)", strerror(errno));
            exit(1);
        }
        if (setuid(sess.uid) != 0) {
            print_err("dinit: failed to set uid (%s)", strerror(errno));
            exit(1);
        }
    }
    execlp(
        "dinitctl", "dinitctl",
        "--socket-path", sess.csock, "start", "boot", nullptr
    );
    exit(1);
    return true;
}

void dinit_child(session &sess, char const *pipenum) {
    if (getuid() == 0) {
        auto *pw = getpwuid(sess.uid);
        if (!pw) {
            perror("dinit: getpwuid failed");
            return;
        }
        if (setgid(sess.gid) != 0) {
            perror("dinit: failed to set gid");
            return;
        }
        if (initgroups(pw->pw_name, sess.gid) != 0) {
            perror("dinit: failed to set supplementary groups");
            return;
        }
        if (setuid(sess.uid) != 0) {
            perror("dinit: failed to set uid");
            return;
        }
    }
    /* set up dinit tempdir after we drop privileges */
    char tdirn[38];
    std::snprintf(
        tdirn, sizeof(tdirn), "dinit.%lu",
        static_cast<unsigned long>(getpid())
    );
    int tdirfd = dir_make_at(sess.dirfd, tdirn, 0700);
    if (tdirfd < 0) {
        perror("dinit: failed to create dinit dir");
        return;
    }
    /* set up service files */
    {
        auto bfd = openat(tdirfd, "boot", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (bfd < 0) {
            perror("dinit: openat failed");
            return;
        }
        /* reopen as a real file handle, now owns bfd */
        auto *f = fdopen(bfd, "w");
        if (!f) {
            perror("dinit: fdopen failed");
            return;
        }
        /* write boot service */
        std::fprintf(f, "type = internal\n");
        /* system service dependency */
        std::fprintf(f, "depends-on = system\n");
        /* wait for a service directory */
        std::fprintf(
            f, "waits-for.d = %s/%s\n", sess.homedir,
            cdata->boot_path.data()
        );
        std::fclose(f);
        /* now system */
        bfd = openat(tdirfd, "system", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (bfd < 0) {
            perror("dinit: openat failed");
            return;
        }
        /* ditto */
        f = fdopen(bfd, "w");
        if (!f) {
            perror("dinit: fdopen failed");
            return;
        }
        /* this is also internal */
        std::fprintf(f, "type = internal\n");
        /* wait for system service directory */
        std::fprintf(f, "waits-for.d = %s\n", cdata->sys_boot_path.data());
        std::fclose(f);
    }
    /* create boot path, if possible; if it fails, it fails (harmless-ish) */
    int hfd = open(sess.homedir, O_RDONLY);
    if (struct stat hstat; !fstat(hfd, &hstat) && S_ISDIR(hstat.st_mode)) {
        char *bptr = &cdata->boot_path[0];
        /* boot dir already exists */
        if (!fstatat(hfd, bptr, &hstat, 0) && S_ISDIR(hstat.st_mode)) {
            goto bdir_done;
        }
        /* otherwise recursively create it */
        char *sl = std::strchr(bptr, '/');
        while (sl) {
            *sl = '\0';
            if (fstatat(hfd, bptr, &hstat, 0) || !S_ISDIR(hstat.st_mode)) {
                if (mkdirat(hfd, bptr, 0755)) {
                    *sl = '/';
                    goto bdir_done;
                }
            }
            *sl = '/';
            sl = strchr(sl + 1, '/');
        }
        /* actually create the dir itself */
        mkdirat(hfd, bptr, 0755);
    }
bdir_done:
    close(hfd);
    /* build up env and args list */
    std::vector<char> execs{};
    std::size_t argc = 0, nexec = 0;
    auto add_str = [&execs, &nexec](auto &&...s) {
        (execs.insert(execs.end(), s, s + std::strlen(s)), ...);
        execs.push_back('\0');
        ++nexec;
    };
    /* argv starts here */
    add_str("dinit");
    add_str("--user");
    add_str("--ready-fd");
    add_str(pipenum);
    add_str("--services-dir");
    add_str(RUN_PATH, "/", SOCK_DIR, "/", sess.uids, "/", tdirn);
    /* onwards */
    for (auto &sp: cdata->srv_paths) {
        add_str("--services-dir");
        if (sp.data()[0] != '/') {
            add_str(sess.homedir, "/", sp.data());
        } else {
            add_str(sp.data());
        }
    }
    argc = nexec;
    /* environment starts here */
    add_str("HOME=", sess.homedir);
    add_str("UID=", sess.uids);
    add_str("GID=", sess.gids);
    add_str("PATH=/usr/local/bin:/usr/bin:/bin");
    if (sess.rundir[0]) {
        add_str("XDG_RUNTIME_DIR=", sess.rundir);
    }
    /* make up env and arg arrays */
    std::vector<char const *> argp{};
    {
        char const *execsp = execs.data();
        argp.reserve(nexec + 2);
        for (std::size_t i = 0; i < argc; ++i) {
            argp.push_back(execsp);
            execsp += std::strlen(execsp) + 1;
        }
        argp.push_back(nullptr);
        for (std::size_t i = argc; i < nexec; ++i) {
            argp.push_back(execsp);
            execsp += std::strlen(execsp) + 1;
        }
        argp.push_back(nullptr);
    }
    auto *argv = const_cast<char **>(&argp[0]);
    /* restore umask to user default */
    umask(022);
    /* fire */
    execvpe("dinit", argv, argv + argc + 1);
}
