/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (c) 2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

void pasta_start_ns(struct ctx *c);
void pasta_ns_conf(struct ctx *c);
void pasta_child_handler(int signal);
