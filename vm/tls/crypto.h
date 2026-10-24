// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * What tls.c and crypto.c, the two halves of tls.jdll, share: the certificate readers, and
 * crypto.c's exports, which tls.c lists in the library's one manifest. */
#ifndef JELLO_TLS_CRYPTO_H
#define JELLO_TLS_CRYPTO_H

#include <jello/jdll.h>

#include <mbedtls/x509_crt.h>

#include <stddef.h>
#include <stdint.h>

/** `pem`, `len` bytes, with a NUL after it, as Mbed TLS reads PEM, in a buffer the caller frees,
 * its length with the NUL in *out_len; NULL when out of memory. */
unsigned char* tls_with_nul(const uint8_t* pem, uint32_t len, size_t* out_len);
/** Reads the certificates in PEM `pem`, `len` bytes with its NUL, into `certs`; an Mbed TLS error,
 * or 0. One that it cannot read makes the whole unreadable. */
int tls_parse_certificates(mbedtls_x509_crt* certs, const unsigned char* pem, size_t len);
/** Reads the system's certificates into `certs`: those of SSL_CERT_FILE and of the directories
 * of SSL_CERT_DIR, separated by JELLO_PATH_LIST_SEP, as OpenSSL does, or when neither is set the
 * first of the usual bundle files that holds any, or on Windows the ROOT store's; an Mbed TLS
 * error, or 0. Certificates that Mbed TLS cannot read are passed over. */
int tls_system_certificates(mbedtls_x509_crt* certs);

void crypto_digest_length(jdlo_ctx* c);
void crypto_digest(jdlo_ctx* c);
void crypto_hmac(jdlo_ctx* c);
void crypto_pbkdf2(jdlo_ctx* c);
void crypto_hkdf(jdlo_ctx* c);
void crypto_equal(jdlo_ctx* c);
void crypto_key_length(jdlo_ctx* c);
void crypto_seal(jdlo_ctx* c);
void crypto_open(jdlo_ctx* c);
void x509_parse(jdlo_ctx* c);
void x509_count(jdlo_ctx* c);
void x509_ca(jdlo_ctx* c);
void x509_field(jdlo_ctx* c);
void x509_verify(jdlo_ctx* c);

/** crypto.c's entries in the manifest, over tls.c's kind letters. */
#define CRYPTO_EXPORTS                                                                             \
  {"crypto_digest_length", crypto_digest_length, I, 1, {Y}, 0},                                    \
      {"crypto_digest", crypto_digest, Y, 2, {Y, Y}, 0},                                           \
      {"crypto_hmac",                                                                              \
       crypto_hmac,                                                                                \
       Y,                                                                                          \
       3,                                                                                          \
       {Y, Y, Y},                                                                                  \
       0}, /* Deriving a key from a password is slow on purpose. */                                \
      {"crypto_pbkdf2", crypto_pbkdf2, Y, 5, {Y, Y, Y, I, I}, JELLO_JDLL_BLOCKING},                \
      {"crypto_hkdf", crypto_hkdf, Y, 5, {Y, Y, Y, Y, I}, 0},                                      \
      {"crypto_equal", crypto_equal, B, 2, {Y, Y}, 0},                                             \
      {"crypto_key_length", crypto_key_length, I, 1, {Y}, 0},                                      \
      {"crypto_seal", crypto_seal, Y, 5, {Y, Y, Y, Y, Y}, 0},                                      \
      {"crypto_open", crypto_open, D, 5, {Y, Y, Y, Y, Y}, 0},                                      \
      {"x509_parse", x509_parse, D, 1, {Y}, 0}, {"x509_count", x509_count, I, 1, {D}, 0},          \
      {"x509_ca", x509_ca, B, 2, {D, I}, 0},                                                       \
      {"x509_field", x509_field, Y, 3,                                                             \
       {D, I, I},    0}, /* A check trusting the system's certificates reads them from a file. */  \
      {"x509_verify", x509_verify, I, 4, {Y, Y, Y, L}, JELLO_JDLL_BLOCKING},

#endif /* JELLO_TLS_CRYPTO_H */
