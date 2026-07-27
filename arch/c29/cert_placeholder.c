/* SPDX-License-Identifier: MIT */
/*
 * Flash-profile boot certificate slot at 0x10000000.  package-seccfg.sh
 * overwrites this section post-link (--update-section cert=...).
 */

#include <stdint.h>

#if defined(ULMK_C29_FLASH) && ULMK_C29_FLASH
__attribute__((retain, section("cert")))
const uint8_t ulmk_c29_certificate[4096] = { 0 };
#endif
