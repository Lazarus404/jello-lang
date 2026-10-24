// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * tls.jdll's cryptography for std.crypto, on Mbed TLS: digests, HMAC, PBKDF2 and HKDF; sealing
 * and opening data with AES-GCM and ChaCha20-Poly1305; and reading and checking X.509
 * certificates. Algorithms are named as Jello code names them, such as "sha256"; Jello code
 * checks names, lengths and counts before calling, through the *_length exports. */
#include "crypto.h"

#include <mbedtls/asn1.h>
#include <mbedtls/cipher.h>
#include <mbedtls/constant_time.h>
#include <mbedtls/error.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/x509_crt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** The size of an AEAD tag, which sealed data ends with. */
#define TAG_LEN 16

static const struct {
  const char* name;
  mbedtls_md_type_t type;
} DIGESTS[] = {
    {"md5", MBEDTLS_MD_MD5},       {"sha1", MBEDTLS_MD_SHA1},     {"sha224", MBEDTLS_MD_SHA224},
    {"sha256", MBEDTLS_MD_SHA256}, {"sha384", MBEDTLS_MD_SHA384}, {"sha512", MBEDTLS_MD_SHA512},
};

static const struct {
  const char* name;
  mbedtls_cipher_type_t type;
} CIPHERS[] = {
    {"aes-128-gcm", MBEDTLS_CIPHER_AES_128_GCM},
    {"aes-256-gcm", MBEDTLS_CIPHER_AES_256_GCM},
    {"chacha20-poly1305", MBEDTLS_CIPHER_CHACHA20_POLY1305},
};

/** Whether Bytes argument `index` is the NUL-terminated `name`. */
static int named(jdlo_ctx* c, int index, const char* name) {
  uint32_t len = jdl_arg_bytes_len(c, index);
  return strlen(name) == len && memcmp(jdl_arg_bytes_data(c, index), name, len) == 0;
}

/** The digest Bytes argument `index` names, or NULL. */
static const mbedtls_md_info_t* digest(jdlo_ctx* c, int index) {
  for(size_t i = 0; i < sizeof DIGESTS / sizeof DIGESTS[0]; i++) {
    if(named(c, index, DIGESTS[i].name)) return mbedtls_md_info_from_type(DIGESTS[i].type);
  }
  return NULL;
}

/** The cipher Bytes argument `index` names, or NULL. */
static const mbedtls_cipher_info_t* cipher(jdlo_ctx* c, int index) {
  for(size_t i = 0; i < sizeof CIPHERS / sizeof CIPHERS[0]; i++) {
    if(named(c, index, CIPHERS[i].name)) return mbedtls_cipher_info_from_type(CIPHERS[i].type);
  }
  return NULL;
}

/** Returns the first `len` bytes of `out`, made by a call that gave the Mbed TLS error `ret`, or
 * 0; fails the call when `out` is NULL or `ret` is not 0. Then wipes and frees `out`, of `cap`
 * bytes. */
static void return_owned(jdlo_ctx* c, uint8_t* out, size_t len, size_t cap, int ret) {
  if(out && !ret) jdl_return_bytes_copy(c, out, (uint32_t)len);
  else jdl_fail(c, out ? "crypto: Mbed TLS failed" : "crypto: out of memory");
  if(out) mbedtls_platform_zeroize(out, cap);
  free(out);
}

/** crypto_digest_length(algorithm): the bytes in its digests, or 0 when there is no such digest. */
void crypto_digest_length(jdlo_ctx* c) {
  const mbedtls_md_info_t* md = digest(c, 0);
  jdl_return_i32(c, md ? mbedtls_md_get_size(md) : 0);
}

/** crypto_digest(algorithm, data): the digest of `data`. */
void crypto_digest(jdlo_ctx* c) {
  uint8_t out[MBEDTLS_MD_MAX_SIZE];
  const mbedtls_md_info_t* md = digest(c, 0);
  mbedtls_md(md, jdl_arg_bytes_data(c, 1), jdl_arg_bytes_len(c, 1), out);
  jdl_return_bytes_copy(c, out, mbedtls_md_get_size(md));
}

