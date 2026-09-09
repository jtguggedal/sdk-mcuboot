/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "bootutil/bootutil.h"
#include "bootutil/bootutil_log.h"
#include "flash_map_backend/flash_map_backend.h"
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include <ironside/se/api.h>
#include <ironside/se/mpcconf.h>

BOOT_LOG_MODULE_DECLARE(mcuboot);

BUILD_ASSERT(MCUBOOT_IMAGE_NUMBER <= 4,
	     "The MPC override configuration used to provide write protection for the image "
	     "partitions does not currently support more than four images.");

/* Required alignment for OVERRIDE addresses in MPC110. */
#define OVERRIDE_ALIGNMENT_KB 4
#define OVERRIDE_ALIGNMENT    KB(OVERRIDE_ALIGNMENT_KB)

#define ACCESSIBLE_MRAM_START PARTITION_NODE_ADDRESS(DT_CHOSEN(zephyr_code_partition))

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(secure_storage_partition))
/* On nRF92 the end of MRAM is NOT the application's to use: per the Saana MRAM_11
 * allocation, everything from 0x0E52F000 up is Soft SIM storage and Cellular firmware,
 * storage and filesystem. Requiring secure storage to end at the end of MRAM, as the
 * nRF54H20 assertion below does, therefore places it inside the Cellular filesystem.
 * IronSide SE accepts that without complaint and it then corrupts, so the requirement has
 * to be dropped rather than satisfied.
 *
 * What the override tables actually need is only that secure storage sits above the write
 * protected bootloader region, so that the R_X and RW ranges below it are well formed. That
 * is asserted further down, where BOOT_PARTITION_END exists. The MRAM above secure storage
 * keeps whatever IronSide SE granted it -- on this part that is override 5, which these
 * tables deliberately leave alone.
 */
#if !defined(CONFIG_SOC_SERIES_NRF92)
BUILD_ASSERT((PARTITION_ADDRESS(secure_storage_partition) +
	      PARTITION_SIZE(secure_storage_partition)) ==
		     (DT_REG_ADDR(DT_NODELABEL(mram1x)) + DT_REG_SIZE(DT_NODELABEL(mram1x))),
	     "The MPC override configuration used to provide write protection for the image "
	     "partitions currently requires that the secure storage partitions are placed at the "
	     "end of MRAM.");
#endif

#define ACCESSIBLE_MRAM_END PARTITION_ADDRESS(secure_storage_partition)
#else
#define ACCESSIBLE_MRAM_END (DT_REG_ADDR(DT_NODELABEL(mram1x)) + DT_REG_SIZE(DT_NODELABEL(mram1x)))
#endif

#define BOOT_PARTITION_END                                                                         \
	(PARTITION_NODE_ADDRESS(DT_CHOSEN(zephyr_code_partition)) +                                \
	 CONFIG_NCS_MCUBOOT_MPCCONF_STATIC_WRITE_PROTECTION_INITIAL_REGION_SIZE)

/* Partition slot addresses (MCUboot index order, not address-sorted)  */
#define PRIMARY_P0_START   PARTITION_ADDRESS(slot0_partition)
#define PRIMARY_P0_END     (PRIMARY_P0_START + PARTITION_SIZE(slot0_partition))
#define SECONDARY_P0_START PARTITION_ADDRESS(slot1_partition)
#define SECONDARY_P0_END   (SECONDARY_P0_START + PARTITION_SIZE(slot1_partition))

#if MCUBOOT_IMAGE_NUMBER > 1
#define PRIMARY_P1_START   PARTITION_ADDRESS(slot2_partition)
#define PRIMARY_P1_END     (PRIMARY_P1_START + PARTITION_SIZE(slot2_partition))
#define SECONDARY_P1_START PARTITION_ADDRESS(slot3_partition)
#define SECONDARY_P1_END   (SECONDARY_P1_START + PARTITION_SIZE(slot3_partition))
#endif

#if MCUBOOT_IMAGE_NUMBER > 2
#define PRIMARY_P2_START   PARTITION_ADDRESS(slot4_partition)
#define PRIMARY_P2_END     (PRIMARY_P2_START + PARTITION_SIZE(slot4_partition))
#define SECONDARY_P2_START PARTITION_ADDRESS(slot5_partition)
#define SECONDARY_P2_END   (SECONDARY_P2_START + PARTITION_SIZE(slot5_partition))
#endif

#if MCUBOOT_IMAGE_NUMBER > 3
#define PRIMARY_P3_START   PARTITION_ADDRESS(slot6_partition)
#define PRIMARY_P3_END     (PRIMARY_P3_START + PARTITION_SIZE(slot6_partition))
#define SECONDARY_P3_START PARTITION_ADDRESS(slot7_partition)
#define SECONDARY_P3_END   (SECONDARY_P3_START + PARTITION_SIZE(slot7_partition))
#endif

/* Median of three values. */
#define MEDIAN3(a, b, c) MAX(MIN((a), (b)), MIN(MAX((a), (b)), (c)))

