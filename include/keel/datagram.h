#ifndef KEEL_DATAGRAM_H
#define KEEL_DATAGRAM_H

/*
 * datagram.h — BACK-COMPAT SHIM (Phase B, step 7B-1b).
 *
 * The datagram provider data-plane vtable (KlDatagramOps + KlDgramRxMeta/RxSlot/TxDesc +
 * KL_DGRAM_RX_*) that used to live here has moved to <keel/socket_dgram.h> — a provider-scoped
 * header on the SOCKET axis — so this header (<keel/datagram.h>) is freed for the future public
 * fixed-slot KlDatagram API (step 7B-3). Until then, and permanently for source compatibility, this
 * re-includes <keel/socket_dgram.h> so existing `#include <keel/datagram.h>` keeps compiling.
 *
 * The legacy KlUdp-embedded transport object formerly typedef'd here as `KlDatagram` was renamed
 * `KlUdpTransport` (7B-1a); its handle typedef + layout now live in <keel/udp_transport_detail.h>.
 */

#include <keel/socket_dgram.h>

#endif /* KEEL_DATAGRAM_H */