/** crypto_hmac(algorithm, key, data): the HMAC of `data` under `key`. */
void crypto_hmac(jdlo_ctx* c) {
  uint8_t out[MBEDTLS_MD_MAX_SIZE];
  const mbedtls_md_info_t* md = digest(c, 0);
  mbedtls_md_hmac(md, jdl_arg_bytes_data(c, 1), jdl_arg_bytes_len(c, 1), jdl_arg_bytes_data(c, 2),
                  jdl_arg_bytes_len(c, 2), out);
  jdl_return_bytes_copy(c, out, mbedtls_md_get_size(md));
  mbedtls_platform_zeroize(out, sizeof out);
}

/** crypto_pbkdf2(algorithm, password, salt, iterations, length): `length` bytes of key from
 * `password` and `salt` (RFC 8018). */
void crypto_pbkdf2(jdlo_ctx* c) {
  size_t len = (size_t)jdl_arg_i32(c, 4);
  uint8_t* out = (uint8_t*)malloc(len + 1);
  int ret = out ? mbedtls_pkcs5_pbkdf2_hmac_ext(mbedtls_md_get_type(digest(c, 0)),
                                                jdl_arg_bytes_data(c, 1), jdl_arg_bytes_len(c, 1),
                                                jdl_arg_bytes_data(c, 2), jdl_arg_bytes_len(c, 2),
                                                (unsigned int)jdl_arg_i32(c, 3), (uint32_t)len, out)
                : 0;
  return_owned(c, out, len, len, ret);
}

/** crypto_hkdf(algorithm, key, salt, info, length): `length` bytes of key from `key`, `salt` and
 * `info` (RFC 5869). */
void crypto_hkdf(jdlo_ctx* c) {
  size_t len = (size_t)jdl_arg_i32(c, 4);
  uint8_t* out = (uint8_t*)malloc(len + 1);
  int ret = out ? mbedtls_hkdf(digest(c, 0), jdl_arg_bytes_data(c, 2), jdl_arg_bytes_len(c, 2),
                               jdl_arg_bytes_data(c, 1), jdl_arg_bytes_len(c, 1),
                               jdl_arg_bytes_data(c, 3), jdl_arg_bytes_len(c, 3), out, len)
                : 0;
  return_owned(c, out, len, len, ret);
}

/** crypto_equal(a, b): whether `a` and `b` are equal, taking a time that depends only on their
 * lengths. */
void crypto_equal(jdlo_ctx* c) {
  uint32_t len = jdl_arg_bytes_len(c, 0);
  jdl_return_bool(
      c, len == jdl_arg_bytes_len(c, 1) &&
             mbedtls_ct_memcmp(jdl_arg_bytes_data(c, 0), jdl_arg_bytes_data(c, 1), len) == 0);
}

/** crypto_key_length(algorithm): the bytes in a key of the cipher, or 0 when there is no such
 * cipher. Each takes a 12-byte nonce. */
void crypto_key_length(jdlo_ctx* c) {
  const mbedtls_cipher_info_t* info = cipher(c, 0);
  jdl_return_i32(c, info ? (int32_t)mbedtls_cipher_info_get_key_bitlen(info) / 8 : 0);
}

/** Seals (`seal`) or opens arguments 1 to 4, key, nonce, data and associated data, with the cipher
 * argument 0 names; opening returns null when the data or the associated data is not as sealed. */
static void aead(jdlo_ctx* c, int seal) {
  mbedtls_cipher_context_t ctx;
  mbedtls_cipher_init(&ctx);
  size_t in_len = jdl_arg_bytes_len(c, 3), len = 0;
  size_t cap = seal ? in_len + TAG_LEN : in_len;
  uint8_t* out = (uint8_t*)malloc(cap + 1);
  int ret = out ? mbedtls_cipher_setup(&ctx, cipher(c, 0)) : 0;
  if(out && !ret) {
    ret = mbedtls_cipher_setkey(&ctx, jdl_arg_bytes_data(c, 1), (int)jdl_arg_bytes_len(c, 1) * 8,
                                seal ? MBEDTLS_ENCRYPT : MBEDTLS_DECRYPT);
  }
  if(out && !ret) {
    ret = (seal ? mbedtls_cipher_auth_encrypt_ext : mbedtls_cipher_auth_decrypt_ext)(
        &ctx, jdl_arg_bytes_data(c, 2), jdl_arg_bytes_len(c, 2), jdl_arg_bytes_data(c, 4),
        jdl_arg_bytes_len(c, 4), jdl_arg_bytes_data(c, 3), in_len, out, cap, &len, TAG_LEN);
  }
  mbedtls_cipher_free(&ctx);
  /* Sealed data too short to hold a tag is not as sealed either. */
  if(out && !seal && ret) {
    free(out);
    jdl_return_null(c);
  } else {
    return_owned(c, out, len, cap, ret);
  }
}

