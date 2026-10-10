/*
 * Parser for the per-device development-convenience environment flags.
 *
 * Several hardcoded conveniences (hold switch, media-present pin, panel
 * display-on) are now overridable so the machine can also be run "honestly"
 * for a guest that needs the raw part. They all take a boolean and fall back
 * to a device-specific default when unset, so this one helper keeps their
 * accepted spellings identical: 0/1, on/off, yes/no. Unknown values keep the
 * default rather than guessing.
 */
#ifndef HW_ARM_IPOD_CLASSIC_ENV_H
#define HW_ARM_IPOD_CLASSIC_ENV_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline bool ipod_classic_env_bool(const char *name, bool def)
{
    const char *e = getenv(name);

    if (!e || !*e) {
        return def;
    }
    if (!strcmp(e, "1") || !strcmp(e, "on") || !strcmp(e, "yes") ||
        !strcmp(e, "true")) {
        return true;
    }
    if (!strcmp(e, "0") || !strcmp(e, "off") || !strcmp(e, "no") ||
        !strcmp(e, "false")) {
        return false;
    }
    return def;
}

#endif /* HW_ARM_IPOD_CLASSIC_ENV_H */
