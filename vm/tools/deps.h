// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Jahred Love. All rights reserved.

/** @file
 * jlo_tool's project commands, implemented in deps.c. */

#ifndef JELLO_TOOLS_DEPS_H
#define JELLO_TOOLS_DEPS_H

/** `jlo_tool get-deps [DIR]`, given the arguments after the command; returns the exit status.
 * Fetches every dependency in DIR's jello.json, transitively, flat under packages/NAME. A
 * dependency is a version (a tag of github.com/$JELLO_DEPS_GITHUB_ORG/NAME, or an archive under
 * $JELLO_REGISTRY_URL), `{"github", "tag" or "commit"}`, or `{"path"}`, which links a local
 * package unpinned and is allowed only in the project's own jello.json. jello.lock pins each
 * archive's SHA-256; a mismatch fails, leaving that package and the lock unchanged. A package is
 * fetched again only when its source differs from the lock's or packages/NAME lacks it. Packages
 * nothing asks for are removed from packages/ and the lock. Two requests for one name from
 * different sources fail. */
int jlo_get_deps_main(int argc, char** argv);
/** `jlo_tool init [DIR] [--name NAME]`, given the arguments after the command; returns the exit
 * status. */
int jlo_init_main(int argc, char** argv);

#endif
