# Single source of truth for the version. Everything else derives from it:
# project(), the VERSIONINFO resource, and the tag check in CI.
#
# installer.toml carries the same number and CI asserts the two agree, because
# Forge stamps the installer from the TOML and cannot see this file.
set(ZCR_VERSION_MAJOR 0)
set(ZCR_VERSION_MINOR 2)
set(ZCR_VERSION_PATCH 2)

set(ZCR_VERSION "${ZCR_VERSION_MAJOR}.${ZCR_VERSION_MINOR}.${ZCR_VERSION_PATCH}")
set(ZCR_VERSION_RC "${ZCR_VERSION_MAJOR},${ZCR_VERSION_MINOR},${ZCR_VERSION_PATCH},0")
set(ZCR_VERSION_RC_STR "${ZCR_VERSION}.0")

set(ZCR_PRODUCT   "ZCR")
set(ZCR_COMPANY   "Locke Werks")
set(ZCR_COPYRIGHT "Copyright (C) 2026 Locke Werks")
