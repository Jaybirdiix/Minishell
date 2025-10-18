#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>        // For file control options
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>     // For waitpid
#include <unistd.h>

/* ======================================================================
 * Mini shell
 * - Keeps the functionality of the new implementation (tokenizer/parser).
 * - Restores the helpful comments from the original file, adapted to the
 *   new structure and names.
 * ====================================================================== */


/* ----------------------- Global last status -----------------------
 * Track last status internally instead of relying on getenv("?").
 * Mirrors the original idea of `set_status()` but uses a private variable.
 */
static int g_last_status = 0;

/* Optional: expose last status for debugging; not used for expansion */
static void set_status_env(int status) {
    g_last_status = status;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", status);
    setenv("MINISHELL_LAST_STATUS", buf, 1);
}


/* -------------------- Dir stack (pushd/popd) -------------
 * Equivalent to the original DirNode stack:
 *  - pushd saves the current directory on a stack and chdirs to the new one
 *  - popd pops and chdirs back
 */

typedef struct DirNode {
    char *directory;
    struct DirNode *next;
} DirNode;

static DirNode *dir_stack = NULL; // Initialize the stack to NULL

/* Print the current directory followed by the stack, like `dirs` */
static void print_dir_stack(void) {
    // Fetch the current working directory
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return; }

    // Print the current directory
    printf("%s", cwd);

    // Iterate through the directory stack and print each directory
    for (DirNode *n = dir_stack; n; n = n->next) printf(" %s", n->directory);

    // Print a newline character at the end
    printf("\n");
    fflush(stdout); // Ensure the output is flushed immediately
}

static int builtin_pushd(char *dir) {
    if (!dir) {
        fprintf(stderr, "pushd: usage: pushd directory\n");
        return 1; // Incorrect usage
    }

    // Save the current directory
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) { perror("pushd:getcwd"); return errno; }

    // Push the current directory onto the stack before changing to the new one
    DirNode *node = malloc(sizeof(*node));
    if (!node) { perror("pushd:malloc"); return 1; }
    node->directory = strdup(cwd);
    if (!node->directory) { perror("pushd:strdup"); free(node); return 1; }
    node->next = dir_stack;
    dir_stack = node;

    // Change to the new directory
    if (chdir(dir) != 0) { perror("pushd:chdir"); return errno; }

    // Print the directory stack
    print_dir_stack();
    return 0;
}

static int builtin_popd(void) {
    if (!dir_stack) {
        fprintf(stderr, "popd: directory stack empty\n");
        return 1; // Stack is empty
    }

    DirNode *node = dir_stack;
    dir_stack = node->next;

    int rc = 0;
    if (chdir(node->directory) != 0) { perror("popd:chdir"); rc = errno; }

    free(node->directory);
    free(node);

    print_dir_stack();
    return rc; // Success or errno from chdir
}

static void free_dir_stack(void) {
    // Free all nodes; set stack to NULL
    DirNode *n = dir_stack;
    while (n) {
        DirNode *next = n->next;
        free(n->directory);
        free(n);
        n = next;
    }
    dir_stack = NULL;
}


/* ---------------- Variable expansion (simple) -------------
 * Expands $VAR and $?; no braces here.  $? uses g_last_status.
 * Single-quoted $ are preserved literally (see tokenizer).
 */
static char *expand_vars(const char *in) {
    size_t cap = 1024, len = 0;
    char *out = malloc(cap);
    if (!out) { perror("malloc"); exit(1); }
    out[0] = '\0';

    for (const char *p = in; *p; ) {
        if (*p == '$') {
            p++;
            // Special case: $?
            if (*p == '?') {
                char num[16];
                snprintf(num, sizeof(num), "%d", g_last_status);
                size_t need = len + strlen(num) + 1;
                if (need > cap) { cap = need * 2; out = realloc(out, cap); if (!out) { perror("realloc"); exit(1);} }
                strcpy(out + len, num);
                len += strlen(num);
                p++;
                continue;
            }
            // Normal $NAME expansion
            if (*p == '\0' || !(isalpha((unsigned char)*p) || *p == '_')) {
                // Not a valid variable start; keep the '$' literally
                if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); if (!out) { perror("realloc"); exit(1);} }
                out[len++] = '$'; out[len] = '\0';
                continue;
            }
            char name[128]; size_t ni = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && ni < sizeof(name)-1) {
                name[ni++] = *p++;
            }
            name[ni] = '\0';
            const char *val = getenv(name);
            if (!val) val = "";

            size_t need = len + strlen(val) + 1;
            if (need > cap) { cap = need * 2; out = realloc(out, cap); if (!out) { perror("realloc"); exit(1);} }
            strcpy(out + len, val);
            len += strlen(val);
        } else {
            // Copy literal char
            if (len + 2 > cap) { cap *= 2; out = realloc(out, cap); if (!out) { perror("realloc"); exit(1);} }
            out[len++] = *p++;
            out[len] = '\0';
        }
    }
    return out;
}