/*
 * Ordering for four distinct values.
 * SORT4_2ND/3RD require all four inputs to be distinct.
 */
#define SORT4_1ST(a, b, c, d) MIN(MIN((a), (b)), MIN((c), (d)))
#define SORT4_4TH(a, b, c, d) MAX(MAX((a), (b)), MAX((c), (d)))
#define SORT4_2ND(a, b, c, d)                                                                      \
	MIN(MIN(MIN(MAX((a), (b)), MAX((a), (c))), MAX((a), (d))),                                 \
	    MIN(MIN(MAX((b), (c)), MAX((b), (d))), MAX((c), (d))))
#define SORT4_3RD(a, b, c, d)                                                                      \
	MAX(MAX(MAX(MIN((a), (b)), MIN((a), (c))), MIN((a), (d))),                                 \
	    MAX(MAX(MIN((b), (c)), MIN((b), (d))), MIN((c), (d))))

/*
 * Map a sorted ACTIVE start back to its partition end. First match wins,
 * so duplicates from unused-slot aliasing are safe.
 */
#define END_FOR_START(s0, e0, s1, e1, s2, e2, s3, e3, key)                                         \
	(((s0) == (key)) ? (e0) : ((s1) == (key)) ? (e1) : ((s2) == (key)) ? (e2) : (e3))

/*
 * MCUboot image indices (P0..P3) are not necessarily in address order in MRAM.
 * The macros below this are used to sort them to be able to add write permissions to the gaps
 * before, between, or after the partitions.
 *
 * When MCUBOOT_IMAGE_NUMBER < 4, unused ACTIVE_* values repeat and the inter-image gaps
 * have zero size. This is done just to simplify certain parts below, like the BUILD_ASSERTs.
 */
#if MCUBOOT_IMAGE_NUMBER == 1

#define PRIMARY_ACTIVE_0_START PRIMARY_P0_START
#define PRIMARY_ACTIVE_1_START PRIMARY_P0_START
#define PRIMARY_ACTIVE_2_START PRIMARY_P0_START
#define PRIMARY_ACTIVE_3_START PRIMARY_P0_START
#define PRIMARY_ACTIVE_0_END   PRIMARY_P0_END
#define PRIMARY_ACTIVE_1_END   PRIMARY_P0_END
#define PRIMARY_ACTIVE_2_END   PRIMARY_P0_END
#define PRIMARY_ACTIVE_3_END   PRIMARY_P0_END

#define SECONDARY_ACTIVE_0_START SECONDARY_P0_START
#define SECONDARY_ACTIVE_1_START SECONDARY_P0_START
#define SECONDARY_ACTIVE_2_START SECONDARY_P0_START
#define SECONDARY_ACTIVE_3_START SECONDARY_P0_START
#define SECONDARY_ACTIVE_0_END   SECONDARY_P0_END
#define SECONDARY_ACTIVE_1_END   SECONDARY_P0_END
#define SECONDARY_ACTIVE_2_END   SECONDARY_P0_END
#define SECONDARY_ACTIVE_3_END   SECONDARY_P0_END

#elif MCUBOOT_IMAGE_NUMBER == 2

#define PRIMARY_ACTIVE_0_START MIN(PRIMARY_P0_START, PRIMARY_P1_START)
#define PRIMARY_ACTIVE_1_START MAX(PRIMARY_P0_START, PRIMARY_P1_START)
#define PRIMARY_ACTIVE_2_START PRIMARY_ACTIVE_1_START
#define PRIMARY_ACTIVE_3_START PRIMARY_ACTIVE_1_START
#define PRIMARY_ACTIVE_0_END                                                                       \
	((PRIMARY_P0_START <= PRIMARY_P1_START) ? PRIMARY_P0_END : PRIMARY_P1_END)
#define PRIMARY_ACTIVE_1_END                                                                       \
	((PRIMARY_P0_START >= PRIMARY_P1_START) ? PRIMARY_P0_END : PRIMARY_P1_END)
#define PRIMARY_ACTIVE_2_END PRIMARY_ACTIVE_1_END
#define PRIMARY_ACTIVE_3_END PRIMARY_ACTIVE_1_END

#define SECONDARY_ACTIVE_0_START MIN(SECONDARY_P0_START, SECONDARY_P1_START)
#define SECONDARY_ACTIVE_1_START MAX(SECONDARY_P0_START, SECONDARY_P1_START)
#define SECONDARY_ACTIVE_2_START SECONDARY_ACTIVE_1_START
#define SECONDARY_ACTIVE_3_START SECONDARY_ACTIVE_1_START
#define SECONDARY_ACTIVE_0_END                                                                     \
	((SECONDARY_P0_START <= SECONDARY_P1_START) ? SECONDARY_P0_END : SECONDARY_P1_END)
#define SECONDARY_ACTIVE_1_END                                                                     \
	((SECONDARY_P0_START >= SECONDARY_P1_START) ? SECONDARY_P0_END : SECONDARY_P1_END)
