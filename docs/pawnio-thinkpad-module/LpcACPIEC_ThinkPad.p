// PawnIO Modules - Modules for various hardware to be used with PawnIO.
// Copyright (C) 2023 namazso
// Modifications Copyright (C) 2026 <SUBMITTER>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor,
// Boston, MA 02110-1301 USA.
//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ===========================================================================
// DRAFT for an upstream PawnIO.Modules contribution. NOT compiled, hardware-
// tested, packaged, or signed. See SUBMISSION.md for rationale, the port
// policy, security analysis, build/sign steps, and open questions requiring
// maintainer/hardware confirmation before this is submitted or used.
// ===========================================================================

#include <pawnio.inc>

// Standard ACPI EC:
//   0x0062 = data
//   0x0066 = command/status
//
// Legacy ThinkPad TYPE1 EC:
//   0x1600 = data
//   0x1604 = TYPE1 EC command/status; also the H8S STR3 status the TWR path polls
//            (read-only there). NOTE: arbitrary WRITES to the TYPE1 pair are this
//            module's dominant capability - see SUBMISSION.md open question 9.
is_ec_pair_port(port) {
    return port == 0x0062 ||
           port == 0x0066 ||
           port == 0x1600 ||
           port == 0x1604;
}

// ThinkPad H8S LPC channel 3 TWR0..TWR15 register row.
// TPFanControl reads all sixteen ports, so the complete span is needed.
is_twr_port(port) {
    return port >= 0x1610 && port <= 0x161F;
}

is_port_read_allowed(port) {
    return is_ec_pair_port(port) || is_twr_port(port);
}

// The two EC command/data pairs retain the stock raw-byte semantics.
//
// The TWR row is more restricted because TWR0 selects a general H8S
// system-management function. These are the only writes performed by
// TPFanControl's observed UseTWR transaction:
//   TWR0      (0x1610) = 0x20
//   TWR1..15  (0x1611..0x161F) = 0x00
is_port_write_allowed(port, value) {
    if (is_ec_pair_port(port))
        return 1;

    if (port == 0x1610)
        return value == 0x20;

    return port >= 0x1611 &&
           port <= 0x161F &&
           value == 0x00;
}

/// Read one byte from an allowed ACPI or ThinkPad EC port.
///
/// @param in [0] = Port
/// @param in_size Must be 1
/// @param out [0] = Value read
/// @param out_size Must be 1
/// @return An NTSTATUS
/// @warning The caller must acquire the
///          "\BaseNamedObjects\Access_EC" mutant and keep it held across
///          the complete logical EC or TWR transaction.
DEFINE_IOCTL_SIZED(ioctl_pio_read, 1, 1) {
    new port = in[0] & 0xFFFF;

    if (!is_port_read_allowed(port))
        return STATUS_ACCESS_DENIED;

    out[0] = io_in_byte(port);
    return STATUS_SUCCESS;
}

/// Write one byte to an allowed ACPI or ThinkPad EC port.
///
/// @param in [0] = Port, [1] = Value
/// @param in_size Must be 2
/// @param out Unused
/// @param out_size Unused
/// @return An NTSTATUS
/// @warning The caller must acquire the
///          "\BaseNamedObjects\Access_EC" mutant and keep it held across
///          the complete logical EC or TWR transaction.
DEFINE_IOCTL_SIZED(ioctl_pio_write, 2, 0) {
    new port = in[0] & 0xFFFF;
    new value = in[1] & 0xFF;

    if (!is_port_write_allowed(port, value))
        return STATUS_ACCESS_DENIED;

    io_out_byte(port, value);
    return STATUS_SUCCESS;
}

NTSTATUS:main() {
    return STATUS_SUCCESS;
}