/* Restore placeholder (0x1F) back to '$' after expansion.
 * Matches original behavior of keeping single-quoted dollars literal.
 */
static void restore_placeholders(char *s) {
    for (char *p = s; *p; ++p) {
        if ((unsigned char)*p == 0x1F) *p = '$';
    }
}


/* --------------------- Tokenizer --------------------------
 * Words can be unquoted, "double-quoted", or 'single-quoted'.
 *  - Double quotes support \n, \t, \", \\ (expansion deferred).
 *  - Single quotes are literal; we mark '$' as 0x1F to prevent expansion.
 *  - Operators: | & < > >> << && ||
 */

typedef struct {
    char **v;
    int n, cap;
} Vec;

static void vec_init(Vec *x){ x->v=NULL; x->n=0; x->cap=0; }
static void vec_push(Vec *x, char *s){
    if (x->n == x->cap) {
        x->cap = x->cap? x->cap*2:16;
        x->v = realloc(x->v, x->cap*sizeof(char*));
        if (!x->v){ perror("realloc"); exit(1);}
    }
    x->v[x->n++] = s;
}

static bool is_op2(char a, char b){
    return (a=='&'&&b=='&')||(a=='|'&&b=='|')||(a=='>'&&b=='>')||(a=='<'&&b=='<');
}

/* String builder helper */
static void sb_putc(char **buf, size_t *len, size_t *cap, char c){
    if (*len + 2 > *cap){
        *cap = (*cap? *cap*2 : 64);
        *buf = realloc(*buf, *cap);
        if(!*buf){perror("realloc"); exit(1);}
    }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = '\0';
}

static char **tokenize(const char *line, int *outc) {
    Vec tok; vec_init(&tok);
    const char *p = line;

    while (*p) {
        // Skip whitespace
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;

        // Two-char operators first
        if (is_op2(p[0], p[1])) {
            char *t = strndup(p, 2);
            vec_push(&tok, t);
            p += 2;
            continue;
        }
        // One-char operators
        if (strchr("|&<>", *p)) {
            char *t = strndup(p, 1);
            vec_push(&tok, t);
            p++;
            continue;
        }

        // Accumulate a word
        char *acc = NULL; size_t len = 0, cap = 0;

        while (*p && !isspace((unsigned char)*p) && !strchr("|&<>", *p) && !is_op2(p[0], p[1])) {
            if (*p == '\'') { /* single-quoted literal */
                p++;
                while (*p && *p != '\'') {
                    char c = *p++;
                    if (c == '$') c = 0x1F; /* placeholder to block expansion */
                    sb_putc(&acc, &len, &cap, c);
                }
                if (*p == '\'') p++; else {
                    fprintf(stderr, "syntax error: unmatched '\\''\n");
                    free(acc);
                    while (*p) p++;   // drain
                    goto finish_line;
                }
                continue;
            }
            if (*p == '\"') { /* double-quoted with escapes */
                p++;
                char *dq = NULL; size_t dlen=0, dcap=0;
                while (*p && *p != '\"') {
                    if (*p == '\\') {
                        p++;
                        if (!*p) break;
                        char c = *p++;
                        switch (c) {
                            case 'n':  sb_putc(&dq, &dlen, &dcap, '\n'); break;
                            case 't':  sb_putc(&dq, &dlen, &dcap, '\t'); break;
                            case '\\': sb_putc(&dq, &dlen, &dcap, '\\'); break;
                            case '\"': sb_putc(&dq, &dlen, &dcap, '\"'); break;
                            default:   sb_putc(&dq, &dlen, &dcap, c);    break;
                        }
                    } else {
                        sb_putc(&dq, &dlen, &dcap, *p++);
                    }
                }
                if (*p == '\"') p++; else {
                    fprintf(stderr, "syntax error: unmatched '\"'\n");
                    free(dq); free(acc);
                    while (*p) p++;   // drain
                    goto finish_line;
                }
                if (dq) {
                    for (char *q = dq; *q; q++) sb_putc(&acc, &len, &cap, *q);
                    free(dq);
                }
                continue;
            }

            // Plain character
            sb_putc(&acc, &len, &cap, *p++);
        }

        if (acc == NULL) acc = strdup("");
        vec_push(&tok, acc);
    }

finish_line:
    vec_push(&tok, NULL);
    *outc = tok.n - 1;
    return tok.v;
}


