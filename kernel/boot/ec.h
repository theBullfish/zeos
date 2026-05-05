/*
 * Zeos — ACPI Embedded Controller (EC) interface.
 *
 * Backs OperationRegion(EmbeddedControl) accesses from AML methods.
 * Required for _BST/_BIF/_TMP on every laptop with battery / thermal
 * data behind the EC.
 *
 * See ACPI 6.x §12 for the host-EC RD_EC / WR_EC protocol.
 */

#ifndef ZEOS_EC_H
#define ZEOS_EC_H

#include <stdint.h>

/* Initialize the EC controller with the given command and data ports.
 * If discovery via ACPI namespace failed, callers may pass the conventional
 * defaults (cmd=0x66, data=0x62). Idempotent. */
void ec_init(uint16_t cmd_port, uint16_t data_port);

/* Returns 1 if ec_init() has been called and the controller has known
 * ports. */
int ec_present(void);

/* Returns the configured command/data port pair (0 if not present). */
uint16_t ec_cmd_port(void);
uint16_t ec_data_port(void);

/* Read one byte from EC space at `offset`. Returns 0 on success, -1 on
 * timeout / not-present. */
int ec_read(uint8_t offset, uint8_t *out);

/* Write one byte to EC space at `offset`. Returns 0 on success, -1 on
 * timeout. */
int ec_write(uint8_t offset, uint8_t value);

/* Convenience: read `len` consecutive bytes starting at `offset`. */
int ec_read_block(uint8_t offset, uint8_t *buf, int len);

/* Operation counters (for shell `ec` introspection). */
uint64_t ec_ops_read(void);
uint64_t ec_ops_write(void);
uint64_t ec_ops_timeout(void);

/* Honest selftest line. Either:
 *   EC .................... ports=0xCMD/0xDATA, AML EC region=ready
 * or:
 *   EC .................... not present (no _HID PNP0C09)
 */
void ec_print_selftest_line(void);

/* Walk the AML namespace looking for a Device with _HID = "PNP0C09".
 * If found, attempt to extract command/data ports from its _CRS. If
 * _CRS parsing isn't implemented, fall back to 0x66/0x62 and log
 * honestly. Calls ec_init() with the resulting ports. Returns 1 if a
 * PNP0C09 device was found in the namespace, 0 if not (in which case
 * the EC is reported "not present"). */
int ec_discover_and_init(void);

#endif /* ZEOS_EC_H */