#define SECONDARY_ACTIVE_2_END SECONDARY_ACTIVE_1_END
#define SECONDARY_ACTIVE_3_END SECONDARY_ACTIVE_1_END

#elif MCUBOOT_IMAGE_NUMBER == 3

#define PRIMARY_ACTIVE_0_START MIN(MIN(PRIMARY_P0_START, PRIMARY_P1_START), PRIMARY_P2_START)
#define PRIMARY_ACTIVE_1_START MEDIAN3(PRIMARY_P0_START, PRIMARY_P1_START, PRIMARY_P2_START)
#define PRIMARY_ACTIVE_2_START MAX(MAX(PRIMARY_P0_START, PRIMARY_P1_START), PRIMARY_P2_START)
#define PRIMARY_ACTIVE_3_START PRIMARY_ACTIVE_2_START
#define PRIMARY_ACTIVE_0_END                                                                       \
	END_FOR_START(PRIMARY_P0_START, PRIMARY_P0_END, PRIMARY_P1_START, PRIMARY_P1_END,          \
		      PRIMARY_P2_START, PRIMARY_P2_END, PRIMARY_P2_START, PRIMARY_P2_END,          \
		      PRIMARY_ACTIVE_0_START)
#define PRIMARY_ACTIVE_1_END                                                                       \
	END_FOR_START(PRIMARY_P0_START, PRIMARY_P0_END, PRIMARY_P1_START, PRIMARY_P1_END,          \
		      PRIMARY_P2_START, PRIMARY_P2_END, PRIMARY_P2_START, PRIMARY_P2_END,          \
		      PRIMARY_ACTIVE_1_START)
#define PRIMARY_ACTIVE_2_END                                                                       \
	END_FOR_START(PRIMARY_P0_START, PRIMARY_P0_END, PRIMARY_P1_START, PRIMARY_P1_END,          \
		      PRIMARY_P2_START, PRIMARY_P2_END, PRIMARY_P2_START, PRIMARY_P2_END,          \
		      PRIMARY_ACTIVE_2_START)
#define PRIMARY_ACTIVE_3_END PRIMARY_ACTIVE_2_END

#define SECONDARY_ACTIVE_0_START                                                                   \
	MIN(MIN(SECONDARY_P0_START, SECONDARY_P1_START), SECONDARY_P2_START)
#define SECONDARY_ACTIVE_1_START MEDIAN3(SECONDARY_P0_START, SECONDARY_P1_START, SECONDARY_P2_START)
#define SECONDARY_ACTIVE_2_START                                                                   \
	MAX(MAX(SECONDARY_P0_START, SECONDARY_P1_START), SECONDARY_P2_START)
#define SECONDARY_ACTIVE_3_START SECONDARY_ACTIVE_2_START
#define SECONDARY_ACTIVE_0_END                                                                     \
	END_FOR_START(SECONDARY_P0_START, SECONDARY_P0_END, SECONDARY_P1_START, SECONDARY_P1_END,  \
		      SECONDARY_P2_START, SECONDARY_P2_END, SECONDARY_P2_START, SECONDARY_P2_END,  \
		      SECONDARY_ACTIVE_0_START)
#define SECONDARY_ACTIVE_1_END                                                                     \
	END_FOR_START(SECONDARY_P0_START, SECONDARY_P0_END, SECONDARY_P1_START, SECONDARY_P1_END,  \
		      SECONDARY_P2_START, SECONDARY_P2_END, SECONDARY_P2_START, SECONDARY_P2_END,  \
		      SECONDARY_ACTIVE_1_START)
#define SECONDARY_ACTIVE_2_END                                                                     \
	END_FOR_START(SECONDARY_P0_START, SECONDARY_P0_END, SECONDARY_P1_START, SECONDARY_P1_END,  \
		      SECONDARY_P2_START, SECONDARY_P2_END, SECONDARY_P2_START, SECONDARY_P2_END,  \
		      SECONDARY_ACTIVE_2_START)
#define SECONDARY_ACTIVE_3_END SECONDARY_ACTIVE_2_END

#else /* MCUBOOT_IMAGE_NUMBER == 4 */

#define PRIMARY_ACTIVE_0_START                                                                     \
	SORT4_1ST(PRIMARY_P0_START, PRIMARY_P1_START, PRIMARY_P2_START, PRIMARY_P3_START)
#define PRIMARY_ACTIVE_1_START                                                                     \
	SORT4_2ND(PRIMARY_P0_START, PRIMARY_P1_START, PRIMARY_P2_START, PRIMARY_P3_START)
#define PRIMARY_ACTIVE_2_START                                                                     \
	SORT4_3RD(PRIMARY_P0_START, PRIMARY_P1_START, PRIMARY_P2_START, PRIMARY_P3_START)
#define PRIMARY_ACTIVE_3_START                                                                     \
	SORT4_4TH(PRIMARY_P0_START, PRIMARY_P1_START, PRIMARY_P2_START, PRIMARY_P3_START)