/* --------------------- Parsing nodes ----------------------
 * Grammar (right-binding split like the original pipeline/AND/OR handling):
 *   cmd & cmd & ...     (background chains)
 *   a && b, a || b      (conditionals)
 *   pipelines with |    (left | right)
 *   simple with redirections (<, >, >>, <<)
 */

typedef enum {
    NT_SIMPLE,
    NT_PIPE,
    NT_AND,
    NT_OR,
    NT_BG
} NodeType;

typedef struct Redir {
    char *in_file;      /* < */
    char *out_file;     /* > or >> */
    int   out_append;   /* 1 if >> */
    char *heredoc_data; /* for << */
} Redir;

typedef struct Node {
    NodeType type;
    struct Node *left, *right;
    char **argv; /* for simple */
    Redir redir;
} Node;

static Node *node_new(NodeType t){ Node *n = calloc(1,sizeof(*n)); if(!n){perror("calloc"); exit(1);} n->type=t; return n; }
static void free_argv(char **a){ if(!a) return; for(int i=0;a[i];i++) free(a[i]); free(a); }
static void node_free(Node *n){
    if(!n) return;
    node_free(n->left);
    node_free(n->right);
    free_argv(n->argv);
    free(n->redir.in_file);
    free(n->redir.out_file);
    free(n->redir.heredoc_data);
    free(n);
}

static int find_rightmost(char **toks, const char *sym1, const char *sym2, int start, int end) {
    for (int i=end-1;i>=start;i--) {
        if (!toks[i]) break;
        if (sym2) {
            if ((strcmp(toks[i], sym1)==0) || (strcmp(toks[i], sym2)==0)) return i;
        } else {
            if (strcmp(toks[i], sym1)==0) return i;
        }
    }
    return -1;
}

static struct Node *parse_range(char **toks, int start, int end);

static Node *parse_simple(char **toks, int start, int end) {
    typedef struct { char **v; int n, cap; } Vec2;
    Vec2 argv = {0};
    #define VEC2_PUSH(V, S) do{ if((V).n==(V).cap){ (V).cap = (V).cap? (V).cap*2:8; (V).v = realloc((V).v, (V).cap*sizeof(char*)); if(!(V).v){perror("realloc"); exit(1);} } (V).v[(V).n++] = (S);}while(0)

    Redir r = (Redir){0};
    for (int i=start;i<end;i++) {
        if (strcmp(toks[i], "<") == 0 && i+1 < end) {
            r.in_file = strdup(toks[++i]);                 /* input redirection */
        } else if (strcmp(toks[i], ">") == 0 && i+1 < end) {
            r.out_file = strdup(toks[++i]); r.out_append = 0;  /* truncate */
        } else if (strcmp(toks[i], ">>") == 0 && i+1 < end) {
            r.out_file = strdup(toks[++i]); r.out_append = 1;  /* append */
        } else if (strcmp(toks[i], "<<") == 0) {
            /* Handling RED_IN_HERE (here-document) */
            if (i+1 >= end) { fprintf(stderr, "syntax error: << needs delimiter\n"); goto fail; }
            const char *delim = toks[++i];
            // Create a here-document by reading until the delimiter line
            fprintf(stderr, "(heredoc '%s') End with line containing only %s\n", delim, delim);
            size_t cap = 1024, len = 0;
            char *buf = malloc(cap);
            if (!buf) { perror("malloc"); goto fail; }
            buf[0] = '\0';
            char *line = NULL; size_t n = 0;
            while (1) {
                fprintf(stderr, "> ");
                ssize_t m = getline(&line, &n, stdin);
                if (m < 0) { free(line); break; }
                if (m>0 && line[m-1]=='\n') line[m-1] = '\0';
                if (strcmp(line, delim) == 0) { free(line); break; }
                line[m-1] = '\n';
                if (len + (size_t)m + 1 > cap) {
                    cap = (len+m+1)*2;
                    buf = realloc(buf, cap);
                    if(!buf){perror("realloc"); free(line); goto fail;}
                }
                memcpy(buf+len, line, (size_t)m);
                len += (size_t)m;
                buf[len] = '\0';
            }
            r.heredoc_data = buf;  // Store the here-document content
        } else if (strcmp(toks[i], "&")==0 || strcmp(toks[i],"|")==0 || strcmp(toks[i],"&&")==0 || strcmp(toks[i],"||")==0) {
            fprintf(stderr, "syntax error near '%s'\n", toks[i]); goto fail;
        } else {
            VEC2_PUSH(argv, strdup(toks[i]));
        }
    }
    VEC2_PUSH(argv, NULL);
    Node *n = node_new(NT_SIMPLE);
    n->argv = argv.v;
    n->redir = r;
    return n;
fail:
    for (int j=0;j<argv.n;j++) free(argv.v[j]);
    free(argv.v);
    free(r.in_file);
    free(r.out_file);
    free(r.heredoc_data);
    return NULL;
}

