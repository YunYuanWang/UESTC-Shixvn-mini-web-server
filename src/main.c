#include "../include/config.h"
#include "../include/log.h"
#include "../include/process_server.h"
#include "../include/tcp_fork_server.h"
#include "../include/tcp_server.h"
#include "../include/user_store.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_help(const char *prog) {
    printf("usage: %s conf/server.conf\n", prog);
    printf("       %s load <csv_path>\n", prog);
    printf("       %s findusr <name>\n", prog);
    printf("       %s addusr <name,password,birthdate,phone,mobile,email>\n", prog);
    printf("       %s delusr <name>\n", prog);
    printf("       %s users index\n", prog);
    printf("       %s users find-index <name>\n", prog);
    printf("       %s users compare_search_method <name>\n", prog);
    printf("       %s users compare_search_method --verbose <name>\n", prog);
    printf("       %s process\n", prog);
    printf("       %s fork\n", prog);
    printf("       %s help\n", prog);
}

static void print_interactive_help(void) {
    printf("Interactive commands:\n");
    printf("  findusr <name>\n");
    printf("  addusr <name,password,birthdate,phone,mobile,email>\n");
    printf("  delusr <name>\n");
    printf("  users index\n");
    printf("  users find-index <name>\n");
    printf("  users compare_search_method <name>\n");
    printf("  users compare_search_method --verbose <name>\n");
    printf("  help\n");
    printf("  quit / exit\n");
}

/*
 * Tokenize a line into argc/argv, splitting on whitespace.
 * Modifies the line in-place (null-terminates each token).
 * Returns argc.  argv must have room for at least MAX_ARGC pointers.
 */
#define MAX_ARGC 16

static int tokenize(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        argv[argc++] = p;

        /* find end of token */
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            p++;
        }
        if (*p) {
            *p = '\0';
            p++;
        }
    }

    return argc;
}

/*
 * Dispatch a single command.
 * In standalone mode, each command loads CSV, executes, then frees.
 * In interactive mode, CSV is already loaded.
 * Returns 0 on success, 1 on error, -1 to quit interactive mode.
 */
static int dispatch(int argc, char *argv[], int interactive) {
    if (argc == 0) {
        return 0;
    }

    /* --- quit / exit --- */
    if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0) {
        if (interactive) {
            return -1;
        }
        return 0;
    }

    /* --- help --- */
    if (strcmp(argv[0], "help") == 0) {
        if (interactive) {
            print_interactive_help();
        } else {
            print_help("./mini_web_server");
        }
        return 0;
    }

    /* --- findusr <name> --- */
    if (strcmp(argv[0], "findusr") == 0) {
        ListNode *u;
        if (argc < 2) {
            printf("error: 'findusr' requires an argument, "
                   "use './mini_web_server help' for usage\n");
            return 1;
        }
        if (!interactive) {
            if (user_store_load_csv("data/users.csv") < 0) {
                printf("error: cannot open data/users.csv\n");
                return 1;
            }
        }
        u = user_store_find(argv[1]);
        printf(u != NULL ? "FOUND\n" : "NOT_FOUND\n");
        if (!interactive) {
            user_store_free();
        }
        return 0;
    }

    /* --- addusr <csv_line> --- */
    if (strcmp(argv[0], "addusr") == 0) {
        int ret;
        if (argc < 2) {
            printf("error: 'addusr' requires an argument, "
                   "use './mini_web_server help' for usage\n");
            return 1;
        }
        if (!interactive) {
            if (user_store_load_csv("data/users.csv") < 0) {
                printf("error: cannot open data/users.csv\n");
                return 1;
            }
        }
        ret = user_store_add(argv[1]);
        printf(ret == 0 ? "ADDED\n" : "EXISTS\n");
        if (!interactive) {
            user_store_free();
        }
        return ret == 0 ? 0 : 1;
    }

    /* --- delusr <name> --- */
    if (strcmp(argv[0], "delusr") == 0) {
        int ret;
        if (argc < 2) {
            printf("error: 'delusr' requires an argument, "
                   "use './mini_web_server help' for usage\n");
            return 1;
        }
        if (!interactive) {
            if (user_store_load_csv("data/users.csv") < 0) {
                printf("error: cannot open data/users.csv\n");
                return 1;
            }
        }
        ret = user_store_delete(argv[1]);
        printf(ret == 0 ? "DELETED\n" : "NO_SUCH_USER\n");
        if (!interactive) {
            user_store_free();
        }
        return ret == 0 ? 0 : 1;
    }

    /* --- users index --- */
    if (strcmp(argv[0], "users") == 0 && argc >= 2 &&
        strcmp(argv[1], "index") == 0) {
        if (!interactive) {
            if (user_store_load_csv("data/users.csv") < 0) {
                printf("error: cannot open data/users.csv\n");
                return 1;
            }
        }
        user_store_print_index();
        if (!interactive) {
            user_store_free();
        }
        return 0;
    }

    /* --- users find-index <name> --- */
    if (strcmp(argv[0], "users") == 0 && argc >= 2 &&
        strcmp(argv[1], "find-index") == 0) {
        ListNode *u;
        if (argc < 3) {
            printf("error: 'users find-index' requires a name argument, "
                   "use './mini_web_server help' for usage\n");
            return 1;
        }
        if (!interactive) {
            if (user_store_load_csv("data/users.csv") < 0) {
                printf("error: cannot open data/users.csv\n");
                return 1;
            }
        }
        u = user_store_find_index(argv[2]);
        printf(u != NULL ? "FOUND\n" : "NOT_FOUND\n");
        if (!interactive) {
            user_store_free();
        }
        return 0;
    }

    /* --- users compare_search_method [--verbose] <name> --- */
    if (strcmp(argv[0], "users") == 0 && argc >= 2 &&
        strcmp(argv[1], "compare_search_method") == 0) {
        int verbose = 0;
        const char *name = NULL;

        if (argc == 3) {
            name = argv[2];
        } else if (argc == 4) {
            if (strcmp(argv[2], "--verbose") == 0) {
                verbose = 1;
                name = argv[3];
            } else if (strcmp(argv[3], "--verbose") == 0) {
                verbose = 1;
                name = argv[2];
            }
        }

        if (name == NULL) {
            printf("error: 'users compare_search_method' requires a name "
                   "argument, use './mini_web_server help' for usage\n");
            return 1;
        }

        if (!interactive) {
            if (user_store_load_csv("data/users.csv") < 0) {
                printf("error: cannot open data/users.csv\n");
                return 1;
            }
        }
        user_store_compare_search_method(name, verbose);
        if (!interactive) {
            user_store_free();
        }
        return 0;
    }

    /* --- users error handling --- */
    if (strcmp(argv[0], "users") == 0) {
        if (argc == 1) {
            printf("error: 'users' requires a subcommand, "
                   "use './mini_web_server help' for usage\n");
            return 1;
        }
        if (argc == 2 &&
            (strcmp(argv[1], "find-index") == 0 ||
             strcmp(argv[1], "compare_search_method") == 0)) {
            printf("error: 'users %s' requires a name argument, "
                   "use './mini_web_server help' for usage\n", argv[1]);
            return 1;
        }
        printf("error: unknown users subcommand '%s', "
               "use './mini_web_server help' for usage\n",
               (argc >= 2 ? argv[1] : ""));
        return 1;
    }

    /* unknown command */
    printf("error: unknown command '%s'\n", argv[0]);
    return 1;
}