#define PRIMARY_ACTIVE_0_END                                                                       \
	END_FOR_START(PRIMARY_P0_START, PRIMARY_P0_END, PRIMARY_P1_START, PRIMARY_P1_END,          \
		      PRIMARY_P2_START, PRIMARY_P2_END, PRIMARY_P3_START, PRIMARY_P3_END,          \
		      PRIMARY_ACTIVE_0_START)
#define PRIMARY_ACTIVE_1_END                                                                       \
	END_FOR_START(PRIMARY_P0_START, PRIMARY_P0_END, PRIMARY_P1_START, PRIMARY_P1_END,          \
		      PRIMARY_P2_START, PRIMARY_P2_END, PRIMARY_P3_START, PRIMARY_P3_END,          \
		      PRIMARY_ACTIVE_1_START)
#define PRIMARY_ACTIVE_2_END                                                                       \
	END_FOR_START(PRIMARY_P0_START, PRIMARY_P0_END, PRIMARY_P1_START, PRIMARY_P1_END,          \
		      PRIMARY_P2_START, PRIMARY_P2_END, PRIMARY_P3_START, PRIMARY_P3_END,          \
		      PRIMARY_ACTIVE_2_START)
#define PRIMARY_ACTIVE_3_END                                                                       \
	END_FOR_START(PRIMARY_P0_START, PRIMARY_P0_END, PRIMARY_P1_START, PRIMARY_P1_END,          \
		      PRIMARY_P2_START, PRIMARY_P2_END, PRIMARY_P3_START, PRIMARY_P3_END,          \
		      PRIMARY_ACTIVE_3_START)

#define SECONDARY_ACTIVE_0_START                                                                   \
	SORT4_1ST(SECONDARY_P0_START, SECONDARY_P1_START, SECONDARY_P2_START, SECONDARY_P3_START)
#define SECONDARY_ACTIVE_1_START                                                                   \
	SORT4_2ND(SECONDARY_P0_START, SECONDARY_P1_START, SECONDARY_P2_START, SECONDARY_P3_START)
#define SECONDARY_ACTIVE_2_START                                                                   \
	SORT4_3RD(SECONDARY_P0_START, SECONDARY_P1_START, SECONDARY_P2_START, SECONDARY_P3_START)
#define SECONDARY_ACTIVE_3_START                                                                   \
	SORT4_4TH(SECONDARY_P0_START, SECONDARY_P1_START, SECONDARY_P2_START, SECONDARY_P3_START)
#define SECONDARY_ACTIVE_0_END                                                                     \
	END_FOR_START(SECONDARY_P0_START, SECONDARY_P0_END, SECONDARY_P1_START, SECONDARY_P1_END,  \
		      SECONDARY_P2_START, SECONDARY_P2_END, SECONDARY_P3_START, SECONDARY_P3_END,  \
		      SECONDARY_ACTIVE_0_START)
#define SECONDARY_ACTIVE_1_END                                                                     \
	END_FOR_START(SECONDARY_P0_START, SECONDARY_P0_END, SECONDARY_P1_START, SECONDARY_P1_END,  \
		      SECONDARY_P2_START, SECONDARY_P2_END, SECONDARY_P3_START, SECONDARY_P3_END,  \
		      SECONDARY_ACTIVE_1_START)
#define SECONDARY_ACTIVE_2_END                                                                     \
	END_FOR_START(SECONDARY_P0_START, SECONDARY_P0_END, SECONDARY_P1_START, SECONDARY_P1_END,  \
		      SECONDARY_P2_START, SECONDARY_P2_END, SECONDARY_P3_START, SECONDARY_P3_END,  \
		      SECONDARY_ACTIVE_2_START)
#define SECONDARY_ACTIVE_3_END                                                                     \
	END_FOR_START(SECONDARY_P0_START, SECONDARY_P0_END, SECONDARY_P1_START, SECONDARY_P1_END,  \
		      SECONDARY_P2_START, SECONDARY_P2_END, SECONDARY_P3_START, SECONDARY_P3_END,  \
		      SECONDARY_ACTIVE_3_START)

#endif

#define CHECK_MPC_ADDRESS_ALIGNMENT(_name_str, _addr)                                              \
	BUILD_ASSERT(((_addr) % OVERRIDE_ALIGNMENT) == 0,                                          \
		     _name_str " is not aligned to " STRINGIFY(OVERRIDE_ALIGNMENT_KB) "KiB")

CHECK_MPC_ADDRESS_ALIGNMENT("Bootloader partition", ACCESSIBLE_MRAM_START);
CHECK_MPC_ADDRESS_ALIGNMENT("MRAM end / secure storage", ACCESSIBLE_MRAM_END);
CHECK_MPC_ADDRESS_ALIGNMENT("Bootloader partition", BOOT_PARTITION_END);

#if defined(CONFIG_SOC_SERIES_NRF92) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(secure_storage_partition))
/* The nRF92 replacement for the end-of-MRAM requirement; see ACCESSIBLE_MRAM_END above. */
BUILD_ASSERT(PARTITION_ADDRESS(secure_storage_partition) >= BOOT_PARTITION_END,
	     "Secure storage must be placed above the write protected bootloader region.");