/** crypto_seal(algorithm, key, nonce, data, associated): `data` encrypted, then a tag that proves
 * it and `associated` unchanged. */
void crypto_seal(jdlo_ctx* c) { aead(c, 1); }

/** crypto_open(algorithm, key, nonce, sealed, associated): the data `sealed` holds, or null when
 * it, or `associated`, is not as sealed. */
void crypto_open(jdlo_ctx* c) { aead(c, 0); }

static void certificates_free(void* payload) {
  mbedtls_x509_crt_free((mbedtls_x509_crt*)payload);
  free(payload);
}

/** Parsed certificates, read once by Jello code and never shared. */
static const jello_jdll_handle_class CERTIFICATES_CLASS = {"X.509 certificates", certificates_free,
                                                           NULL};

/** Whether `data`, `len` bytes, has a PEM block's first line in it. */
static int has_pem(const uint8_t* data, size_t len) {
  static const char BEGIN[] = "-----BEGIN ";
  for(size_t i = 0; i + sizeof BEGIN - 1 <= len; i++) {
    if(memcmp(data + i, BEGIN, sizeof BEGIN - 1) == 0) return 1;
  }
  return 0;
}

/** Reads the certificates in Bytes argument `index` into `certs`: PEM, or DER, one after another;
 * an Mbed TLS error, or 0. One that it cannot read makes the whole unreadable. */
static int read_certificates(jdlo_ctx* c, int index, mbedtls_x509_crt* certs) {
  const uint8_t* data = jdl_arg_bytes_data(c, index);
  size_t len = jdl_arg_bytes_len(c, index);
  if(len && !has_pem(data, len)) {
    for(size_t at = 0; at < len;) {
      int ret = mbedtls_x509_crt_parse_der(certs, data + at, len - at);
      if(ret) return ret;
      const mbedtls_x509_crt* last = certs;
      while(last->next) last = last->next;
      at += last->raw.len;
    }
    return 0;
  }
  unsigned char* text = tls_with_nul(data, (uint32_t)len, &len);
  int ret = text ? tls_parse_certificates(certs, text, len) : MBEDTLS_ERR_X509_ALLOC_FAILED;
  free(text);
  return ret;
}

/** x509_parse(text): the certificates in `text`, PEM or DER, as a handle, or, when it cannot be
 * read, why, as Bytes. */
void x509_parse(jdlo_ctx* c) {
  mbedtls_x509_crt* certs = (mbedtls_x509_crt*)malloc(sizeof *certs);
  if(!certs) {
    jdl_fail(c, "x509_parse: out of memory");
    return;
  }
  mbedtls_x509_crt_init(certs);
  int ret = read_certificates(c, 0, certs);
  if(ret) {
    char why[160];
    mbedtls_strerror(ret, why, sizeof why);
    certificates_free(certs);
    jdl_return_bytes_copy(c, (const uint8_t*)why, (uint32_t)strlen(why));
    return;
  }
  size_t bytes = 0;
  for(const mbedtls_x509_crt* crt = certs; crt; crt = crt->next)
    bytes += sizeof *crt + crt->raw.len;
  jdl_return_handle(c, &CERTIFICATES_CLASS, certs, (uint32_t)bytes);
}

/** Certificate argument 1 of the handle in argument 0, or NULL, with the call failed. */
static const mbedtls_x509_crt* certificate(jdlo_ctx* c) {
  const mbedtls_x509_crt* crt = (const mbedtls_x509_crt*)jdl_arg_handle(c, 0, &CERTIFICATES_CLASS);
  for(int32_t i = jdl_arg_i32(c, 1); crt && i > 0; i--) crt = crt->next;
  if(!crt) jdl_fail(c, "x509: no such certificate");
  return crt;
}

