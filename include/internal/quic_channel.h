/*
 * Copyright 2022 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#ifndef OSSL_QUIC_CHANNEL_H
# define OSSL_QUIC_CHANNEL_H

# include <openssl/ssl.h>
# include "internal/quic_types.h"
# include "internal/quic_stream_map.h"
# include "internal/quic_reactor.h"
# include "internal/quic_statm.h"
# include "internal/time.h"
# include "internal/thread.h"

# ifndef OPENSSL_NO_QUIC

/*
 * QUIC Channel
 * ============
 *
 * A QUIC channel (QUIC_CHANNEL) is an object which binds together all of the
 * various pieces of QUIC into a single top-level object, and handles connection
 * state which is not specific to the client or server roles. In particular, it
 * is strictly separated from the libssl front end I/O API personality layer,
 * and is not an SSL object.
 *
 * The name QUIC_CHANNEL is chosen because QUIC_CONNECTION is already in use,
 * but functionally these relate to the same thing (a QUIC connection). The use
 * of two separate objects ensures clean separation between the API personality
 * layer and common code for handling connections, and between the functionality
 * which is specific to clients and which is specific to servers, and the
 * functionality which is common to both.
 *
 * The API personality layer provides SSL objects (e.g. a QUIC_CONNECTION) which
 * consume a QUIC channel and implement a specific public API. Things which are
 * handled by the API personality layer include emulation of blocking semantics,
 * handling of SSL object mode flags like non-partial write mode, etc.
 *
 * Where the QUIC_CHANNEL is used in a server role, there is one QUIC_CHANNEL
 * per connection. In the future a QUIC Channel Manager will probably be defined
 * to handle ownership of resources which are shared between connections (e.g.
 * demuxers). Since we only use server-side functionality for dummy test servers
 * for now, which only need to handle one connection at a time, this is not
 * currently modelled.
 *
 * Synchronisation
 * ---------------
 *
 * To support thread assisted mode, QUIC_CHANNEL can be used by multiple
 * threads. **It is the caller's responsibility to ensure that the QUIC_CHANNEL
 * is only accessed (whether via its methods or via direct access to its state)
 * while the channel mutex is held**, except for methods explicitly marked as
 * not requiring prior locking. This is an unchecked precondition.
 *
 * The instantiator of the channel is responsible for providing a suitable
 * mutex which then serves as the channel mutex; see QUIC_CHANNEL_ARGS.
 */

/*
 * The function does not acquire the channel mutex and assumes it is already
 * held by the calling thread.
 *
 * Any function tagged with this has the following precondition:
 *
 *   Precondition: must hold channel mutex (unchecked)
 */
#  define QUIC_NEEDS_LOCK

/*
 * The function acquires the channel mutex and releases it before returning in
 * all circumstances.
 *
 * Any function tagged with this has the following precondition and
 * postcondition:
 *
 *   Precondition: must not hold channel mutex (unchecked)
 *   Postcondition: channel mutex is not held (by calling thread)
 */
#  define QUIC_TAKES_LOCK

/*
 * The function acquires the channel mutex and leaves it acquired
 * when returning success.
 *
 * Any function tagged with this has the following precondition and
 * postcondition:
 *
 *   Precondition: must not hold channel mutex (unchecked)
 *   Postcondition: channel mutex is held by calling thread
 *      or function returned failure
 */
#  define QUIC_ACQUIRES_LOCK

#  define QUIC_TODO_LOCK

#  define QUIC_CHANNEL_STATE_IDLE                        0
#  define QUIC_CHANNEL_STATE_ACTIVE                      1
#  define QUIC_CHANNEL_STATE_TERMINATING_CLOSING         2
#  define QUIC_CHANNEL_STATE_TERMINATING_DRAINING        3
#  define QUIC_CHANNEL_STATE_TERMINATED                  4

typedef struct quic_channel_args_st {
    OSSL_LIB_CTX    *libctx;
    const char      *propq;
    int             is_server;
    SSL             *tls;

    /*
     * This must be a mutex the lifetime of which will exceed that of the
     * channel. The instantiator of the channel is responsible for providing a
     * mutex as this makes it easier to handle instantiation and teardown of
     * channels in situations potentially requiring locking.
     *
     * Note that this is a MUTEX not a RWLOCK as it needs to be an OS mutex for
     * compatibility with an OS's condition variable wait API, whereas RWLOCK
     * may, depending on the build configuration, be implemented using an OS's
     * mutex primitive or using its RW mutex primitive.
     */
    CRYPTO_MUTEX    *mutex;

    /*
     * Optional function pointer to use to retrieve the current time. If NULL,
     * ossl_time_now() is used.
     */
    OSSL_TIME       (*now_cb)(void *arg);
    void            *now_cb_arg;
} QUIC_CHANNEL_ARGS;

typedef struct quic_channel_st QUIC_CHANNEL;