static Node *parse_pipeline(char **toks, int start, int end) {
    int cut = find_rightmost(toks, "|", NULL, start, end);
    if (cut < 0) return parse_simple(toks, start, end);
    Node *left = parse_pipeline(toks, start, cut);
    Node *right = parse_pipeline(toks, cut+1, end);
    if (!left || !right) { node_free(left); node_free(right); return NULL; }
    Node *n = node_new(NT_PIPE);
    n->left = left; n->right = right;
    return n;
}

static Node *parse_and_or(char **toks, int start, int end) {
    int cut = find_rightmost(toks, "&&", "||", start, end);
    if (cut < 0) return parse_pipeline(toks, start, end);
    Node *left = parse_and_or(toks, start, cut);
    Node *right = parse_and_or(toks, cut+1, end);
    if (!left || !right) { node_free(left); node_free(right); return NULL; }
    Node *n = node_new(strcmp(toks[cut], "&&")==0 ? NT_AND : NT_OR);
    n->left = left; n->right = right;
    return n;
}

static Node *parse_range(char **toks, int start, int end) {
    // Background operator splits at the rightmost '&'
    int cut = find_rightmost(toks, "&", NULL, start, end);
    if (cut >= 0) {
        Node *left = parse_range(toks, start, cut);
        Node *right = NULL;
        if (cut+1 < end) right = parse_range(toks, cut+1, end);
        if (!left) { node_free(right); return NULL; }
        Node *n = node_new(NT_BG);
        n->left = left; n->right = right;
        return n;
    }
    return parse_and_or(toks, start, end);
}

static Node *parse(char **toks) {
    int n=0; while (toks[n]) n++;
    if (n==0) return NULL;
    return parse_range(toks, 0, n);
}


/* --------------------- Execution --------------------------
 * setup_redirs:
 *  - Handle input redirection: < and << (here-document)
 *  - Handle output redirection: > (truncate) and >> (append)
 * Matches the structure and comments of the original implementation.
 */

static void setup_redirs(const Redir *r) {
    int fd;
    if (r->in_file) {
        // Expand and open input file
        char *exp = expand_vars(r->in_file);
        for(char *q=exp; *q; ++q) if ((unsigned char)*q==0x1F) *q='$';
        fd = open(exp, O_RDONLY);
        free(exp);
        if (fd < 0) { perror("open <"); _exit(127); }
        if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2 <"); _exit(127); }
        close(fd);
    }
    if (r->heredoc_data) {
        // Handling RED_IN_HERE (here-document):
        // Create a temporary file, write the here-document content to it,
        // reset file offset to the beginning, and redirect stdin from it.
        int fd2;
        char tmp[] = "/tmp/minish_heredoc_XXXXXX";
        fd2 = mkstemp(tmp);
        if (fd2 < 0) { perror("mkstemp"); _exit(127); }

        // Write the here-document content to the temporary file
        ssize_t w = write(fd2, r->heredoc_data, (ssize_t)strlen(r->heredoc_data));
        (void)w;

        // Reset file offset to the beginning
        lseek(fd2, 0, SEEK_SET);

        if (dup2(fd2, STDIN_FILENO) < 0) { perror("dup2 heredoc"); _exit(127); }
        close(fd2);
        unlink(tmp);
    }
    // Handle output redirection
    if (r->out_file) {
        char *exp = expand_vars(r->out_file);
        for(char *q=exp; *q; ++q) if ((unsigned char)*q==0x1F) *q='$';
        int flags = O_WRONLY | O_CREAT | (r->out_append ? O_APPEND : O_TRUNC);
        int fd3 = open(exp, flags, 0644);
        free(exp);
        if (fd3 < 0) { perror("open >"); _exit(127); }
        if (dup2(fd3, STDOUT_FILENO) < 0) { perror("dup2 >"); _exit(127); }
        close(fd3);
    }
}

