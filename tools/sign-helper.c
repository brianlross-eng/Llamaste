// sign-helper.c — Ed25519 file signer for Llamaste update packages
//
// Signs the contents of a file with an Ed25519 secret key and outputs
// the 64-byte detached signature as hex to stdout.
//
// Build:
//   gcc -o sign-helper sign-helper.c ../src/llamaste/tweetnacl.c -I../src/llamaste
//
// Usage:
//   ./sign-helper <secret-key-file> <file-to-sign>
//   # Outputs: 128 hex characters (64-byte signature) followed by newline

#include "tweetnacl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Dummy randombytes — not needed for signing, but TweetNaCl requires the symbol
void randombytes(unsigned char *x, unsigned long long xlen) {
    (void)x; (void)xlen;
}

static unsigned char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <secret-key-file> <file-to-sign>\n", argv[0]);
        fprintf(stderr, "\nOutputs 128 hex chars (64-byte Ed25519 detached signature)\n");
        return 1;
    }

    // Read secret key (must be exactly 64 bytes)
    size_t sk_len = 0;
    unsigned char *sk = read_file(argv[1], &sk_len);
    if (!sk) { fprintf(stderr, "error: cannot read secret key: %s\n", argv[1]); return 1; }
    if (sk_len != 64) {
        fprintf(stderr, "error: secret key must be 64 bytes, got %zu\n", sk_len);
        free(sk); return 1;
    }

    // Read file to sign
    size_t msg_len = 0;
    unsigned char *msg = read_file(argv[2], &msg_len);
    if (!msg) { fprintf(stderr, "error: cannot read file: %s\n", argv[2]); free(sk); return 1; }

    // Sign
    unsigned char sig[64];
    if (crypto_sign_ed25519_sign_detached(sig, msg, (unsigned long long)msg_len, sk) != 0) {
        fprintf(stderr, "error: signing failed\n");
        free(sk); free(msg); return 1;
    }

    // Output signature as hex
    for (int i = 0; i < 64; i++) {
        printf("%02x", sig[i]);
    }
    printf("\n");

    // Scrub secret key from memory
    memset(sk, 0, 64);
    free(sk);
    free(msg);
    return 0;
}
