#ifndef LFP_RP66_H
#define LFP_RP66_H

#include <lfp/lfp.h>

#if (__cplusplus)
extern "C" {
#endif

/** Visible Envelope
 *
 * The Visible Envelope(VE) is an access mechanic from the DLIS spec, rp66v1 [1].
 *
 * A dlis file consist of a series of Visible Records(VR), each consisting of a
 * VE and one or more Logical Record Segments (LRS).
 *
 * The rp66 protocol provides a view as if the VE were not present.
 * `lfp_seek()` and `lfp_tell()` consider offsets as if the file had no VE.
 *
 * The first 80 bytes of the *first* VE consist of ASCII characters and
 * constitute a Storage Unit Label (SUL). The information in the SUL is not
 * used by this protocol. However, the SUL might be of interest to the caller.
 * Therefore the responsibility of reading the SUL is left to the caller.
 * This protocol assumes that the SUL is dealt with elsewhere, i.e. that the
 * first byte of the underlying handle is the Visible Record Length of the
 * first VE.
 *
 * [1] http://w3.energistics.org/RP66/V1/Toc/main.html
 */
lfp_protocol* lfp_rp66_open(lfp_protocol*);

#if (__cplusplus)
} // extern "C"
#endif

#endif // LFP_RP66_H