static int run_node(struct Node *n);

/* Expand argv at execution time, preserving single-quoted dollars.
 * Mirrors original strategy of expanding right before exec/builtin.
 */
static void exec_time_expand_argv(char **argv) {
    for (int i=0; argv[i]; ++i) {
        char *exp = expand_vars(argv[i]);   /* expands $VAR and $? via g_last_status */
        restore_placeholders(exp);          /* turn 0x1F back into '$' */
        free(argv[i]);
        argv[i] = exp;
    }
}

/* Run a simple command or built-in
 * Built-ins implemented here: cd, export, pushd, popd, exit.
 * External commands are execvp'ed in a child after redirections.
 */
static int run_simple(struct Node *n) {
    if (!n->argv || !n->argv[0]) return 0;

    // Execute-time argv expansion so $? holds the most recent command
    exec_time_expand_argv(n->argv);

    // Built-in: cd
    if (strcmp(n->argv[0], "cd") == 0) {
        // If no argument, default to HOME (as in the original)
        const char *dir = n->argv[1] ? n->argv[1] : getenv("HOME");
        if (!dir) { fprintf(stderr, "cd: HOME not set\n"); return 1; }
        if (chdir(dir) != 0) { perror("cd"); return errno; }
        return 0;
    }

    // Built-in: export (expects NAME=VALUE)
    if (strcmp(n->argv[0], "export") == 0) {
        int rc = 0;
        for (int i=1; n->argv[i]; i++) {
            char *eq = strchr(n->argv[i], '=');
            if (!eq) { fprintf(stderr, "export: invalid: %s\n", n->argv[i]); rc = 1; continue; }
            *eq = '\0';
            if (setenv(n->argv[i], eq+1, 1) != 0) { perror("export"); rc = errno; }
        }
        return rc;
    }

    // Built-ins: pushd / popd
    if (strcmp(n->argv[0], "pushd") == 0) return builtin_pushd(n->argv[1]);
    if (strcmp(n->argv[0], "popd") == 0)  return builtin_popd();

    // Built-in: exit
    if (strcmp(n->argv[0], "exit") == 0) {
        int code = 0; if (n->argv[1]) code = atoi(n->argv[1]);
        free_dir_stack();
        exit(code);
    }

    // External command
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return errno; }

    if (pid == 0) { // Child process
        // Reset signal handlers (restore default)
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        // Unblock all signals
        sigset_t empty; sigemptyset(&empty); sigprocmask(SIG_SETMASK, &empty, NULL);

        // Set up redirections
        setup_redirs(&n->redir);

        // Execute the command
        execvp(n->argv[0], n->argv);
        // Handle execvp errors as in the original handle_execvp_error()
        if (errno == ENOENT) fprintf(stderr, "%s: No such file or directory\n", n->argv[0]);
        else if (errno == EACCES) fprintf(stderr, "%s: Permission denied\n", n->argv[0]);
        else if (errno == EISDIR) fprintf(stderr, "%s: Is a directory\n", n->argv[0]);
        else if (errno == ENOEXEC) fprintf(stderr, "%s: Exec format error. Binary file not executable.\n", n->argv[0]);
        else fprintf(stderr, "%s: %s\n", n->argv[0], strerror(errno));
        _exit(127); // Exit child on failure
    }

    // Parent process: wait for the child
    int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st)) { int rc = WEXITSTATUS(st); set_status_env(rc); return rc; }
    if (WIFSIGNALED(st)) { int rc = 128 + WTERMSIG(st); set_status_env(rc); return rc; }
    set_status_env(1);
    return 1;
}

/* Pipe execution.
 * "Counts cmds in a pipe" by running left and right.
 * Returns the rightmost command's status
 */