/* Represents the cause for a connection's termination. */
typedef struct quic_terminate_cause_st {
    /*
     * If we are in a TERMINATING or TERMINATED state, this is the error code
     * associated with the error. This field is valid iff we are in the
     * TERMINATING or TERMINATED states.
     */
    uint64_t                        error_code;

    /*
     * If terminate_app is set and this is nonzero, this is the frame type which
     * caused the connection to be terminated.
     */
    uint64_t                        frame_type;

    /* Is this error code in the transport (0) or application (1) space? */
    unsigned int                    app : 1;

    /*
     * If set, the cause of the termination is a received CONNECTION_CLOSE
     * frame. Otherwise, we decided to terminate ourselves and sent a
     * CONNECTION_CLOSE frame (regardless of whether the peer later also sends
     * one).
     */
    unsigned int                    remote : 1;
} QUIC_TERMINATE_CAUSE;


/*
 * Create a new QUIC channel using the given arguments. The argument structure
 * does not need to remain allocated. Returns NULL on failure.
 */
QUIC_CHANNEL *ossl_quic_channel_new(const QUIC_CHANNEL_ARGS *args);

/* No-op if ch is NULL. */
void ossl_quic_channel_free(QUIC_CHANNEL *ch);

/* Set mutator callbacks for test framework support */
int ossl_quic_channel_set_mutator(QUIC_CHANNEL *ch,
                                  ossl_mutate_packet_cb mutatecb,
                                  ossl_finish_mutate_cb finishmutatecb,
                                  void *mutatearg);

/*
 * Connection Lifecycle Events
 * ===========================
 *
 * Various events that can be raised on the channel by other parts of the QUIC
 * implementation. Some of these are suitable for general use by any part of the
 * code (e.g. ossl_quic_channel_raise_protocol_error), others are for very
 * specific use by particular components only (e.g.
 * ossl_quic_channel_on_handshake_confirmed).
 */

/*
 * To be used by a QUIC connection. Starts the channel. For a client-mode
 * channel, this starts sending the first handshake layer message, etc. Can only
 * be called in the idle state; successive calls are ignored.
 */
int ossl_quic_channel_start(QUIC_CHANNEL *ch);

/* Start a locally initiated connection shutdown. */
void ossl_quic_channel_local_close(QUIC_CHANNEL *ch, uint64_t app_error_code);

/*
 * Called when the handshake is confirmed.
 */
int ossl_quic_channel_on_handshake_confirmed(QUIC_CHANNEL *ch);

/*
 * Raises a protocol error. This is intended to be the universal call suitable
 * for handling of all peer-triggered protocol violations or errors detected by
 * us. We specify a QUIC transport-scope error code and optional frame type
 * which was responsible. If a frame type is not applicable, specify zero. The
 * reason string is not currently handled, but should be a string of static
 * storage duration. If the connection has already terminated due to a previous
 * protocol error, this is a no-op; first error wins.
 */
void ossl_quic_channel_raise_protocol_error(QUIC_CHANNEL *ch,
                                            uint64_t error_code,
                                            uint64_t frame_type,
                                            const char *reason);
/*
 * Returns 1 if permanent net error was detected on the QUIC_CHANNEL,
 * 0 otherwise.
 */
int ossl_quic_channel_net_error(QUIC_CHANNEL *ch);

/* Restore saved error state (best effort) */
void ossl_quic_channel_restore_err_state(QUIC_CHANNEL *ch);

/* For RXDP use. */
void ossl_quic_channel_on_remote_conn_close(QUIC_CHANNEL *ch,
                                            OSSL_QUIC_FRAME_CONN_CLOSE *f);
void ossl_quic_channel_on_new_conn_id(QUIC_CHANNEL *ch,
                                      OSSL_QUIC_FRAME_NEW_CONN_ID *f);

/*
 * Queries and Accessors
 * =====================
 */

/* Gets the reactor which can be used to tick/poll on the channel. */
QUIC_REACTOR *ossl_quic_channel_get_reactor(QUIC_CHANNEL *ch);

/* Gets the QSM used with the channel. */
QUIC_STREAM_MAP *ossl_quic_channel_get_qsm(QUIC_CHANNEL *ch);

/* Gets the statistics manager used with the channel. */
OSSL_STATM *ossl_quic_channel_get_statm(QUIC_CHANNEL *ch);

/*
 * Gets/sets the current peer address. Generally this should be used before
 * starting a channel in client mode.
 */
int ossl_quic_channel_get_peer_addr(QUIC_CHANNEL *ch, BIO_ADDR *peer_addr);
int ossl_quic_channel_set_peer_addr(QUIC_CHANNEL *ch, const BIO_ADDR *peer_addr);

/* Gets/sets the underlying network read and write BIOs. */
BIO *ossl_quic_channel_get_net_rbio(QUIC_CHANNEL *ch);
BIO *ossl_quic_channel_get_net_wbio(QUIC_CHANNEL *ch);
int ossl_quic_channel_set_net_rbio(QUIC_CHANNEL *ch, BIO *net_rbio);
int ossl_quic_channel_set_net_wbio(QUIC_CHANNEL *ch, BIO *net_wbio);

