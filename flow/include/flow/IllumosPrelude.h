/*
 * IllumosPrelude.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2026 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Force-included before any other header on illumos targets so that
 * libc symbols that collide with FoundationDB's global names can be
 * renamed out of the way before they are ever declared.
 *
 * Kept deliberately tiny; no dependencies on FoundationDB types.
 */

#ifndef FLOW_ILLUMOS_PRELUDE_H
#define FLOW_ILLUMOS_PRELUDE_H

#if defined(__sun) && defined(__SVR4)

/*
 * illumos <unistd.h> exposes a legacy BSD `yield(void)` that collides with
 * flow's `Future<Void> yield(TaskPriority)`.  Rename the libc symbol out of
 * the way before it is declared.  FDB does not call the libc yield().
 */
#define yield __illumos_libc_yield_do_not_use

#include <unistd.h>

#undef yield

/*
 * illumos <sys/types.h> typedefs `index_t` and other short names that can
 * collide with template aliases.  We rename flow's template alias instead
 * (see ObjectSerializerTraits.h -> pack_index_t) rather than undefining
 * here, so leave the libc typedef intact.
 */

#endif /* __sun && __SVR4 */

#endif /* FLOW_ILLUMOS_PRELUDE_H */
