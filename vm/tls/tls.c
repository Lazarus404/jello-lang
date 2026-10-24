// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * tls.jdll: TLS for Jello code, on the vendored Mbed TLS (third_party/mbedtls-3.5.1). It exports
 * configurations to Jello code, and to the VM's net.c, through the table `jello_tls_ops`
 * (jello/internal/tls.h), the sessions it runs on a stream's socket once its handshake starts.
 *
 * A configuration is one side's: a server's certificate chain and private key, or the
 * certificates a client trusts, which it checks the server's chain and name against. It never
 * changes once made, so a handle copied to another process shares it. A session belongs to its
 * socket, so it moves with it; its socket is non-blocking, and a call that cannot go on returns
 * what it waits for, for net.c to park its process until then. Mbed TLS's errors become net_error
 * codes from NET_TLS_ERRORS, and its socket errors errno values. */
#include <jello/internal/entropy.h>
#include <jello/internal/platform.h>
#include <jello/internal/sockets.h>
#include <jello/internal/tls.h>

#include "crypto.h"

#include <mbedtls/build_info.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <wincrypt.h>
#else
/** Where POSIX systems keep the certificates they trust, for a client given none. */
static const char* const SYSTEM_CERTIFICATES[] = {
    "/etc/ssl/cert.pem",                  /* macOS, Alpine, the BSDs */
    "/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu */
    "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora, RHEL */
    "/etc/ssl/ca-bundle.pem",             /* openSUSE */
};
#endif

typedef struct net_tls_config {
  jdl_refs refs;
  int server;
  mbedtls_ssl_config conf;
  /** A server's chain, its own first, or the certificates a client trusts. */
  mbedtls_x509_crt certs;
  mbedtls_pk_context key; /**< A server's private key. */
} net_tls_config;

struct net_tls {
  mbedtls_ssl_context ssl;
  net_tls_config* config; /**< Held while the session lives. */
  int fd;
  int sys_err; /**< The errno of the last send or receive that failed. */
  int32_t err; /**< Why the last call failed, as a net_error code. */
};

/** Mbed TLS's random bytes, from the system's entropy. */
static int random_bytes(void* unused, unsigned char* out, size_t len) {
  return jello_entropy(out, len) ? 0 : MBEDTLS_ERR_ERROR_GENERIC_ERROR;
}

static void config_free(void* payload) {
  if(!jdl_release(payload)) return;
  net_tls_config* t = (net_tls_config*)payload;
  mbedtls_ssl_config_free(&t->conf);
  mbedtls_x509_crt_free(&t->certs);
  mbedtls_pk_free(&t->key);
  free(t);
}

static const jello_jdll_handle_class CONFIG_CLASS = {"TLS configuration", config_free, jdl_retain};

unsigned char* tls_with_nul(const uint8_t* pem, uint32_t len, size_t* out_len) {
  unsigned char* s = (unsigned char*)malloc((size_t)len + 1);
  if(!s) return NULL;
  if(len) memcpy(s, pem, len);
  s[len] = '\0';
  *out_len = (size_t)len + 1;
  return s;
}

int tls_parse_certificates(mbedtls_x509_crt* certs, const unsigned char* pem, size_t len) {
  int ret = mbedtls_x509_crt_parse(certs, pem, len);
  return ret > 0 ? MBEDTLS_ERR_X509_CERT_UNKNOWN_FORMAT : ret;
}

int tls_system_certificates(mbedtls_x509_crt* certs) {
  int ret = MBEDTLS_ERR_X509_FILE_IO_ERROR;
  const char* file = getenv("SSL_CERT_FILE");
  const char* dirs = getenv("SSL_CERT_DIR");
  if(file && !*file) file = NULL;
  if(dirs && !*dirs) dirs = NULL;
  if(file || dirs) {
    if(file) ret = mbedtls_x509_crt_parse_file(certs, file);
    for(const char* dir = dirs; dir && *dir;) {
      const char* end = strchr(dir, JELLO_PATH_LIST_SEP);
      size_t len = end ? (size_t)(end - dir) : strlen(dir);
      char path[4096];
      if(len > 0 && len < sizeof path) {
        memcpy(path, dir, len);
        path[len] = '\0';
        int got = mbedtls_x509_crt_parse_path(certs, path);
        if(got < 0) ret = got;
      }
      dir = end ? end + 1 : dir + len;
    }
  } else {
#ifdef _WIN32
    /* The certificates Windows trusts to identify servers, those of the ROOT store. */
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    for(PCCERT_CONTEXT cert = NULL; store && (cert = CertEnumCertificatesInStore(store, cert));) {
      if(cert->dwCertEncodingType & X509_ASN_ENCODING)
        mbedtls_x509_crt_parse_der(certs, cert->pbCertEncoded, cert->cbCertEncoded);
    }
    if(store) CertCloseStore(store, 0);
#else
    size_t n = sizeof SYSTEM_CERTIFICATES / sizeof SYSTEM_CERTIFICATES[0];
    struct stat st;
    for(size_t i = 0; i < n && !certs->version; i++) {
      if(stat(SYSTEM_CERTIFICATES[i], &st) != 0) continue;
      ret = mbedtls_x509_crt_parse_file(certs, SYSTEM_CERTIFICATES[i]);
    }
#endif
  }
  return certs->version ? 0 : ret < 0 ? ret : MBEDTLS_ERR_X509_CERT_UNKNOWN_FORMAT;
}