#endif
CHECK_MPC_ADDRESS_ALIGNMENT("At least one primary image partition", PRIMARY_ACTIVE_0_START);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one primary image partition", PRIMARY_ACTIVE_0_END);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one primary image partition", PRIMARY_ACTIVE_1_START);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one primary image partition", PRIMARY_ACTIVE_1_END);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one primary image partition", PRIMARY_ACTIVE_2_START);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one primary image partition", PRIMARY_ACTIVE_2_END);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one primary image partition", PRIMARY_ACTIVE_3_START);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one primary image partition", PRIMARY_ACTIVE_3_END);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one secondary image partition", SECONDARY_ACTIVE_0_START);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one secondary image partition", SECONDARY_ACTIVE_0_END);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one secondary image partition", SECONDARY_ACTIVE_1_START);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one secondary image partition", SECONDARY_ACTIVE_1_END);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one secondary image partition", SECONDARY_ACTIVE_2_START);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one secondary image partition", SECONDARY_ACTIVE_2_END);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one secondary image partition", SECONDARY_ACTIVE_3_START);
CHECK_MPC_ADDRESS_ALIGNMENT("At least one secondary image partition", SECONDARY_ACTIVE_3_END);

/* MPC overrides used to implement image write protection.
 *
 * See the explanations on the various tables below to understand how they are used.
 * The main basis for selecting these is that they all accept OWNERID = None, meaning
 * that we can use them to implement write protection without forcing owner based isolation.
 * Overrides 1 and 2 are those used by IronSide SE to give full RWX access to MRAM in the
 * default configuration - therefore overwriting those overwrites the defaults.
 */

#if defined(CONFIG_SOC_SERIES_NRF92)

/* IronSide SE on nRF92 validates every MPCCONF entry against an allow list, and on this part
 * that list is overrides 4, 5, 6, 7 and 29 of MPC110 -- measured on an nRF9251 DK against
 * IronSide SE 23.7.99-SHA:410d750+127, since no allow list ships in the SoC binary bundles.
 * Anything else fails the boot with UICR status ERROR_CONFIG, regid UICR.MPCCONF and detail
 * status MPCCONF_ERROR_REGISTER_NOT_PERMITTED.
 *
 * Only three of the five are usable here, because two are carrying grants that must survive:
 *
 *   Override 5 grants the application RWX over the MRAM above secure storage. Overwriting it
 *   would remove the application's access to its own settings and DFU area.
 *   Override 6 grants RWX over all application RAM with OWNERID 0, which is what lets the
 *   Cellular domain reach the IPC shared-memory windows. Overwriting it breaks the modem.
 *
 * So 4, 7 and 29 are what these tables may use, which is exactly enough for a single image:
 * the default R_X region, the gap between the protected region and the active image, and the
 * tail from the active image to the start of secure storage. The inter-image gap overrides
 * are left at their nRF54H20 indices and are unreachable here, which the BUILD_ASSERT below
 * enforces.
 */
BUILD_ASSERT(MCUBOOT_IMAGE_NUMBER == 1,
	     "On nRF92 only three MPC110 overrides are both allow-listed and free (4, 7, 29), "
	     "which is enough for a single image only.");

#define MPC110_OVERRIDE_DEFAULT_RX                       (uintptr_t)&NRF_MPC110->OVERRIDE[4]
#define MPC110_OVERRIDE_INITIAL_END_TO_ACTIVE0_START_RWX (uintptr_t)&NRF_MPC110->OVERRIDE[29]
#define MPC110_OVERRIDE_LAST_ACTIVE_END_TO_ACCESSIBLE_MRAM_END_RWX                                 \
	(uintptr_t)&NRF_MPC110->OVERRIDE[7]

/* Saana HAC AXI0 wires overrides 4/5/6/7/29 to master ports 0 mAXI_2_M, 1 mAX2X_APP_GD,
 * 2 mAX2X_SEC_GD, 6 mAX2X_TDD_GD, 7 mAXONS_NN and 8 mAXONS_DSP. Bit 2 is the one IronSide SE
 * itself uses for every override it programs: read back as 0x00000004 from overrides 4, 5 and
 * 6 on a DK, and writes with that bit set read back unchanged. An earlier reading of the HAC
 * table concluded port 2 was not connected and that writing it would fail with
 * MPCCONF_ERROR_READBACK_MISMATCH; measurement disagrees.
 */
#define MASTERPORT_DEFAULT BIT(2)

/* The Saana HAC AXI0 override table fixes OWNER = APP for overrides 4 and 29 and marks it
 * programmable only for 6. Writing NRF_OWNER_NONE into a fixed-owner field is what IronSide
 * SE refuses, so these entries name the application explicitly. On this part that costs
 * nothing: the application core is the only owner these ranges are for.
 */
#define MPCCONF_OWNER NRF_OWNER_APPLICATION

#else /* nRF54H20 */