/*
 * Returns an existing stream by stream ID. Returns NULL if the stream does not
 * exist.
 */
QUIC_STREAM *ossl_quic_channel_get_stream_by_id(QUIC_CHANNEL *ch,
                                                uint64_t stream_id);

/* Returns 1 if channel is terminating or terminated. */
int ossl_quic_channel_is_term_any(const QUIC_CHANNEL *ch);
const QUIC_TERMINATE_CAUSE *
ossl_quic_channel_get_terminate_cause(const QUIC_CHANNEL *ch);
int ossl_quic_channel_is_terminated(const QUIC_CHANNEL *ch);
int ossl_quic_channel_is_active(const QUIC_CHANNEL *ch);
int ossl_quic_channel_is_handshake_complete(const QUIC_CHANNEL *ch);
int ossl_quic_channel_is_handshake_confirmed(const QUIC_CHANNEL *ch);

QUIC_DEMUX *ossl_quic_channel_get0_demux(QUIC_CHANNEL *ch);

SSL *ossl_quic_channel_get0_ssl(QUIC_CHANNEL *ch);

/*
 * Retrieves a pointer to the channel mutex which was provided at the time the
 * channel was instantiated. In order to allow locks to be acquired and released
 * with the correct granularity, it is the caller's responsibility to ensure
 * this lock is held for write while calling any QUIC_CHANNEL method, except for
 * methods explicitly designed otherwise.
 *
 * This method is thread safe and does not require prior locking. It can also be
 * called while the lock is already held. Note that this is simply a convenience
 * function to access the mutex which was passed to the channel at instantiation
 * time; it does not belong to the channel but rather is presumed to belong to
 * the owner of the channel.
 */
CRYPTO_MUTEX *ossl_quic_channel_get_mutex(QUIC_CHANNEL *ch);

/*
 * Creates a new locally-initiated stream in the stream mapper, choosing an
 * appropriate stream ID. If is_uni is 1, creates a unidirectional stream, else
 * creates a bidirectional stream. Returns NULL on failure.
 */
QUIC_STREAM *ossl_quic_channel_new_stream_local(QUIC_CHANNEL *ch, int is_uni);

/*
 * Creates a new remotely-initiated stream in the stream mapper. The stream ID
 * is used to confirm the initiator and determine the stream type. The stream is
 * automatically added to the QSM's accept queue. A pointer to the stream is
 * also returned. Returns NULL on failure.
 */
QUIC_STREAM *ossl_quic_channel_new_stream_remote(QUIC_CHANNEL *ch,
                                                 uint64_t stream_id);

/*
 * Configures incoming stream auto-reject. If enabled, incoming streams have
 * both their sending and receiving parts automatically rejected using
 * STOP_SENDING and STREAM_RESET frames. aec is the application error
 * code to be used for those frames.
 */
void ossl_quic_channel_set_incoming_stream_auto_reject(QUIC_CHANNEL *ch,
                                                       int enable,
                                                       uint64_t aec);

/*
 * Causes the channel to reject the sending and receiving parts of a stream,
 * as though autorejected. Can be used if a stream has already been
 * accepted.
 */
void ossl_quic_channel_reject_stream(QUIC_CHANNEL *ch, QUIC_STREAM *qs);

/* Replace local connection ID in TXP and DEMUX for testing purposes. */
int ossl_quic_channel_replace_local_cid(QUIC_CHANNEL *ch,
                                        const QUIC_CONN_ID *conn_id);

/* Setters for the msg_callback and msg_callback_arg */
void ossl_quic_channel_set_msg_callback(QUIC_CHANNEL *ch,
                                        ossl_msg_cb msg_callback,
                                        SSL *msg_callback_ssl);
void ossl_quic_channel_set_msg_callback_arg(QUIC_CHANNEL *ch,
                                            void *msg_callback_arg);

/* Testing use only - sets a TXKU threshold packet count override value. */
void ossl_quic_channel_set_txku_threshold_override(QUIC_CHANNEL *ch,
                                                   uint64_t tx_pkt_threshold);

/* Testing use only - gets current 1-RTT key epochs for QTX and QRX. */
uint64_t ossl_quic_channel_get_tx_key_epoch(QUIC_CHANNEL *ch);
uint64_t ossl_quic_channel_get_rx_key_epoch(QUIC_CHANNEL *ch);

/* Artificially trigger a spontaneous TXKU if possible. */
int ossl_quic_channel_trigger_txku(QUIC_CHANNEL *ch);
int ossl_quic_channel_has_pending(const QUIC_CHANNEL *ch);

/* Force transmission of an ACK-eliciting packet. */
int ossl_quic_channel_ping(QUIC_CHANNEL *ch);

/* For testing use. While enabled, ticking is not performed. */
void ossl_quic_channel_set_inhibit_tick(QUIC_CHANNEL *ch, int inhibit);

# endif

#endif
