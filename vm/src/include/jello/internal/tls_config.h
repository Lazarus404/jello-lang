// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * The options the VM adds to Mbed TLS's default configuration, which Mbed TLS reads as its
 * MBEDTLS_USER_CONFIG_FILE. A configuration's key is shared by the processes using it, on several
 * threads, and its RSA blinding changes with each signature, so Mbed TLS locks its contexts. AES's
 * tables are constant, since building them on first use races when two threads start at once. */
#ifndef JELLO_INTERNAL_TLS_CONFIG_H
#define JELLO_INTERNAL_TLS_CONFIG_H

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#define MBEDTLS_AES_ROM_TABLES

#endif
