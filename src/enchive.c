#include "../config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#ifdef _WIN32
#include "w32-compat.h"
#else
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/socket.h>
#endif

#include "docs.h"
#include "sha256.h"
#include "chacha.h"
#include "optparse.h"

int curve25519_donna(u8 *p, const u8 *s, const u8 *b);

/* Global options. */
static char *global_pubkey = 0;
static char *global_seckey = 0;

#if ENCHIVE_AGENT_DEFAULT_ENABLED
static int global_agent_timeout = ENCHIVE_AGENT_TIMEOUT;
#else
static int global_agent_timeout = 0;
#endif

static const char enchive_suffix[] = ".enchive";

static const char *cleanup_pubfile;
static const char *cleanup_secfile;
static const char *cleanup_outfile;

/**
 * Print a message, cleanup, and exit the program with a failure code.
 */
static void
fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "enchive: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    if (cleanup_pubfile)
        remove(cleanup_pubfile);
    if (cleanup_secfile)
        remove(cleanup_secfile);
    if (cleanup_outfile)
        remove(cleanup_outfile);
    va_end(ap);
    exit(EXIT_FAILURE);
}

/**
 * Print a non-fatal warning message.
 */
static void
warning(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "warning: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/**
 * Return a copy of S, which may be NULL.
 * Abort the program if out of memory.
 */
static char *
dupstr(const char *s)
{
    char *copy = 0;
    if (s) {
        size_t len = strlen(s) + 1;
        copy = malloc(len);
        if (!copy)
            fatal("out of memory");
        memcpy(copy, s, len);
    }
    return copy;
}

/**
 * Concatenate N strings as a new string.
 * Abort the program if out of memory.
 */
static char *
joinstr(int n, ...)
{
    int i;
    va_list ap;
    char *p, *str;
    size_t len = 1;

    va_start(ap, n);
    for (i = 0; i < n; i++) {
        char *s = va_arg(ap, char *);
        len += strlen(s);
    }
    va_end(ap);

    p = str = malloc(len);
    if (!str)
        fatal("out of memory");

    va_start(ap, n);
    for (i = 0; i < n; i++) {
        char *s = va_arg(ap, char *);
        size_t slen = strlen(s);
        memcpy(p, s, slen);
        p += slen;
    }
    va_end(ap);

    *p = 0;
    return str;
}

static ssize_t
full_read(int fd, void *buf, size_t len)
{
    size_t z = 0;
    while (z < len) {
        ssize_t r = read(fd, (char *)buf + z, len - z);
        if (r == -1)
            return -1;
        if (r == 0)
            break;
        z += r;
    }
    return z;
}

/**
 * Read the protection key from a key agent identified by its IV.
 */
static int agent_read(u8 *key, const u8 *id);

/**
 * Serve the protection key on a key agent identified by its IV.
 */
static int agent_run(const u8 *key, const u8 *id);

#if ENCHIVE_OPTION_AGENT

/**
 * Fill ADDR with a unix domain socket name for the agent.
 */
static int
agent_addr(struct sockaddr_un *addr, const u8 *iv)
{
    char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir) {
        dir = getenv("TMPDIR");
        if (!dir)
            dir = "/tmp";
    }

    addr->sun_family = AF_UNIX;
    if (strlen(dir) + 1 + 16 + 1 > sizeof(addr->sun_path)) {
        warning("agent socket path too long -- %s", dir);
        return 0;
    } else {
        sprintf(addr->sun_path, "%s/%02x%02x%02x%02x%02x%02x%02x%02x", dir,
                iv[0], iv[1], iv[2], iv[3], iv[4], iv[5], iv[6], iv[7]);
        return 1;
    }
}

static int
agent_read(u8 *key, const u8 *iv)
{
    int success;
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (!agent_addr(&addr, iv)) {
        close(fd);
        return 0;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        close(fd);
        return 0;
    }
    success = read(fd, key, 32) == 32;
    close(fd);
    return success;
}