/** About how many bytes `t`'s certificates and key take, for the collector. */
static uint32_t config_bytes(const net_tls_config* t, size_t key_len) {
  size_t n = sizeof(net_tls_config) + key_len;
  for(const mbedtls_x509_crt* crt = &t->certs; crt && crt->version; crt = crt->next) {
    n += sizeof(mbedtls_x509_crt) + crt->raw.len;
  }
  return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

/** A configuration from tls_config's arguments, NULL with *err set to a
 * net_error code when it cannot be made, and its size in *bytes. */
static net_tls_config* make_config(jdlo_ctx* c, uint32_t* bytes, int32_t* err) {
  int server = jdl_arg_bool(c, 0);
  size_t first_len = 0, key_len = 0;
  net_tls_config* t = (net_tls_config*)calloc(1, sizeof(net_tls_config));
  unsigned char* first =
      t ? tls_with_nul(jdl_arg_bytes_data(c, 1), jdl_arg_bytes_len(c, 1), &first_len) : NULL;
  unsigned char* key =
      first && server ? tls_with_nul(jdl_arg_bytes_data(c, 2), jdl_arg_bytes_len(c, 2), &key_len)
                      : NULL;
  if(!first || (server && !key)) {
    free(first);
    free(t);
    *err = ENOMEM;
    return NULL;
  }
  t->refs = 1;
  t->server = server;
  mbedtls_ssl_config_init(&t->conf);
  mbedtls_x509_crt_init(&t->certs);
  mbedtls_pk_init(&t->key);
  int side = server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT;
  int ret = mbedtls_ssl_config_defaults(&t->conf, side, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
  mbedtls_ssl_conf_rng(&t->conf, random_bytes, NULL);
  if(server) {
    if(!ret) ret = tls_parse_certificates(&t->certs, first, first_len);
    if(!ret) ret = mbedtls_pk_parse_key(&t->key, key, key_len, NULL, 0, random_bytes, NULL);
    if(!ret) ret = mbedtls_pk_check_pair(&t->certs.pk, &t->key, random_bytes, NULL);
    if(!ret) ret = mbedtls_ssl_conf_own_cert(&t->conf, &t->certs, &t->key);
  } else {
    if(!ret) {
      ret = first_len > 1 ? tls_parse_certificates(&t->certs, first, first_len)
                          : tls_system_certificates(&t->certs);
    }
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->certs, NULL);
  }
  mbedtls_platform_zeroize(key, key_len);
  free(key);
  free(first);
  if(ret) {
    config_free(t);
    *err = NET_TLS_ERRORS - ret;
    return NULL;
  }
  *bytes = config_bytes(t, key_len);
  return t;
}

static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
  net_tls* s = (net_tls*)ctx;
  for(;;) {
    ssize_t n = jello_send(s->fd, buf, len);
    if(n >= 0) return (int)n;
    if(errno == EINTR) continue;
    if(errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    s->sys_err = errno;
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
}

static int bio_recv(void* ctx, unsigned char* buf, size_t len) {
  net_tls* s = (net_tls*)ctx;
  for(;;) {
    ssize_t n = jello_recv(s->fd, buf, len);
    if(n >= 0) return (int)n;
    if(errno == EINTR) continue;
    if(errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    s->sys_err = errno;
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }
}

static net_tls* open_session(void* config, int fd, const uint8_t* host, uint32_t len,
                             int32_t* err) {
  net_tls_config* t = (net_tls_config*)config;
  /* A client checks the server's certificate is for the name it was given, so needs one. */
  if(!t->server && !len) {
    *err = EINVAL;
    return NULL;
  }
  net_tls* s = (net_tls*)calloc(1, sizeof(net_tls));
  char* name = s ? (char*)malloc((size_t)len + 1) : NULL;
  if(!name) {
    free(s);
    *err = ENOMEM;
    return NULL;
  }
  memcpy(name, host, len);
  name[len] = '\0';
  mbedtls_ssl_init(&s->ssl);
  int ret = mbedtls_ssl_setup(&s->ssl, &t->conf);
  if(!ret && !t->server) ret = mbedtls_ssl_set_hostname(&s->ssl, name);
  free(name);
  if(ret) {
    mbedtls_ssl_free(&s->ssl);
    free(s);
    *err = NET_TLS_ERRORS - ret;
    return NULL;
  }
  s->fd = fd;
  mbedtls_ssl_set_bio(&s->ssl, s, bio_send, bio_recv, NULL);
  jdl_retain(t);
  s->config = t;
  return s;
}

/** What Mbed TLS's `ret` means for a session's call, recording why when it failed. */
static int result(net_tls* s, int ret) {
  if(ret >= 0) return ret;
  if(ret == MBEDTLS_ERR_SSL_WANT_READ) return NET_TLS_WANT_READ;
  if(ret == MBEDTLS_ERR_SSL_WANT_WRITE) return NET_TLS_WANT_WRITE;
  int net = ret == MBEDTLS_ERR_NET_SEND_FAILED || ret == MBEDTLS_ERR_NET_RECV_FAILED;
  s->err = net && s->sys_err ? s->sys_err : NET_TLS_ERRORS - ret;
  return NET_TLS_FAILED;
}

static int handshake(net_tls* s) { return result(s, mbedtls_ssl_handshake(&s->ssl)); }

static int read_data(net_tls* s, uint8_t* buf, uint32_t max) {
  int ret = mbedtls_ssl_read(&s->ssl, buf, max);
  /* The other end's closing, said or not, ends the stream. */
  if(ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
  return result(s, ret);
}

static int write_data(net_tls* s, const uint8_t* data, uint32_t len) {
  return result(s, mbedtls_ssl_write(&s->ssl, data, len));
}

static int32_t error(const net_tls* s) { return s->err; }

static void close_session(net_tls* s, int notify) {
  if(notify) mbedtls_ssl_close_notify(&s->ssl);
  mbedtls_ssl_free(&s->ssl);
  config_free(s->config);
  free(s);
}

static void error_text(int32_t code, char* out, size_t size) {
  mbedtls_strerror(NET_TLS_ERRORS - code, out, size);
}

JELLO_JDLL_EXPORT const net_tls_ops jello_tls_ops = {
    NET_TLS_OPS_VERSION, &CONFIG_CLASS, open_session,  handshake,  read_data,
    write_data,          error,         close_session, error_text,
};

/** tls_version(): the TLS library's name and version, as "Mbed TLS 3.5.1". */
static void tls_version(jdlo_ctx* c) {
  static const char VERSION[] = MBEDTLS_VERSION_STRING_FULL;
  jdl_return_bytes_copy(c, (const uint8_t*)VERSION, (uint32_t)(sizeof VERSION - 1));
}

/** tls_config(server, certificates, key): a server's configuration, presenting the PEM
 * certificate chain `certificates`, its own first, proven by the PEM private key `key`, or a
 * client's, trusting the PEM certificates `certificates`, or the system's when it is empty; when
 * it cannot be made, why, as an I32 net_error code. */
static void tls_config(jdlo_ctx* c) {
  uint32_t bytes = 0;
  int32_t err = 0;
  net_tls_config* t = make_config(c, &bytes, &err);
  if(t) jdl_return_handle(c, &CONFIG_CLASS, t, bytes);
  else jdl_return_value(c, jello_make_i32(err));
}

#define B (uint8_t)JELLO_T_BOOL
#define Y (uint8_t)JELLO_T_BYTES
#define I (uint8_t)JELLO_T_I32
#define L (uint8_t)JELLO_T_I64
#define D (uint8_t)JELLO_T_DYNAMIC

static const jello_jdll_export TLS_EXPORTS[] = {
    {"tls_version", tls_version, Y, 0, {0}, 0},
    /* A client trusting the system's certificates reads them from a file. */
    {"tls_config", tls_config, D, 3, {B, Y, Y}, JELLO_JDLL_BLOCKING},
    CRYPTO_EXPORTS};

JELLO_JDLL_LIBRARY(tls, TLS_EXPORTS);