static int run_pipe(struct Node *n) {
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); return 1; }

    pid_t left_pid = fork();
    if (left_pid < 0) { perror("fork"); close(fds[0]); close(fds[1]); return 1; }
    if (left_pid == 0) {
        // Left child: connect stdout -> pipe write
        signal(SIGINT, SIG_DFL);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        int code = run_node(n->left);
        _exit(code);
    }

    pid_t right_pid = fork();
    if (right_pid < 0) { perror("fork"); close(fds[0]); close(fds[1]); return 1; }
    if (right_pid == 0) {
        // Right child: connect stdin <- pipe read
        signal(SIGINT, SIG_DFL);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]); close(fds[1]);
        int code = run_node(n->right);
        _exit(code);
    }

    // Parent: close pipe ends and wait
    close(fds[0]); close(fds[1]);

    // Wait for all children; track last non-zero / signaled
    int st, last = 0;
    while (waitpid(left_pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st)) last = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) last = 128 + WTERMSIG(st);
    while (waitpid(right_pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st)) last = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) last = 128 + WTERMSIG(st);

    set_status_env(last);
    return last; /* rightmost status */
}

/* Recursively run nodes:
 *  - AND: run right iff left exits 0
 *  - OR:  run right iff left exits non-zero
 *  - BG:  fork the left (background), immediately continue with right
 */
static int run_node(struct Node *n) {
    if (!n) { set_status_env(0); return 0; }
    switch (n->type) {
        case NT_SIMPLE: {
            int rc = run_simple(n);
            set_status_env(rc);
            return rc;
        }
        case NT_PIPE: {
            int rc = run_pipe(n);
            set_status_env(rc);
            return rc;
        }
        case NT_AND: {
            int left_status = run_node(n->left);
            set_status_env(left_status);
            if (left_status == 0) {
                int right_status = run_node(n->right);
                set_status_env(right_status);
                return right_status;
            } else {
                return left_status;
            }
        }
        case NT_OR: {
            int left_status = run_node(n->left);
            set_status_env(left_status);
            if (left_status != 0) {
                int right_status = run_node(n->right);
                set_status_env(right_status);
                return right_status;
            } else {
                return left_status;
            }
        }
        case NT_BG: {
            pid_t pid = fork();

            if (pid < 0) { perror("fork"); set_status_env(1); return 1; }

            if (pid == 0) { // Child process
                // **Ensure standard output and error are unbuffered**
                setvbuf(stdout, NULL, _IONBF, 0);
                setvbuf(stderr, NULL, _IONBF, 0);

                // Reset signals to default
                signal(SIGINT, SIG_DFL);
                signal(SIGTERM, SIG_DFL);

                // Execute the entire command (left) in background
                int code = run_node(n->left);
                _exit(code);
            } else { // Parent process
                // Do not wait for the child
                fprintf(stderr, "Backgrounded: %d\n", pid);
                int rc = n->right ? run_node(n->right) : 0;
                set_status_env(rc);
                return rc;
            }
        }
    }
    set_status_env(1);
    return 1;
}

/* Reap zombie processes.
 * Use WNOHANG to check for any completed child processes.
 * Report completed background processes, matching original messaging.
 */
static void reap_zombies(void) {
    int st; pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        int code = 0;
        if (WIFEXITED(st)) code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) code = 128 + WTERMSIG(st);
        fprintf(stderr, "Completed: %d (%d)\n", p, code);
    }
}


/* ---------------------- REPL ------------------------------
 * Parent ignores SIGINT (like a shell), children restore defaults.
 */
static void install_parent_signals(void) {
    struct sigaction sa = (struct sigaction){0};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, NULL);
}

int main(void) {
    set_status_env(0);
    install_parent_signals();

    char *line = NULL;
    size_t n = 0;

    while (1) {
        // Reap zombies only from the shell process
        reap_zombies();

        // Prompt
        fprintf(stdout, "mini$ ");
        fflush(stdout);

        // Read line
        ssize_t m = getline(&line, &n, stdin);
        if (m < 0) { printf("\n"); break; }
        if (m > 0 && line[m-1] == '\n') line[m-1] = '\0';
        if (line[0] == '\0') continue;

        // Tokenize and parse
        int tokc = 0;
        char **toks = tokenize(line, &tokc);
        if (!toks || tokc == 0) { free(toks); continue; }

        struct Node *root = parse(toks);
        for (int i=0; toks[i]; i++) free(toks[i]);
        free(toks);

        if (!root) { set_status_env(2); continue; }

        // Execute and set last status
        int code = run_node(root);
        set_status_env(code);
        node_free(root);
    }

    free(line);
    free_dir_stack();
    return 0;
}