static int
agent_run(const u8 *key, const u8 *iv)
{
    struct pollfd pfd = {-1, POLLIN, 0};
    struct sockaddr_un addr;
    pid_t pid;

    pfd.fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (pfd.fd == -1) {
        warning("could not create agent socket");
        return 0;
    }

    if (!agent_addr(&addr, iv))
        return 0;

    pid = fork();
    if (pid == -1) {
        warning("could not fork() agent -- %s", strerror(errno));
        return 0;
    } else if (pid != 0) {
        return 1;
    }
    close(0);
    close(1);

    umask(~(S_IRUSR | S_IWUSR));

    if (unlink(addr.sun_path))
        if (errno != ENOENT)
            fatal("failed to remove existing socket -- %s", strerror(errno));

    if (bind(pfd.fd, (struct sockaddr *)&addr, sizeof(addr))) {
        if (errno != EADDRINUSE)
            warning("could not bind agent socket %s -- %s",
                    addr.sun_path, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (listen(pfd.fd, SOMAXCONN)) {
        if (errno != EADDRINUSE)
            fatal("could not listen on agent socket -- %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    close(2);
    for (;;) {
        int cfd;
        int r = poll(&pfd, 1, global_agent_timeout * 1000);
        if (r < 0) {
            unlink(addr.sun_path);
            fatal("agent poll failed -- %s", strerror(errno));
        }
        if (r == 0) {
            unlink(addr.sun_path);
            fputs("info: agent timeout\n", stderr);
            close(pfd.fd);
            break;
        }
        cfd = accept(pfd.fd, 0, 0);
        if (cfd != -1) {
            if (write(cfd, key, 32) != 32)
                warning("agent write failed");
            close(cfd);
        }
    }
    exit(EXIT_SUCCESS);
}

#else
static int
agent_read(u8 *key, const u8 *id)
{
    (void)key;
    (void)id;
    (void)warning;
    return 0;
}

static int
agent_run(const u8 *key, const u8 *id)
{
    (void)key;
    (void)id;
    return 0;
}
#endif

/**
 * Prepend the system user config directory to a filename, creating
 * the directory if necessary. Calls fatal() on any error.
 */
static char *storage_directory(char *file);

#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/**
 * Return non-zero if path exists and is a directory.
 */
static int
dir_exists(const char *path)
{
    struct stat info;
    return !stat(path, &info) && S_ISDIR(info.st_mode);
}

/* Use $XDG_CONFIG_HOME/enchive, or $HOME/.config/enchive. */
static char *
storage_directory(char *file)
{
    static const char enchive[] = "/enchive/";
    static const char config[] = "/.config";
    char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    char *path, *s;

    if (!xdg_config_home) {
        char *home = getenv("HOME");
        if (!home)
            fatal("no $HOME or $XDG_CONFIG_HOME, giving up");
        if (home[0] != '/')
            fatal("$HOME is not absolute");
        path = joinstr(4, home, config, enchive, file);
    } else {
        if (xdg_config_home[0] != '/')
            fatal("$XDG_CONFIG_HOME is not absolute");
        path = joinstr(3, xdg_config_home, enchive, file);
    }

    s = strchr(path + 1, '/');
    while (s) {
        *s = 0;
        if (dir_exists(path) || !mkdir(path, 0700)) {
            DIR *dir = opendir(path);
            if (dir)
                closedir(dir);
            else
                fatal("opendir(%s) -- %s", path, strerror(errno));
        } else {
            fatal("mkdir(%s) -- %s", path, strerror(errno));
        }
        *s = '/';
        s = strchr(s + 1, '/');
    }

    return path;
}

#elif defined(_WIN32)
#include <windows.h>

/* Use %APPDATA% */
static char *
storage_directory(char *file)
{
    char *parent;
    static const char enchive[] = "\\enchive\\";
    char *appdata = getenv("APPDATA");
    if (!appdata)
        fatal("$APPDATA is unset");

    parent = joinstr(2, appdata, enchive);
    if (!CreateDirectory(parent, 0)) {
        if (GetLastError() == ERROR_PATH_NOT_FOUND) {
            fatal("$APPDATA directory doesn't exist");
        } else { /* ERROR_ALREADY_EXISTS */
            DWORD attr = GetFileAttributes(parent);
            if ((attr == INVALID_FILE_ATTRIBUTES) ||
                !(attr & FILE_ATTRIBUTE_DIRECTORY))
                fatal("%s is not a directory", parent);
        }
    }
    free(parent);

    return joinstr(3, appdata, enchive, file);
}

#endif /* _WIN32 */

/**
 * Read a passphrase directly from the keyboard without echo.
 */
static void
get_passphrase(char *buf, size_t len, char *prompt)
{
    struct termios old, new;
    ssize_t z;
    int tty;

    tty = open("/dev/tty", O_RDWR);
    if (tty == -1)
        fatal("could not open /dev/tty -- %s", strerror(errno));

    if (write(tty, prompt, strlen(prompt)) != (ssize_t)strlen(prompt))
        fatal("could not write to /dev/tty -- %s", strerror(errno));

    if (tcgetattr(tty, &old) == -1)
        fatal("tcgetattr() -- %s", strerror(errno));
    new = old;
    new.c_lflag &= ~ECHO;
    if (tcsetattr(tty, TCSANOW, &new) == -1)
        fatal("tcsetattr() -- %s", strerror(errno));

    z = read(tty, buf, len);
    (void)tcsetattr(tty, TCSANOW, &old);  /* don't care if this fails */
    (void)write(tty, "\n", 1);            /* don't care if this fails */

    if (z == -1) {
        fatal("error readin /dev/tty -- %s", strerror(errno));
    } else if (z == 0) {
        buf[z] = 0;
    } else {
        char *end;
        buf[z] = 0;
        if ((end = strchr(buf, '\r')))
            *end = 0;
        else if ((end = strchr(buf, '\n')))
            *end = 0;
    }
    close(tty);
}

/**
 * Initialize a SHA-256 context for HMAC-SHA256.
 * All message data will go into the resulting context.
 */
static void
hmac_init(SHA256_CTX *ctx, const u8 *key)
{
    int i;
    u8 pad[SHA256_BLOCK_SIZE];
    sha256_init(ctx);
    for (i = 0; i < SHA256_BLOCK_SIZE; i++)
        pad[i] = key[i] ^ 0x36U;
    sha256_update(ctx, pad, sizeof(pad));
}

/**
 * Compute the final HMAC-SHA256 MAC.
 * The key must be the same as used for initialization.
 */
static void
hmac_final(SHA256_CTX *ctx, const u8 *key, u8 *hash)
{
    int i;
    u8 pad[SHA256_BLOCK_SIZE];
    sha256_final(ctx, hash);
    sha256_init(ctx);
    for (i = 0; i < SHA256_BLOCK_SIZE; i++)
        pad[i] = key[i] ^ 0x5cU;
    sha256_update(ctx, pad, sizeof(pad));
    sha256_update(ctx, hash, SHA256_BLOCK_SIZE);
    sha256_final(ctx, hash);
}

/**
 * Derive a 32-byte key from null-terminated passphrase into buf.
 * Optionally provide an 8-byte salt.
 */
static void
key_derive(const char *passphrase, u8 *buf, int iexp, const u8 *salt)
{
    u8 salt32[SHA256_BLOCK_SIZE] = {0};
    SHA256_CTX ctx[1];
    unsigned long i;
    unsigned long memlen = 1UL << iexp;
    unsigned long mask = memlen - 1;
    unsigned long iterations = 1UL << (iexp - 5);
    u8 *memory, *memptr, *p;

    memory = malloc(memlen + SHA256_BLOCK_SIZE);
    if (!memory)
        fatal("not enough memory for key derivation");

    if (salt)
        memcpy(salt32, salt, 8);
    hmac_init(ctx, salt32);
    sha256_update(ctx, (u8 *)passphrase, strlen(passphrase));
    hmac_final(ctx, salt32, memory);

    for (p = memory + SHA256_BLOCK_SIZE;
         p < memory + memlen + SHA256_BLOCK_SIZE;
         p += SHA256_BLOCK_SIZE) {
        sha256_init(ctx);
        sha256_update(ctx, p - SHA256_BLOCK_SIZE, SHA256_BLOCK_SIZE);
        sha256_final(ctx, p);
    }

    memptr = memory + memlen - SHA256_BLOCK_SIZE;
    for (i = 0; i < iterations; i++) {
        unsigned long offset;
        sha256_init(ctx);
        sha256_update(ctx, memptr, SHA256_BLOCK_SIZE);
        sha256_final(ctx, memptr);
        offset = ((unsigned long)memptr[3] << 24 |
                  (unsigned long)memptr[2] << 16 |
                  (unsigned long)memptr[1] <<  8 |
                  (unsigned long)memptr[0] <<  0);
        memptr = memory + (offset & mask);
    }

    memcpy(buf, memptr, SHA256_BLOCK_SIZE);
    free(memory);
}

/**
 * Get secure entropy suitable for key generation from OS.
 * Abort the program if the entropy could not be retrieved.
 */
static void
secure_entropy(void *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd == -1)
        fatal("failed to open %s", "/dev/urandom");
    if (read(fd, buf, len) != (ssize_t)len)
        fatal("failed to gather entropy");
    close(fd);
}

/**
 * Generate a brand new Curve25519 secret key from system entropy.
 */
static void
generate_secret(u8 *s)
{
    secure_entropy(s, 32);
    s[0] &= 248;
    s[31] &= 127;
    s[31] |= 64;
}

/**
 * Generate a Curve25519 public key from a secret key.
 */
static void
compute_public(u8 *p, const u8 *s)
{
    static const u8 b[32] = {9};
    curve25519_donna(p, s, b);
}

/**
 * Compute a shared secret from our secret key and their public key.
 */
static void
compute_shared(u8 *sh, const u8 *s, const u8 *p)
{
    curve25519_donna(sh, s, p);
}

/**
 * Encrypt from file to file using key/iv, aborting on any error.
 */
static void
symmetric_encrypt(int in, int out, const u8 *key, const u8 *iv)
{
    static u8 buffer[2][CHACHA_BLOCKLENGTH * 1024];
    u8 mac[SHA256_BLOCK_SIZE];
    SHA256_CTX hmac[1];
    chacha_ctx ctx[1];

    chacha_keysetup(ctx, key, 256);
    chacha_ivsetup(ctx, iv);
    hmac_init(hmac, key);

    for (;;) {
        ssize_t z = full_read(in, buffer[0], sizeof(buffer[0]));
        if (z < 0)
            fatal("error reading plaintext file -- %s", strerror(errno));
        if (z == 0)
            break;
        sha256_update(hmac, buffer[0], z);
        chacha_encrypt_bytes(ctx, buffer[0], buffer[1], z);
        if (write(out, buffer[1], z) != z)
            fatal("error writing ciphertext file -- %s", strerror(errno));
        if (z < (ssize_t)sizeof(buffer[0]))
            break;
    }

    hmac_final(hmac, key, mac);

    if (write(out, mac, sizeof(mac)) != sizeof(mac))
        fatal("error writing checksum to ciphertext file -- %s",
              strerror(errno));
}

/**
 * Decrypt from file to file using key/iv, aborting on any error.
 */
static void
symmetric_decrypt(int in, int out, const u8 *key, const u8 *iv)
{
    ssize_t r;
    static u8 buffer[2][CHACHA_BLOCKLENGTH * 1024 + SHA256_BLOCK_SIZE];
    u8 mac[SHA256_BLOCK_SIZE];
    SHA256_CTX hmac[1];
    chacha_ctx ctx[1];

    chacha_keysetup(ctx, key, 256);
    chacha_ivsetup(ctx, iv);
    hmac_init(hmac, key);

    /* Always keep SHA256_BLOCK_SIZE bytes in the buffer. */
    r = full_read(in, buffer[0], SHA256_BLOCK_SIZE);
    if (r < 0)
        fatal("cannot read ciphertext file -- %s", strerror(errno));
    if (r != SHA256_BLOCK_SIZE)
        fatal("ciphertext file too short");

    for (;;) {
        u8 *p = buffer[0] + SHA256_BLOCK_SIZE;
        ssize_t z = full_read(in, p, sizeof(buffer[0]) - SHA256_BLOCK_SIZE);
        if (z < 0)
            fatal("error reading ciphertext file");
        else if (z == 0)
            break;
        chacha_encrypt_bytes(ctx, buffer[0], buffer[1], z);
        sha256_update(hmac, buffer[1], z);
        if (write(out, buffer[1], z) != z)
            fatal("error writing plaintext file -- %s", strerror(errno));

        /* Move last SHA256_BLOCK_SIZE bytes to the front. */
        memmove(buffer[0], buffer[0] + z, SHA256_BLOCK_SIZE);

        if (z < (ssize_t)sizeof(buffer[0]) - SHA256_BLOCK_SIZE)
            break;
    }

    hmac_final(hmac, key, mac);
    if (memcmp(buffer[0], mac, sizeof(mac)) != 0)
        fatal("checksum mismatch!");
}

/**
 * Return the default public key file.
 */
static char *
default_pubfile(void)
{
    return storage_directory("enchive.pub");
}

/**
 * Return the default secret key file.
 */
static char *
default_secfile(void)
{
    return storage_directory("enchive.sec");
}

/**
 * Dump the public key to a file, aborting on error.
 */
static void
write_pubkey(char *file, u8 *key)
{
    int fd = open(file, O_CREAT | O_WRONLY, 0600);
    if (fd == -1)
        fatal("failed to open key file for writing '%s' -- %s",
              file, strerror(errno));
    cleanup_pubfile = file;
    if (write(fd, key, 32) != 32)
        fatal("failed to write key file '%s'", file);
    close(fd);
}

/* Layout of secret key file */
#define SECFILE_IV            0
#define SECFILE_ITERATIONS    8
#define SECFILE_VERSION       9
#define SECFILE_PROTECT_HASH  12
#define SECFILE_SECKEY        32

/**
 * Write the secret key to a file, encrypting it if necessary.
 */
static void
write_seckey(char *file, const u8 *seckey, int iexp)
{
    int secfd;
    chacha_ctx cha[1];
    SHA256_CTX sha[1];
    u8 buf[8 + 1 + 3 + 20 + 32] = {0}; /* entire file contents */
    u8 protect[32];

    u8 *buf_iv           = buf + SECFILE_IV;
    u8 *buf_iterations   = buf + SECFILE_ITERATIONS;
    u8 *buf_version      = buf + SECFILE_VERSION;
    u8 *buf_protect_hash = buf + SECFILE_PROTECT_HASH;
    u8 *buf_seckey       = buf + SECFILE_SECKEY;

    buf_version[0] = ENCHIVE_FORMAT_VERSION;

    if (iexp) {
        /* Prompt for a passphrase. */
        char pass[2][ENCHIVE_PASSPHRASE_MAX];
        get_passphrase(pass[0], sizeof(pass[0]),
                       "passphrase (empty for none): ");
        if (!pass[0][0]) {
            /* Nevermind. */
            iexp = 0;
        }  else {
            get_passphrase(pass[1], sizeof(pass[0]),
                           "passphrase (repeat): ");
            if (strcmp(pass[0], pass[1]) != 0)
                fatal("passphrases don't match");

            /* Generate an IV to double as salt. */
            secure_entropy(buf_iv, 8);

            key_derive(pass[0], protect, iexp, buf_iv);
            buf_iterations[0] = iexp;

            sha256_init(sha);
            sha256_update(sha, protect, sizeof(protect));
            sha256_final(sha, buf_protect_hash);
        }
    }

    if (iexp) {
        /* Encrypt using key derived from passphrase. */
        chacha_keysetup(cha, protect, 256);
        chacha_ivsetup(cha, buf_iv);
        chacha_encrypt_bytes(cha, seckey, buf_seckey, 32);
    } else {
        /* Copy key to output buffer. */
        memcpy(buf_seckey, seckey, 32);
    }

    secfd = open(file, O_CREAT | O_WRONLY, 0600);
    if (secfd == -1)
        fatal("failed to open key file for writing '%s'", file);
    cleanup_secfile = file;
    if (write(secfd, buf, sizeof(buf)) != sizeof(buf))
        fatal("failed to write key file '%s'", file);
    close(secfd);
}

/**
 * Load the public key from the file.
 */
static void
load_pubkey(const char *file, u8 *key)
{
    FILE *f = fopen(file, "rb");
    if (!f)
        fatal("failed to open key file for reading '%s' -- %s",
              file, strerror(errno));
    if (!fread(key, 32, 1, f))
        fatal("failed to read key file '%s'", file);
    fclose(f);
}

/**
 * Attempt to load and decrypt the secret key stored in a file.
 *
 * If the key is encrypted, attempt to query a key agent. If that
 * fails (no agent, bad key) prompt the user for a passphrase. If that
 * fails (wrong passphrase), abort the program.
 *
 * If "global_agent_timeout" is non-zero, start a key agent if
 * necessary.
 */
static void
load_seckey(const char *file, u8 *seckey)
{
    FILE *secfile;
    chacha_ctx cha[1];
    SHA256_CTX sha[1];
    u8 buf[8 + 4 + 20 + 32];            /* entire key file contents */
    u8 protect[32];                     /* protection key */
    u8 protect_hash[SHA256_BLOCK_SIZE]; /* hash of protection key */
    int iexp;
    int version;

    u8 *buf_iv           = buf + SECFILE_IV;
    u8 *buf_iterations   = buf + SECFILE_ITERATIONS;
    u8 *buf_version      = buf + SECFILE_VERSION;
    u8 *buf_protect_hash = buf + SECFILE_PROTECT_HASH;
    u8 *buf_seckey       = buf + SECFILE_SECKEY;

    /* Read the entire file into buf. */
    secfile = fopen(file, "rb");
    if (!secfile)
        fatal("failed to open key file for reading '%s' -- %s",
              file, strerror(errno));
    if (!fread(buf, sizeof(buf), 1, secfile))
        fatal("failed to read key file -- %s", file);
    fclose(secfile);

    version = buf_version[0];
    if (version != ENCHIVE_FORMAT_VERSION)
        fatal("secret key version mismatch -- expected %d, got %d",
              ENCHIVE_FORMAT_VERSION, version);

    iexp = buf_iterations[0];
    if (iexp) {
        /* Secret key is encrypted. */
        int agent_success = agent_read(protect, buf_iv);
        if (agent_success) {
            /* Check validity of agent key. */
            sha256_init(sha);
            sha256_update(sha, protect, 32);
            sha256_final(sha, protect_hash);
            agent_success = !memcmp(protect_hash, buf_protect_hash, 20);
        }

        if (!agent_success) {
            /* Ask user for passphrase. */
            char pass[ENCHIVE_PASSPHRASE_MAX];
            get_passphrase(pass, sizeof(pass), "passphrase: ");
            key_derive(pass, protect, iexp, buf_iv);

            /* Validate passphrase. */
            sha256_init(sha);
            sha256_update(sha, protect, sizeof(protect));
            sha256_final(sha, protect_hash);
            if (memcmp(protect_hash, buf_protect_hash, 20) != 0)
                fatal("wrong passphrase");
        }

        /* We have the correct protection key. Start the agent? */
        if (!agent_success && global_agent_timeout)
            agent_run(protect, buf_iv);

        /* Decrypt the key into the output. */
        chacha_keysetup(cha, protect, 256);
        chacha_ivsetup(cha, buf_iv);
        chacha_encrypt_bytes(cha, buf_seckey, seckey, 32);
    } else {
        /* Key is unencrypted, copy into output. */
        memcpy(seckey, buf_seckey, 32);
    }
}

/**
 * Return 1 if file exists, or 0 if it doesn't.
 */
static int
file_exists(char *filename)
{
    FILE *f = fopen(filename, "r");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

/**
 * Print a nice fingerprint of a key.
 */
static void
print_fingerprint(const u8 *key)
{
    int i;
    u8 hash[32];
    SHA256_CTX sha[1];

    sha256_init(sha);
    sha256_update(sha, key, 32);
    sha256_final(sha, hash);
    for (i = 0; i < 16; i += 4) {
        unsigned long chunk =
            ((unsigned long)hash[i + 0] << 24) |
            ((unsigned long)hash[i + 1] << 16) |
            ((unsigned long)hash[i + 2] <<  8) |
            ((unsigned long)hash[i + 3] <<  0);
        printf("%s%08lx", i ? "-" : "", chunk);
    }
}

enum command {
    COMMAND_UNKNOWN = -2,
    COMMAND_AMBIGUOUS = -1,
    COMMAND_KEYGEN,
    COMMAND_FINGERPRINT,
    COMMAND_ARCHIVE,
    COMMAND_EXTRACT
};

static const char command_names[][12] = {
    "keygen", "fingerprint", "archive", "extract"
};

/**
 * Attempt to unambiguously parse the user's command into an enum.
 */
static enum command
parse_command(char *command)
{
    int found = COMMAND_UNKNOWN;
    size_t len = strlen(command);
    int i;
    for (i = 0; i < 5; i++) {
        if (strncmp(command, command_names[i], len) == 0) {
            if (found >= 0)
                return COMMAND_AMBIGUOUS;
            found = i;
        }
    }
    return found;
}

static void
command_keygen(struct optparse *options)
{
    static const struct optparse_long keygen[] = {
        {"derive",      'd', OPTPARSE_OPTIONAL},
        {"edit"  ,      'e', OPTPARSE_NONE},
        {"force",       'f', OPTPARSE_NONE},
        {"fingerprint", 'i', OPTPARSE_NONE},
        {"iterations",  'k', OPTPARSE_REQUIRED},
        {"plain",       'u', OPTPARSE_NONE},
        {0, 0, 0}
    };

    char *pubfile = dupstr(global_pubkey);
    char *secfile = dupstr(global_seckey);
    int pubfile_exists;
    int secfile_exists;
    u8 public[32];
    u8 secret[32];
    int clobber = 0;
    int derive = 0;
    int edit = 0;
    int protect = 1;
    int fingerprint = 0;
    int key_derive_iterations = ENCHIVE_KEY_DERIVE_ITERATIONS;
    int seckey_derive_iterations = ENCHIVE_SECKEY_DERIVE_ITERATIONS;

    int option;
    while ((option = optparse_long(options, keygen, 0)) != -1) {
        switch (option) {
            case 'd': {
                char *p;
                char *arg = options->optarg;
                derive = 1;
                if (arg) {
                    long n;
                    errno = 0;
                    n = strtol(arg, &p, 10);
                    if (errno || *p)
                        fatal("invalid argument -- %s", arg);
                    if (n < 5 || n > 31)
                        fatal("--derive argument must be 5 <= n <= 31 -- %s",
                              arg);
                    seckey_derive_iterations = n;
                }
            } break;
            case 'e':
                edit = 1;
                break;
            case 'f':
                clobber = 1;
                break;
            case 'i':
                fingerprint = 1;
                break;
            case 'k': {
                char *p;
                char *arg = options->optarg;
                long n;
                errno = 0;
                n = strtol(arg, &p, 10);
                if (errno || *p)
                    fatal("invalid argument -- %s", arg);
                if (n < 5 || n > 31)
                    fatal("--iterations argument must be 5 <= n <= 31 -- %s",
                          arg);
                key_derive_iterations = n;
            } break;
            case 'u':
                protect = 0;
                break;
            default:
                fatal("%s", options->errmsg);
        }
    }

    if (edit && derive)
        fatal("--edit and --derive are mutually exclusive");

    if (!pubfile)
        pubfile = default_pubfile();
    pubfile_exists = file_exists(pubfile);
    if (!secfile)
        secfile = default_secfile();
    secfile_exists = file_exists(secfile);

    if (!edit && !clobber) {
        if (pubfile_exists)
            fatal("operation would clobber %s", pubfile);
        if (secfile_exists)
            fatal("operation would clobber %s", secfile);
    }

    if (edit) {
        if (!secfile_exists)
            fatal("cannot edit non-existing file %s", secfile);
        load_seckey(secfile, secret);
    } else if (derive) {
        /* Generate secret key from passphrase. */
        char pass[2][ENCHIVE_PASSPHRASE_MAX];
        get_passphrase(pass[0], sizeof(pass[0]),
                       "secret key passphrase: ");
        get_passphrase(pass[1], sizeof(pass[0]),
                       "secret key passphrase (repeat): ");
        if (strcmp(pass[0], pass[1]) != 0)
            fatal("passphrases don't match");
        key_derive(pass[0], secret, seckey_derive_iterations, 0);
        secret[0] &= 248;
        secret[31] &= 127;
        secret[31] |= 64;
    } else {
        /* Generate secret key from entropy. */
        generate_secret(secret);
    }

    compute_public(public, secret);

    if (fingerprint) {
        fputs("keyid: ", stdout);
        print_fingerprint(public);
        putchar('\n');
    }

    write_seckey(secfile, secret, protect ? key_derive_iterations : 0);
    write_pubkey(pubfile, public);
}

static void
command_fingerprint(struct optparse *options)
{
    static const struct optparse_long fingerprint[] = {
        {0, 0, 0}
    };

    char *pubfile = dupstr(global_pubkey);
    u8 public[32];

    int option;
    while ((option = optparse_long(options, fingerprint, 0)) != -1) {
        switch (option) {
            default:
                fatal("%s", options->errmsg);
        }
    }

    if (!pubfile)
        pubfile = default_pubfile();
    load_pubkey(pubfile, public);
    free(pubfile);

    print_fingerprint(public);
    putchar('\n');
}

static void
command_archive(struct optparse *options)
{
    static const struct optparse_long archive[] = {
        {"delete", 'd', OPTPARSE_NONE},
        {0, 0, 0}
    };

    /* Options */
    char *infile;
    char *outfile;
    int in = STDIN_FILENO;
    int out = STDOUT_FILENO;
    char *pubfile = dupstr(global_pubkey);
    int delete = 0;

    /* Workspace */
    u8 public[32];
    u8 esecret[32];
    u8 epublic[32];
    u8 shared[32];
    u8 iv[SHA256_BLOCK_SIZE];
    SHA256_CTX sha[1];

    int option;
    while ((option = optparse_long(options, archive, 0)) != -1) {
        switch (option) {
            case 'd':
                delete = 1;
                break;
            default:
                fatal("%s", options->errmsg);
        }
    }

    if (!pubfile)
        pubfile = default_pubfile();
    load_pubkey(pubfile, public);
    free(pubfile);

    infile = optparse_arg(options);
    if (infile) {
        in = open(infile, O_RDONLY);
        if (in == -1)
            fatal("could not open input file '%s' -- %s",
                  infile, strerror(errno));
    }

    outfile = dupstr(optparse_arg(options));
    if (!outfile && infile) {
        /* Generate an output filename. */
        outfile = joinstr(2, infile, enchive_suffix);
    }
    if (outfile) {
        out = open(outfile, O_CREAT | O_WRONLY, 0600);
        if (out == -1)
            fatal("could not open output file '%s' -- %s",
                  outfile, strerror(errno));
        cleanup_outfile = outfile;
    }

    /* Generare ephemeral keypair. */
    generate_secret(esecret);
    compute_public(epublic, esecret);

    /* Create shared secret between ephemeral key and master key. */
    compute_shared(shared, esecret, public);
    sha256_init(sha);
    sha256_update(sha, shared, sizeof(shared));
    sha256_final(sha, iv);
    iv[0] += (unsigned)ENCHIVE_FORMAT_VERSION;
    if (write(out, iv, 8) != 8)
        fatal("failed to write IV to archive");
    if (write(out, epublic, sizeof(epublic)) != sizeof(epublic))
        fatal("failed to write ephemeral key to archive");
    symmetric_encrypt(in, out, shared, iv);

    close(in);
    close(out);
    if (delete && infile)
        remove(infile);
}

static void
command_extract(struct optparse *options)
{
    static const struct optparse_long extract[] = {
        {"delete", 'd', OPTPARSE_NONE},
        {0, 0, 0}
    };

    /* Options */
    char *infile;
    char *outfile;
    int in = STDIN_FILENO;
    int out = STDOUT_FILENO;
    char *secfile = dupstr(global_seckey);
    int delete = 0;

    /* Workspace */
    SHA256_CTX sha[1];
    u8 secret[32];
    u8 epublic[32];
    u8 shared[32];
    u8 iv[8];
    u8 check_iv[SHA256_BLOCK_SIZE];

    int option;
    while ((option = optparse_long(options, extract, 0)) != -1) {
        switch (option) {
            case 'd':
                delete = 1;
                break;
            default:
                fatal("%s", options->errmsg);
        }
    }

    if (!secfile)
        secfile = default_secfile();
    load_seckey(secfile, secret);
    free(secfile);

    infile = optparse_arg(options);
    if (infile) {
        in = open(infile, O_RDONLY);
        if (in == -1)
            fatal("could not open input file '%s' -- %s",
                  infile, strerror(errno));
    }

    outfile = dupstr(optparse_arg(options));
    if (!outfile && infile) {
        /* Generate an output filename. */
        size_t slen = sizeof(enchive_suffix) - 1;
        size_t len = strlen(infile);
        if (len <= slen || strcmp(enchive_suffix, infile + len - slen) != 0)
            fatal("could not determine output filename from %s", infile);
        outfile = dupstr(infile);
        outfile[len - slen] = 0;
    }
    if (outfile) {
        out = open(outfile, O_CREAT | O_WRONLY, 0600);
        if (out == -1)
            fatal("could not open output file '%s' -- %s",
                  infile, strerror(errno));
        cleanup_outfile = outfile;
    }

    if (full_read(in, iv, sizeof(iv)) != sizeof(iv))
        fatal("failed to read IV from archive -- %s", strerror(errno));
    if (full_read(in, epublic, sizeof(epublic)) != sizeof(epublic))
        fatal("failed to read ephemeral key from archive -- %s",
              strerror(errno));
    compute_shared(shared, secret, epublic);

    /* Validate key before processing the file. */
    sha256_init(sha);
    sha256_update(sha, shared, sizeof(shared));
    sha256_final(sha, check_iv);
    check_iv[0] += (unsigned)ENCHIVE_FORMAT_VERSION;
    if (memcmp(iv, check_iv, sizeof(iv)) != 0)
        fatal("invalid master key or format");

    symmetric_decrypt(in, out, shared, iv);

    close(in);
    close(out);
    if (delete && infile)
        remove(infile);
}

/**
 * Write a NULL-terminated array of strings with a newline after each.
 */
static void
multiputs(const char **s, FILE *f)
{
    while (*s) {
        fputs(*s++, f);
        fputc('\n', f);
    }
}

static void
print_usage(FILE *f)
{
    multiputs(docs_usage, f);
}

static void
print_version(void)
{
    puts("enchive " STR(ENCHIVE_VERSION));
}

int
main(int argc, char **argv)
{
    static const struct optparse_long global[] = {
#if ENCHIVE_OPTION_AGENT
        {"agent",         'a', OPTPARSE_OPTIONAL},
        {"no-agent",      'A', OPTPARSE_NONE},
#endif
        {"pubkey",        'p', OPTPARSE_REQUIRED},
        {"seckey",        's', OPTPARSE_REQUIRED},
        {"version",       'V', OPTPARSE_NONE},
        {"help",          'h', OPTPARSE_NONE},
        {0, 0, 0}
    };

    int option;
    char *command;
    struct optparse options[1];
    optparse_init(options, argv);
    options->permute = 0;
    (void)argc;

    while ((option = optparse_long(options, global, 0)) != -1) {
        switch (option) {
#if ENCHIVE_OPTION_AGENT
            case 'a':
                if (options->optarg) {
                    char *arg = options->optarg;
                    char *endptr;
                    errno = 0;
                    global_agent_timeout = strtol(arg, &endptr, 10);
                    if (*endptr || errno)
                        fatal("invalid --agent argument -- %s", arg);
                } else
                    global_agent_timeout = ENCHIVE_AGENT_TIMEOUT;
                break;
            case 'A':
                global_agent_timeout = 0;
                break;
#endif
            case 'p':
                global_pubkey = options->optarg;
                break;
            case 's':
                global_seckey = options->optarg;
                break;
            case 'h':
                print_usage(stdout);
                exit(EXIT_SUCCESS);
                break;
            case 'V':
                print_version();
                exit(EXIT_SUCCESS);
                break;
            default:
                fatal("%s", options->errmsg);
        }
    }

    command = optparse_arg(options);
    options->permute = 1;
    if (!command) {
        fprintf(stderr, "enchive: missing command\n");
        print_usage(stderr);
        exit(EXIT_FAILURE);
    }

    switch (parse_command(command)) {
        case COMMAND_UNKNOWN:
        case COMMAND_AMBIGUOUS:
            fprintf(stderr, "enchive: unknown command, %s\n", command);
            print_usage(stderr);
            exit(EXIT_FAILURE);
            break;
        case COMMAND_KEYGEN:
            command_keygen(options);
            break;
        case COMMAND_FINGERPRINT:
            command_fingerprint(options);
            break;
        case COMMAND_ARCHIVE:
            command_archive(options);
            break;
        case COMMAND_EXTRACT:
            command_extract(options);
            break;
    }

    return 0;
}
