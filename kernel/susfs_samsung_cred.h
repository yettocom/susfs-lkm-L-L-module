/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SUSFS_SAMSUNG_CRED_H
#define __SUSFS_SAMSUNG_CRED_H

#include <linux/cred.h>
#include <linux/types.h>

/* Credential references through Samsung's KDP helpers, with the plain inline pair as the
 * fallback.  See susfs_samsung_cred.c for why the inline pair is not safe on a Samsung kernel. */

/* Resolve the helpers once.  Idempotent, and safe to call before anything uses the pair below:
 * when the symbols are absent the pair stays on the standard path. */
void susfs_samsung_cred_init(void);

const struct cred *susfs_samsung_get_cred(const struct cred *cred);
void susfs_samsung_put_cred(const struct cred *cred);

#endif /* __SUSFS_SAMSUNG_CRED_H */
