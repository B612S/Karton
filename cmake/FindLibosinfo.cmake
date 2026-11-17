# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
# SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBOSINFO REQUIRED libosinfo-1.0 IMPORTED_TARGET)

if(TARGET PkgConfig::LIBOSINFO AND NOT TARGET Libosinfo::Libosinfo)
  add_library(Libosinfo::Libosinfo ALIAS PkgConfig::LIBOSINFO)
endif()

mark_as_advanced(LIBOSINFO_INCLUDE_DIR LIBOSINFO_LIBRARY)