/** x509_count(certificates): how many there are. */
void x509_count(jdlo_ctx* c) {
  const mbedtls_x509_crt* crt = (const mbedtls_x509_crt*)jdl_arg_handle(c, 0, &CERTIFICATES_CLASS);
  int32_t n = 0;
  for(; crt; crt = crt->next) n++;
  jdl_return_i32(c, n);
}

/** x509_ca(certificates, index): whether the certificate may sign others. */
void x509_ca(jdlo_ctx* c) {
  const mbedtls_x509_crt* crt = certificate(c);
  if(crt)
    jdl_return_bool(c, mbedtls_x509_crt_has_ext_type(crt, MBEDTLS_X509_EXT_BASIC_CONSTRAINTS) &&
                           crt->MBEDTLS_PRIVATE(ca_istrue));
}

/** Writes `t` to `out` in ISO 8601, in UTC. */
static int iso_time(char* out, size_t size, const mbedtls_x509_time* t) {
  return snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02dZ", t->year, t->mon, t->day, t->hour,
                  t->min, t->sec);
}

/** Writes the certificate's subject alternative names, DNS names and IP addresses, to `out`, one a
 * line; the bytes written, or -1 when they do not fit. */
static int alt_names(const mbedtls_x509_crt* crt, char* out, size_t size) {
  int n = 0;
  for(const mbedtls_x509_sequence* s = &crt->subject_alt_names; s && s->buf.p; s = s->next) {
    mbedtls_x509_subject_alternative_name san;
    if(mbedtls_x509_parse_subject_alt_name(&s->buf, &san) != 0) continue;
    const mbedtls_x509_buf* b = &san.san.unstructured_name;
    const char* sep = n ? "\n" : "";
    int w = 0;
    if(san.type == MBEDTLS_X509_SAN_DNS_NAME) {
      w = snprintf(out + n, size - (size_t)n, "%s%.*s", sep, (int)b->len, (const char*)b->p);
    } else if(san.type == MBEDTLS_X509_SAN_IP_ADDRESS && b->len == 4) {
      w = snprintf(out + n, size - (size_t)n, "%s%d.%d.%d.%d", sep, b->p[0], b->p[1], b->p[2],
                   b->p[3]);
    } else if(san.type == MBEDTLS_X509_SAN_IP_ADDRESS && b->len == 16) {
      w = snprintf(out + n, size - (size_t)n, "%s", sep);
      for(int i = 0; i < 16 && w >= 0 && (size_t)(n + w) < size; i += 2) {
        w += snprintf(out + n + w, size - (size_t)(n + w), "%s%x", i ? ":" : "",
                      b->p[i] << 8 | b->p[i + 1]);
      }
    }
    mbedtls_x509_free_subject_alt_name(&san);
    if(w < 0 || (size_t)(n + w) >= size) return -1;
    n += w;
  }
  return n;
}

/** x509_field(certificates, index, field): the certificate's subject (0) or issuer (1) name, its
 * serial number in hexadecimal (2), its DER (3), the start (4) and end (5) of its validity in ISO
 * 8601, its subject alternative names a line each (6), or its key's kind and size, as "RSA 2048"
 * (7). */
void x509_field(jdlo_ctx* c) {
  const mbedtls_x509_crt* crt = certificate(c);
  int field = jdl_arg_i32(c, 2);
  if(!crt) return;
  if(field == 3) {
    jdl_return_bytes_copy(c, crt->raw.p, (uint32_t)crt->raw.len);
    return;
  }
  /* Room for the longest names any certificate in use has, and many times more. */
  enum { SIZE = 65536 };
  char* text = (char*)malloc(SIZE);
  if(!text) {
    jdl_fail(c, "x509_field: out of memory");
    return;
  }
  int n = 0;
  switch(field) {
    case 0: n = mbedtls_x509_dn_gets(text, SIZE, &crt->subject); break;
    case 1: n = mbedtls_x509_dn_gets(text, SIZE, &crt->issuer); break;
    case 2: {
      /* DER puts a 0 before a positive number whose top bit is set, which is not part of it. */
      size_t i = 0;
      while(i + 1 < crt->serial.len && crt->serial.p[i] == 0) i++;
      for(; i < crt->serial.len && n + 3 < SIZE; i++) {
        n += snprintf(text + n, SIZE - (size_t)n, "%02x", crt->serial.p[i]);
      }
      break;
    }
    case 4: n = iso_time(text, SIZE, &crt->valid_from); break;
    case 5: n = iso_time(text, SIZE, &crt->valid_to); break;
    case 6: n = alt_names(crt, text, SIZE); break;
    case 7:
      n = snprintf(text, SIZE, "%s %u", mbedtls_pk_get_name(&crt->pk),
                   (unsigned)mbedtls_pk_get_bitlen(&crt->pk));
      break;
    default: n = -2;
  }
  if(n == -2) jdl_fail(c, "x509_field: no such field");
  else if(n < 0) jdl_fail(c, "x509_field: a name is too long");
  else jdl_return_bytes_copy(c, (const uint8_t*)text, (uint32_t)n);
  free(text);
}

