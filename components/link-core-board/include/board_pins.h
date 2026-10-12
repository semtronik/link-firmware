/**
 * @file board_pins.h
 * @brief Pin map for the link base board, for the selected revision.
 *
 * Include this, never a board_pins_rev_*.h directly. The revision is a
 * compile-time choice - CONFIG_LINK_BOARD_REV_*, set in menuconfig or
 * sdkconfig.defaults - because a board does not change revision while it
 * runs, and a firmware built for one has no business on another.
 *
 * BOARD_REV_STRING names the revision a binary was built for. Report it
 * wherever a unit identifies itself, so a firmware on the wrong board
 * can be told apart from a broken one.
 *
 * Adding a revision: add the Kconfig choice entry, add the header, add a
 * branch below. The pin maps are generated from the schematic - do not
 * hand-edit them.
 */

#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_LINK_BOARD_REV_A)
#include "board_pins_rev_a.h"
#else
#error "no board revision selected - set CONFIG_LINK_BOARD_REV_* in sdkconfig"
#endif