/* Override used for assigning R_X perms as a default. */
#define MPC110_OVERRIDE_DEFAULT_RX (uintptr_t)&NRF_MPC110->OVERRIDE[1]

/* Override used for assigning RWX perms to the range between the first write protected
 * region (bootloader code area by default, but can be extended) to the start of the first
 * active image area.
 */
#define MPC110_OVERRIDE_INITIAL_END_TO_ACTIVE0_START_RWX (uintptr_t)&NRF_MPC110->OVERRIDE[2]

/* Override used for assigning RWX perms between the end of the lower image and the start of
 * the next image by address (first inter-image gap when MCUBOOT_IMAGE_NUMBER > 1).
 */
#define MPC110_OVERRIDE_ACTIVE0_END_TO_ACTIVE1_START_RWX (uintptr_t)&NRF_MPC110->OVERRIDE[6]

/* Override used for assigning RWX perms between the end of the middle image and the start
 * of the next image by address (second inter-image gap when MCUBOOT_IMAGE_NUMBER > 2).
 */
#define MPC110_OVERRIDE_ACTIVE1_END_TO_ACTIVE2_START_RWX (uintptr_t)&NRF_MPC110->OVERRIDE[7]

/* Override used for assigning RWX perms between the end of the third image by address and the
 * start of the fourth (third inter-image gap when MCUBOOT_IMAGE_NUMBER > 3).
 */
#define MPC110_OVERRIDE_ACTIVE2_END_TO_ACTIVE3_START_RWX (uintptr_t)&NRF_MPC110->OVERRIDE[10]

/* Override used for assigning RWX perms to the range between the end of the last active
 * image area and the end of the available MRAM.
 */
#define MPC110_OVERRIDE_LAST_ACTIVE_END_TO_ACCESSIBLE_MRAM_END_RWX                                 \
	(uintptr_t)&NRF_MPC110->OVERRIDE[11]

/* Default masterport settings for the above MPC110 overrides. */
#define MASTERPORT_DEFAULT (BIT(2) | BIT(3) | BIT(6))

#define MPCCONF_OWNER NRF_OWNER_NONE

#endif /* CONFIG_SOC_SERIES_NRF92 */

/* The 3 (2 if !DIRECT_XIP) tables below this implement the access permissions at the different
 * stages of the boot:
 *
 * - UICR.MPCCONF is loaded by IronSide SE before starting MCUboot.
 * - Slot 0 entries are loaded by MCUboot before starting the slot 0 application.
 *   This overwrites the UICR.MPCCONF configuration.
 * - (DIRECT_XIP only) Slot 1 entries are loaded by MCUboot before starting the slot 1 application.
 *   This overwrites the UICR.MPCCONF configuration.
 */

/* MPC override configuration set in UICR MPCCONF, which is loaded before MCUboot is started,
 * and is active until MCUboot loads the final configuration via IronSide SE IPC.
 * This configuration is overwritten again by MCUboot before jumping to the application, so
 * it is only active between when MCUboot is started at cold boot and before jumping to the
 * application. The goal of this configuration is to write protect the bootloader code partition
 * from before the Application core is booted by IronSide SE:
 *
 * |----------------------------------------------------------------------------------------|
 * | MRAM                                                                                   |
 * | Bootloader |                                                         | (SECURESTORAGE) |
 * |----------------------------------------------------------------------------------------|
 * |    RX      |                                                         |                 |
 * |----------------------------------------------------------------------------------------|
 * |            |                          RW                             |                 |
 * |----------------------------------------------------------------------------------------|
 *
 * Override 1: R_X | [boot partition start - boot partition end]
 * Override 2: RW  | [boot partition end - end of accessible MRAM]
 *
 * The '.mpcconf_entry' section is where the UICR generator image looks for static MPCCONF
 * entries to be extracted to UICR MPCCONF.
 */
static const struct mpcconf_entry uicr_entries[] __used Z_GENERIC_DOT_SECTION(mpcconf_entry) = {
	/* R_X | [boot start - boot end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ false,
					    /* ENABLE */ true, MPC110_OVERRIDE_DEFAULT_RX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ false,
					    /* X */ true,
					    /* S */ false, ACCESSIBLE_MRAM_START),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, BOOT_PARTITION_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},

#if ACCESSIBLE_MRAM_END > BOOT_PARTITION_END
	/* RW | [boot end - user end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ false, /* ENABLE */ true,
			MPC110_OVERRIDE_LAST_ACTIVE_END_TO_ACCESSIBLE_MRAM_END_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ false,
					    /* S */ false, BOOT_PARTITION_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, ACCESSIBLE_MRAM_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
	/* UICR.MPCCONF.MAXCOUNT may be larger than this array. We expect the UICR generator tool
	 * to fill the rest of the MPCCONF blob with 0xFFFF_FFFF.
	 */
};