/** Whether `a` is before `b`. */
static int time_before(const mbedtls_x509_time* a, const mbedtls_x509_time* b) {
  int x[] = {a->year, a->mon, a->day, a->hour, a->min, a->sec};
  int y[] = {b->year, b->mon, b->day, b->hour, b->min, b->sec};
  for(int i = 0; i < 6; i++) {
    if(x[i] != y[i]) return x[i] < y[i];
  }
  return 0;
}

/** A time to check certificates at: its second, and whether it is later in that second. */
typedef struct check_time {
  mbedtls_x509_time second;
  int late;
} check_time;

/** Judges the certificate's dates by the check_time `at` points to, rather than the clock. */
static int check_dates(void* at, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
  (void)depth;
  const check_time* now = (const check_time*)at;
  *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
  if(time_before(&now->second, &crt->valid_from)) *flags |= MBEDTLS_X509_BADCERT_FUTURE;
  /* A certificate is valid up to and including the second its validity ends. */
  if(time_before(&crt->valid_to, &now->second) ||
     (now->late && !time_before(&now->second, &crt->valid_to))) {
    *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
  }
  return 0;
}

/** x509_verify(chain, trusted, host, at): what is wrong with the DER certificate chain `chain`,
 * the one to check first, when trusting the DER certificates `trusted`, or the system's when it is
 * empty, at the time `at`, in milliseconds since 1970, and for `host`, unless that is empty; Mbed
 * TLS's MBEDTLS_X509_BADCERT_ flags, so 0 when nothing is. */
void x509_verify(jdlo_ctx* c) {
  mbedtls_x509_crt chain, trusted;
  mbedtls_x509_crt_init(&chain);
  mbedtls_x509_crt_init(&trusted);
  uint32_t flags = 0;
  int64_t ms = jdl_arg_i64(c, 3);
  time_t seconds = (time_t)(ms / 1000 - (ms % 1000 < 0));
  struct tm tm;
  check_time at = {{0}, ms % 1000 != 0};
#ifdef _WIN32
  if(gmtime_s(&tm, &seconds) == 0) {
#else
  if(gmtime_r(&seconds, &tm)) {
#endif
    at.second = (mbedtls_x509_time){tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                    tm.tm_hour,        tm.tm_min,     tm.tm_sec};
  }
  int ret = read_certificates(c, 0, &chain);
  if(!ret)
    ret = jdl_arg_bytes_len(c, 1) ? read_certificates(c, 1, &trusted)
                                  : tls_system_certificates(&trusted);
  size_t host_len = 0;
  char* host = (char*)tls_with_nul(jdl_arg_bytes_data(c, 2), jdl_arg_bytes_len(c, 2), &host_len);
  if(!host) ret = MBEDTLS_ERR_X509_ALLOC_FAILED;
  if(!ret) {
    ret = mbedtls_x509_crt_verify(&chain, &trusted, NULL, host_len > 1 ? host : NULL, &flags,
                                  check_dates, &at);
  }
  free(host);
  mbedtls_x509_crt_free(&chain);
  mbedtls_x509_crt_free(&trusted);
  /* A chain that could not be checked at all is not trusted. */
  if(ret && !flags) flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
  jdl_return_i32(c, (int32_t)flags);
}