/*
 * Interactive mode: load a CSV, then read commands from stdin.
 */
static int run_interactive(const char *csv_path) {
    char line[1024];
    char *argv[MAX_ARGC];
    int argc;
    int ret;

    printf("Loading %s ...\n", csv_path);
    fflush(stdout);

    if (user_store_load_csv(csv_path) < 0) {
        printf("error: cannot open '%s'\n", csv_path);
        return 1;
    }

    printf("Ready.  Type 'help' for commands, 'quit' to exit.\n\n");

    for (;;) {
        printf("> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        argc = tokenize(line, argv, MAX_ARGC);

        if (argc == 0) {
            continue;  /* empty line */
        }

        ret = dispatch(argc, argv, 1);
        if (ret < 0) {
            break;  /* quit/exit */
        }
    }

    user_store_free();
    printf("Bye.\n");
    return 0;
}

/* ================================================================
 * main
 * ================================================================ */
int main(int argc, char *argv[]) {
    server_config_t config;

    /* --- help --- */
    if (argc >= 2 && strcmp(argv[1], "help") == 0) {
        print_help(argv[0]);
        return 0;
    }

    /* --- load <csv_path> (interactive mode) --- */
    if (argc == 3 && strcmp(argv[1], "load") == 0) {
        return run_interactive(argv[2]);
    }

    /* --- standalone commands (known subcommands only) --- */
    if (argc >= 2 &&
        (strcmp(argv[1], "findusr") == 0 ||
         strcmp(argv[1], "addusr")  == 0 ||
         strcmp(argv[1], "delusr")  == 0 ||
         strcmp(argv[1], "users")   == 0)) {
        return dispatch(argc - 1, argv + 1, 0);
    }

    /* --- process mode (multi-process request handler) --- */
    if (argc == 2 && strcmp(argv[1], "process") == 0) {
        if (log_init("logs/server.log") != 0) {
            printf("failed to open log file\n");
            return 1;
        }

        log_info("========================================");
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "  Parent PID: %d", (int)getpid());
            log_info(buf);
        }
        log_info("========================================");

        if (user_store_load_csv("data/users.csv") < 0) {
            printf("error: cannot open data/users.csv\n");
            log_close();
            return 1;
        }

        int ret = process_server_run();

        log_close();
        user_store_free();
        return ret;
    }

    /* --- fork mode (multi-process TCP/HTTP server) --- */
    if (argc == 2 && strcmp(argv[1], "fork") == 0) {
        if (log_init("logs/server.log") != 0) {
            printf("failed to open log file\n");
            return 1;
        }

        log_info("========================================");
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "  Parent PID: %d", (int)getpid());
            log_info(buf);
        }
        log_info("========================================");

        if (user_store_load_csv("data/users.csv") < 0) {
            printf("error: cannot open data/users.csv\n");
            log_close();
            return 1;
        }

        int ret = tcp_fork_server_run();

        log_close();
        user_store_free();
        return ret;
    }

    /* --- server mode (config file) --- */
    if (argc != 2) {
        print_help(argv[0]);
        return 1;
    }

    memset(&config, 0, sizeof(config));

    if (load_config(argv[1], &config) != 0) {
        printf("failed to load config\n");
        return 1;
    }

    if (log_init(config.log_path) != 0) {
        printf("failed to open log file\n");
        return 1;
    }

    log_info("server config loaded");
    log_info("document root loaded");

    if (user_store_load_csv(config.data_path) < 0) {
        printf("error: cannot open '%s'\n", config.data_path);
        log_close();
        return 1;
    }

    print_config(&config);

    /* run the TCP server — handles one connection then exits */
    if (tcp_server_run(&config) != 0) {
        log_error("tcp server exited with error");
        log_close();
        user_store_free();
        return 1;
    }

    log_close();
    user_store_free();
    return 0;
}