/* The table(s) below contain the MPC110 override configuration loaded by MCUboot right before
 * booting the images, and represents the final MPC configuration (overwriting that set using UICR).
 *
 * The MPC override setup assumes a partition layout as described below.
 * Empty columns represent either inactive images or other space between the adjacent partitions.
 * Parentheses represent optional components.
 *
 * |----------------------------------------------------------------------------------------|
 * | MRAM                                                                                   |
 * | Bootloader |   | Act 0 |   | (Act 1) |   | (Act 2) |   | (Act 3) |   | (SECURESTORAGE) |
 * |----------------------------------------------------------------------------------------|
 * |                   RX                                                 |                 |
 * |----------------------------------------------------------------------------------------|
 * |            |RWX|       |RWX|         |RWX|         |RWX|         |RWX|                 |
 * |----------------------------------------------------------------------------------------|
 *
 * The goal of the override configuration is to disable write access to the active firmware regions
 * (bootloader + images contained in the slot) and keep write access enabled for other parts of
 * MRAM.
 *
 * Override 1:  R_X | [boot partition start - end of accessible MRAM]
 * Override 2:  RWX | [boot partition end - ACTIVE_0 partition start]
 * Override 6:  RWX | [ACTIVE_0 end - ACTIVE_1 start] (first inter-image gap when N > 1)
 * Override 7:  RWX | [ACTIVE_1 end - ACTIVE_2 start] (second inter-image gap when N > 2)
 * Override 10: RWX | [ACTIVE_2 end - ACTIVE_3 start] (third inter-image gap when N > 3)
 * Override 11: RWX | [last active partition end - end of accessible MRAM]
 *
 * Note that the effect of overlapping the R_X with RWX is RWX (perms are OR-ed).
 */
static const struct mpcconf_entry primary_entries[] = {
	/* R_X | [boot start - user end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ true,
					    /* ENABLE */ true, MPC110_OVERRIDE_DEFAULT_RX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ false,
					    /* X */ true,
					    /* S */ false, ACCESSIBLE_MRAM_START),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, ACCESSIBLE_MRAM_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},

#if PRIMARY_ACTIVE_0_START > BOOT_PARTITION_END
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true, /* ENABLE */ true,
			MPC110_OVERRIDE_INITIAL_END_TO_ACTIVE0_START_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, BOOT_PARTITION_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, PRIMARY_ACTIVE_0_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif

#if MCUBOOT_IMAGE_NUMBER > 1
#if PRIMARY_ACTIVE_1_START > PRIMARY_ACTIVE_0_END
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ true,
					    /* ENABLE */ true,
					    MPC110_OVERRIDE_ACTIVE0_END_TO_ACTIVE1_START_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, PRIMARY_ACTIVE_0_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, PRIMARY_ACTIVE_1_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#endif /* MCUBOOT_IMAGE_NUMBER > 1 */

#if MCUBOOT_IMAGE_NUMBER > 2
#if PRIMARY_ACTIVE_2_START > PRIMARY_ACTIVE_1_END
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ true,
					    /* ENABLE */ true,
					    MPC110_OVERRIDE_ACTIVE1_END_TO_ACTIVE2_START_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, PRIMARY_ACTIVE_1_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, PRIMARY_ACTIVE_2_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#endif /* MCUBOOT_IMAGE_NUMBER > 2 */

#if MCUBOOT_IMAGE_NUMBER > 3
#if PRIMARY_ACTIVE_3_START > PRIMARY_ACTIVE_2_END
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ true,
					    /* ENABLE */ true,
					    MPC110_OVERRIDE_ACTIVE2_END_TO_ACTIVE3_START_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, PRIMARY_ACTIVE_2_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, PRIMARY_ACTIVE_3_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#endif /* MCUBOOT_IMAGE_NUMBER > 3 */

#if ACCESSIBLE_MRAM_END > PRIMARY_ACTIVE_3_END
	/* RWX | [ACTIVE_3 end - user end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true, /* ENABLE */ true,
			MPC110_OVERRIDE_LAST_ACTIVE_END_TO_ACCESSIBLE_MRAM_END_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, PRIMARY_ACTIVE_3_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, ACCESSIBLE_MRAM_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
};

#if defined(MCUBOOT_DIRECT_XIP)
static const struct mpcconf_entry secondary_entries[] = {
	/* R_X | [boot start - user end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(/* LOCK */ true, /* ENABLE */ true,
					    MPC110_OVERRIDE_DEFAULT_RX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ false,
					    /* X */ true,
					    /* S */ false, ACCESSIBLE_MRAM_START),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, ACCESSIBLE_MRAM_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},

#if SECONDARY_ACTIVE_0_START > BOOT_PARTITION_END
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true, /* ENABLE */ true,
			MPC110_OVERRIDE_INITIAL_END_TO_ACTIVE0_START_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, BOOT_PARTITION_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, SECONDARY_ACTIVE_0_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif

#if MCUBOOT_IMAGE_NUMBER > 1
#if SECONDARY_ACTIVE_1_START > SECONDARY_ACTIVE_0_END
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true,
			/* ENABLE */ true, MPC110_OVERRIDE_ACTIVE0_END_TO_ACTIVE1_START_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(
			/* R */ true, /* W */ true, /* X */ true,
			/* S */ false, SECONDARY_ACTIVE_0_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, SECONDARY_ACTIVE_1_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#endif /* MCUBOOT_IMAGE_NUMBER > 1 */

#if MCUBOOT_IMAGE_NUMBER > 2
#if SECONDARY_ACTIVE_2_START > SECONDARY_ACTIVE_1_END
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true,
			/* ENABLE */ true, MPC110_OVERRIDE_ACTIVE1_END_TO_ACTIVE2_START_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(
			/* R */ true, /* W */ true, /* X */ true,
			/* S */ false, SECONDARY_ACTIVE_1_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, SECONDARY_ACTIVE_2_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#endif /* MCUBOOT_IMAGE_NUMBER > 2 */

#if MCUBOOT_IMAGE_NUMBER > 3
#if SECONDARY_ACTIVE_3_START > SECONDARY_ACTIVE_2_END
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true,
			/* ENABLE */ true, MPC110_OVERRIDE_ACTIVE2_END_TO_ACTIVE3_START_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(
			/* R */ true, /* W */ true, /* X */ true,
			/* S */ false, SECONDARY_ACTIVE_2_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, SECONDARY_ACTIVE_3_START),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
#endif /* MCUBOOT_IMAGE_NUMBER > 3 */

#if ACCESSIBLE_MRAM_END > SECONDARY_ACTIVE_3_END
	/* RWX | [ACTIVE_3 end - user end] */
	{
		MPCCONF_ENTRY_CONFIG0_VALUE(
			/* LOCK */ true, /* ENABLE */ true,
			MPC110_OVERRIDE_LAST_ACTIVE_END_TO_ACCESSIBLE_MRAM_END_RWX),
		MPCCONF_ENTRY_CONFIG1_VALUE(/* R */ true, /* W */ true,
					    /* X */ true,
					    /* S */ false, SECONDARY_ACTIVE_3_END),
		MPCCONF_ENTRY_CONFIG2_VALUE(MPCCONF_OWNER, ACCESSIBLE_MRAM_END),
		MPCCONF_ENTRY_CONFIG3_VALUE(MASTERPORT_DEFAULT),
	},
#endif
};
#endif /* defined(MCUBOOT_DIRECT_XIP) */

#ifdef MCUBOOT_DIRECT_XIP
/* Slot to load MPCCONF for. */
static enum boot_slot mpcconf_slot = BOOT_SLOT_PRIMARY;

int nrf_mpcconf_update_active_slot(const struct boot_rsp *rsp)
{
	int rc;
	uintptr_t flash_base;

	rc = flash_device_base(rsp->br_flash_dev_id, &flash_base);
	if (rc != 0) {
		return -1;
	}

	const uintptr_t abs_addr = flash_base + rsp->br_image_off;

	if (IN_RANGE(abs_addr, SECONDARY_P0_START, SECONDARY_P0_END - 1)
#if MCUBOOT_IMAGE_NUMBER > 1
	    || IN_RANGE(abs_addr, SECONDARY_P1_START, SECONDARY_P1_END - 1)
#endif
#if MCUBOOT_IMAGE_NUMBER > 2
	    || IN_RANGE(abs_addr, SECONDARY_P2_START, SECONDARY_P2_END - 1)
#endif
#if MCUBOOT_IMAGE_NUMBER > 3
	    || IN_RANGE(abs_addr, SECONDARY_P3_START, SECONDARY_P3_END - 1)
#endif
	) {
		mpcconf_slot = BOOT_SLOT_SECONDARY;
	} else {
		mpcconf_slot = BOOT_SLOT_PRIMARY;
	}

	return 0;
}
#endif

/* @warning This function should always be called while executing from an area in
 * memory where permissions are not managed through the global domain MPCs (e.g. local RAM).
 * In the future this function may remove MCUboot's access to global domain memory.
 * Therefore, do not access any global memory during or after the call to
 * ironside_se_mpcconf_write().
 */
int __ramfunc nrf_load_mpcconf(void)
{
	int rc;
	struct ironside_se_mpcconf_status status = {0};

	size_t num_entries;
	const struct mpcconf_entry *entries = NULL;

#ifdef MCUBOOT_DIRECT_XIP
	switch (mpcconf_slot) {
	case BOOT_SLOT_PRIMARY:
		entries = primary_entries;
		num_entries = ARRAY_SIZE(primary_entries);
		break;
	case BOOT_SLOT_SECONDARY:
		entries = secondary_entries;
		num_entries = ARRAY_SIZE(secondary_entries);
		break;
	default:
		/* Should not be possible. */
		return -1;
	}
#else
	entries = primary_entries;
	num_entries = ARRAY_SIZE(primary_entries);
#endif /* MCUBOOT_DIRECT_XIP */

	status = ironside_se_mpcconf_write(entries, num_entries);

	/* Finalization should be done regardless of the write result. */
	rc = ironside_se_mpcconf_finish_init();

	if (rc != 0 || status.status != 0) {
		return -1;
	}

	return 0;
}
