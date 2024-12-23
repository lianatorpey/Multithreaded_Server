/*
 * TU: simulates a "telephone unit", which interfaces a client with the PBX.
 */
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pbx.h" // includes tu.h already - if i try and reinclude the recursive opening breaks
#include "debug.h"

// TU structure definition
typedef struct tu {
    int fd; // File descriptor for the client connection
    int ext; // Extension number assigned to instance of TU
    int ref_count; // Reference count for the TU, to manage lifetime
    TU_STATE state; // Current state of the TU (what it is currently doing)
    struct tu *peer; // Pointer to the peer TU in a call, if any
    pthread_mutex_t mutex; // Mutex to ensure thread-safe access - or at least trying my hardest
} TU;

// Helper function to send a message to the client connected to the given file descriptor
static void notify_client(int fd, const char *message) {
    // Write the message to the client; logs failure if write fails
    if (fd < 0) {
        fprintf(stderr, "ERROR: Invalid file descriptor, cannot send message: %s\n", message);
        return; // logs and skips invalid file descriptors
    }

    if (write(fd, message, strlen(message)) < 0) {
        fprintf(stderr, "ERROR: Failed to write to client on fd (%d) with message: %s", fd, message);
        close(fd); // close file descriptor if write fails
    }
}

void notify_client_of_tu_state(TU *tu) {
    char buffer[64]; // Sufficient size to hold dynamically constructed messages

    TU_STATE state = tu->state;
    // fprintf(stderr, "tu state: %d\n", tu->state);

    switch (state) {
        case TU_ON_HOOK:
            // Construct message with the current TU's file descriptor
            snprintf(buffer, sizeof(buffer), "ON HOOK %d\r\n", tu->fd);
            break;

        case TU_CONNECTED:
            // Construct message with the peer's extension number
            snprintf(buffer, sizeof(buffer), "CONNECTED %d\r\n", tu->peer->ext);
            break;

        default:
            // Use static messages for other states
            {
                static const char *state_messages[] = {
                    "ON_HOOK\r\n", // placholder for matching never returned (matched first case above)
                    "RINGING\r\n",
                    "DIAL_TONE\r\n",
                    "RING_BACK\r\n",
                    "BUSY_SIGNAL\r\n",
                    "CONNECTED\r\n", // placeholder for matching never returned (matched second case above)
                    "ERROR\r\n"
                };
                strncpy(buffer, state_messages[state], sizeof(buffer) - 1);
                buffer[sizeof(buffer) - 1] = '\0'; // Ensure null termination
            }
            break;
    }
    // Notify the client of the constructed message
    notify_client(tu->fd, buffer);
}

// PREVENT DEADLOCKS when working with multiple tu objects simulataneously in multithreads
// deadlock is when threads acquire locks on same set of resources in different orders
// function wrapper for safe mutex lock with multithreading with multiple tu objects
void safe_mutex_lock(TU *tu1, TU *tu2) { // use functions to ensure consistent behavior across functions that interact w/ mutliple tus
    // guarantees no matter which thread locking tus, order will always be the same

    // fprintf(stderr, "safe mutex lock\n");

    if (tu1 < tu2) { // if both threads hold respective first locks, neither can proceed to acquire second lock resulting in deadlock
        pthread_mutex_lock(&tu1->mutex);
        pthread_mutex_lock(&tu2->mutex);
    } else { // acquires lock in consistent address order
        pthread_mutex_lock(&tu2->mutex);
        pthread_mutex_lock(&tu1->mutex);
    }
} // ensures state transitions and client interactions are synchronized across threads

// function wrapper for safe mutex unlock - therefore mutexes are always acquired in the same order
// locks must be acquired in consistent, deterministic order
void safe_mutex_unlock(TU *tu1, TU *tu2) { // conditions based on memory address - always same order

    // fprintf(stderr, "safe mutex unlock\n");

    if (tu1 < tu2) { // release locks in same order as acquired
        pthread_mutex_unlock(&tu1->mutex);
        pthread_mutex_unlock(&tu2->mutex);
    } else {
        pthread_mutex_unlock(&tu2->mutex);
        pthread_mutex_unlock(&tu1->mutex);
    }
}

/*
 * Initialize a TU
 *
 * @param fd  The file descriptor of the underlying network connection.
 * @return  The TU, newly initialized and in the TU_ON_HOOK state, if initialization
 * was successful, otherwise NULL.
 */
TU *tu_init(int fd) {
    TU *tu = malloc(sizeof(TU)); // Allocate memory for the TU - gotta make sure to manage this to free it later
    if (!tu) { // Check for memory allocation failure
        return NULL;
    }

    pthread_mutex_lock(&tu->mutex);

    tu->fd = fd; // Set file descriptor for the TU
    // tu->ext = -1; // Initialize extension number to -1 (unset)
    tu->ext = fd;
    tu->ref_count = 1; // Set initial reference count to 1
    tu->state = TU_ON_HOOK; // Initialize state to ON_HOOK
    tu->peer = NULL; // No peer connected initially

    pthread_mutex_init(&tu->mutex, NULL); // Initialize the mutex

    char buffer [15];
    snprintf(buffer, 15, "ON HOOK %d\r\n", fd);
    notify_client(fd, buffer); // Notify the client of the initial state

    pthread_mutex_unlock(&tu->mutex);

    return tu;
}

/*
 * Increment the reference count on a TU.
 *
 * @param tu  The TU whose reference count is to be incremented
 * @param reason  A string describing the reason why the count is being incremented
 * (for debugging purposes).
 */
void tu_ref(TU *tu, char *reason) {
    tu->ref_count++; // Increment reference count
    // fprintf(stderr, "TU reference count incremented: %s (count=%d)\n", reason, tu->ref_count); // Log the operation
}

/*
 * Decrement the reference count on a TU, freeing it if the count becomes 0.
 *
 * @param tu  The TU whose reference count is to be decremented
 * @param reason  A string describing the reason why the count is being decremented
 * (for debugging purposes).
 */
void tu_unref(TU *tu, char *reason) {
    if (!tu) return;

    pthread_mutex_lock(&tu->mutex);
    tu->ref_count--; // Decrement reference count
    int ref_count = tu->ref_count;
    pthread_mutex_unlock(&tu->mutex);

    if (ref_count == 0) { // If reference count reaches 0
        // fprintf(stderr, "Freeing TU resources: %s\n", reason);

        if (tu->peer) { // if peer exists

            pthread_mutex_lock(&tu->peer->mutex);

            tu->peer->state = TU_ON_HOOK; // reset peer state

            char buffer [15];
            snprintf(buffer, 15, "ON HOOK %d\r\n", tu->peer->fd);
            notify_client(tu->peer->fd, buffer); // Notify the client of the initial state

            tu_unref(tu->peer, "Peer disconnected during cleanup"); // unref peer
            tu->peer->peer = NULL;
            tu->peer = NULL;

            pthread_mutex_unlock(&tu->peer->mutex);

        }

        if (tu->fd >= 0) { // close file descriptor if valid
            close(tu->fd);
            tu->fd = -1; // mark closed
        }

        pthread_mutex_destroy(&tu->mutex);
        free(tu); // Free the TU memory
    }
}

/*
 * Get the file descriptor for the network connection underlying a TU.
 * This file descriptor should only be used by a server to read input from
 * the connection.  Output to the connection must only be performed within
 * the PBX functions.
 *
 * @param tu
 * @return the underlying file descriptor, if any, otherwise -1.
 */
int tu_fileno(TU *tu) {
    return tu->fd; // returns tu file descriptor (by default it is -1)
}

/*
 * Get the extension number for a TU.
 * This extension number is assigned by the PBX when a TU is registered
 * and it is used to identify a particular TU in calls to tu_dial().
 * The value returned might be the same as the value returned by tu_fileno(),
 * but is not necessarily so.
 *
 * @param tu
 * @return the extension number, if any, otherwise -1.
 */
int tu_extension(TU *tu) {
    int ext = tu->ext; // retrieves the extension number
    return ext;
}

/*
 * Set the extension number for a TU.
 * A notification is set to the client of the TU.
 * This function should be called at most once one any particular TU.
 *
 * @param tu  The TU whose extension is being set.
 */
int tu_set_extension(TU *tu, int ext) {

    if (tu->ext != -1) { // Check if the extension is already set
        return -1;
    }

    tu->ext = ext; // Set the extension number

    char buffer [15];
    snprintf(buffer, 15, "ON HOOK %d\r\n", tu->ext);
    notify_client(tu->fd, buffer); // Notify the client of the initial state

    return 0;
}

/*
 * Initiate a call from a specified originating TU to a specified target TU.
 *   If the originating TU is not in the TU_DIAL_TONE state, then there is no effect.
 *   If the target TU is the same as the originating TU, then the TU transitions
 *     to the TU_BUSY_SIGNAL state.
 *   If the target TU already has a peer, or the target TU is not in the TU_ON_HOOK
 *     state, then the originating TU transitions to the TU_BUSY_SIGNAL state.
 *   Otherwise, the originating TU and the target TU are recorded as peers of each other
 *     (this causes the reference count of each of them to be incremented),
 *     the target TU transitions to the TU_RINGING state, and the originating TU
 *     transitions to the TU_RING_BACK state.
 *
 * In all cases, a notification of the resulting state of the originating TU is sent to
 * to the associated network client.  If the target TU has changed state, then its client
 * is also notified of its new state.
 *
 * If the caller of this function was unable to determine a target TU to be called,
 * it will pass NULL as the target TU.  In this case, the originating TU will transition
 * to the TU_ERROR state if it was in the TU_DIAL_TONE state, and there will be no
 * effect otherwise.  This situation is handled here, rather than in the caller,
 * because here we have knowledge of the current TU state and we do not want to introduce
 * the possibility of transitions to a TU_ERROR state from arbitrary other states,
 * especially in states where there could be a peer TU that would have to be dealt with.
 *
 * @param tu  The originating TU.
 * @param target  The target TU, or NULL if the caller of this function was unable to
 * identify a TU to be dialed.
 * @return 0 if successful, -1 if any error occurs that results in the originating
 * TU transitioning to the TU_ERROR state.
 */
int tu_dial(TU *tu, TU *target) {
    if (!tu) return -1;

    pthread_mutex_lock(&tu->mutex); // locks mutex for originating tu

    if (tu->state != TU_DIAL_TONE) { // Ensure TU is in DIAL_TONE state
        // response will be sent from server that just repeats the state of the TU which does not change
        notify_client_of_tu_state(tu);
        pthread_mutex_unlock(&tu->mutex);
        return -1; // originating TU is not in the TU_DIAL_TONE state, then there is no effect
    }

    if (!target) { // Check if target is NULL
        tu->state = TU_ERROR; // Set TU state to ERROR
        notify_client(tu->fd, "ERROR\r\n");
        pthread_mutex_unlock(&tu->mutex);
        return -1;
    }

    pthread_mutex_unlock(&tu->mutex);

    safe_mutex_lock(tu, target);

    if (tu == target ||                // Check if TU dials itself
        target->state != TU_ON_HOOK || // Target is not ON_HOOK
        target->peer != NULL) {        // Target has peer connection already

        // fprintf(stderr, "Target is invalid\n");

        tu->state = TU_BUSY_SIGNAL; // Set TU state to BUSY_SIGNAL (do i have to notify the client?)
        notify_client(tu->fd, "BUSY SIGNAL\r\n");

        safe_mutex_unlock(tu, target);

        return -1;
    }

    // fprintf(stderr, "Target is valid: setting up tu_target\n");

    // establish the connection
    tu->peer = target; // Set target as TU's peer
    target->peer = tu; // Set TU as target's peer

    tu_ref(target, "Dial target"); // Increment reference count for target
    tu_ref(tu, "Dial originating"); // Increment reference count for tu

    tu->state = TU_RING_BACK; // Set TU state to RING_BACK
    target->state = TU_RINGING; // Set target state to RINGING

    notify_client(tu->fd, "RING BACK\r\n"); // Notify the client
    notify_client(target->fd, "RINGING\r\n"); // Notify the target

    safe_mutex_unlock(tu, target);

    return 0;
}

/*
 * Take a TU receiver off-hook (i.e. pick up the handset).
 *   If the TU is in neither the TU_ON_HOOK state nor the TU_RINGING state,
 *     then there is no effect.
 *   If the TU is in the TU_ON_HOOK state, it goes to the TU_DIAL_TONE state.
 *   If the TU was in the TU_RINGING state, it goes to the TU_CONNECTED state,
 *     reflecting an answered call.  In this case, the calling TU simultaneously
 *     also transitions to the TU_CONNECTED state.
 *
 * In all cases, a notification of the resulting state of the specified TU is sent to
 * to the associated network client.  If a peer TU has changed state, then its client
 * is also notified of its new state.
 *
 * @param tu  The TU that is to be picked up.
 * @return 0 if successful, -1 if any error occurs that results in the originating
 * TU transitioning to the TU_ERROR state.
 */
int tu_pickup(TU *tu) {
    if (!tu) return -1;

    pthread_mutex_lock(&tu->mutex);

    if (tu->state != TU_ON_HOOK && tu->state != TU_RINGING) {
        notify_client_of_tu_state(tu);
        pthread_mutex_unlock(&tu->mutex);
        return 0;
    }

    else if (tu->state == TU_ON_HOOK) {
        tu->state = TU_DIAL_TONE;
        notify_client(tu->fd, "DIAL TONE\r\n");
    }

    else if (tu->state == TU_RINGING) {

        pthread_mutex_lock(&tu->peer->mutex);

        tu->state = TU_CONNECTED;

        char buffer [15];
        snprintf(buffer, 15, "CONNECTED %d\r\n", tu->peer->ext);
        notify_client(tu->fd, buffer);

        tu->peer->state = TU_CONNECTED;

        char buff [15];
        snprintf(buff, 15, "CONNECTED %d\r\n", tu->ext);
        notify_client(tu->peer->fd, buff);

        pthread_mutex_unlock(&tu->peer->mutex);
    }

    pthread_mutex_unlock(&tu->mutex);

    return 0;
}

/*
 * Hang up a TU (i.e. replace the handset on the switchhook).
 *
 *   If the TU is in the TU_CONNECTED or TU_RINGING state, then it goes to the
 *     TU_ON_HOOK state.  In addition, in this case the peer TU (the one to which
 *     the call is currently connected) simultaneously transitions to the TU_DIAL_TONE
 *     state.
 *   If the TU was in the TU_RING_BACK state, then it goes to the TU_ON_HOOK state.
 *     In addition, in this case the calling TU (which is in the TU_RINGING state)
 *     simultaneously transitions to the TU_ON_HOOK state.
 *   If the TU was in the TU_DIAL_TONE, TU_BUSY_SIGNAL, or TU_ERROR state,
 *     then it goes to the TU_ON_HOOK state.
 *
 * In all cases, a notification of the resulting state of the specified TU is sent to
 * to the associated network client.  If a peer TU has changed state, then its client
 * is also notified of its new state.
 *
 * @param tu  The tu that is to be hung up.
 * @return 0 if successful, -1 if any error occurs that results in the originating
 * TU transitioning to the TU_ERROR state.
 */
int tu_hangup(TU *tu) {
    if (!tu) return -1;

    pthread_mutex_lock(&tu->mutex);

    if (tu->state == TU_CONNECTED || tu->state == TU_RINGING) {
        pthread_mutex_lock(&tu->peer->mutex);

        tu->state = TU_ON_HOOK;

        char buffer [15];
        snprintf(buffer, 15, "ON HOOK %d\r\n", tu->fd);
        notify_client(tu->fd, buffer); // Notify the client of the initial state

        tu->peer->state = TU_DIAL_TONE;

        notify_client(tu->peer->fd, "DIAL TONE\r\n");

        tu->peer->peer = NULL;

        pthread_mutex_unlock(&tu->peer->mutex);
    }

    else if (tu->state == TU_RING_BACK) {
        pthread_mutex_lock(&tu->peer->mutex);

        tu->state = TU_ON_HOOK;

        char buff [15];
        snprintf(buff, 15, "ON HOOK %d\r\n", tu->fd);
        notify_client(tu->fd, buff);

        tu->peer->state = TU_ON_HOOK; // i think the peer is the calling tu?

        char buffer [15];
        snprintf(buffer, 15, "ON HOOK %d\r\n", tu->peer->fd);
        notify_client(tu->peer->fd, buffer);

        tu->peer->peer = NULL;

        pthread_mutex_unlock(&tu->peer->mutex);
    }

    else if (tu->state == TU_DIAL_TONE || tu->state == TU_BUSY_SIGNAL || tu->state == TU_ERROR) {
        tu->state = TU_ON_HOOK;

        char buffer [15];
        snprintf(buffer, 15, "ON HOOK %d\r\n", tu->fd);
        notify_client(tu->fd, buffer); // Notify the client of the initial state
    }

    else {
        notify_client_of_tu_state(tu);
        pthread_mutex_unlock(&tu->mutex);
        return -1;
    }

    tu_unref(tu->peer, "Peer disconnected"); // Decrement peer's ref count
    tu->peer = NULL; // Disconnect the peer

    pthread_mutex_unlock(&tu->mutex);

    return 0;
}

/*
 * "Chat" over a connection.
 *
 * If the state of the TU is not TU_CONNECTED, then nothing is sent and -1 is returned.
 * Otherwise, the specified message is sent via the network connection to the peer TU.
 * In all cases, the states of the TUs are left unchanged and a notification containing
 * the current state is sent to the TU sending the chat.
 *
 * @param tu  The tu sending the chat.
 * @param msg  The message to be sent.
 * @return 0  If the chat was successfully sent, -1 if there is no call in progress
 * or some other error occurs.
 */
int tu_chat(TU *tu, char *msg) {
    if (!tu || !msg) return -1;

    pthread_mutex_lock(&tu->mutex);

    if (tu->state != TU_CONNECTED || !tu->peer) { // Ensure TU is connected
        // fprintf(stderr, "TU is not connected with a peer\n");
        notify_client_of_tu_state(tu);
        pthread_mutex_unlock(&tu->mutex);
        return -1;
    }

    pthread_mutex_unlock(&tu->mutex);

    safe_mutex_lock(tu, tu->peer);

    size_t message_length = strlen("CHAT ") + strlen(msg) + strlen("\r\n") + 1;
    char *message = malloc(message_length);
    if (!message) {
        safe_mutex_unlock(tu, tu->peer);
        return -1;
    }

    snprintf(message, message_length, "CHAT %s\r\n", msg);

    // fprintf(stderr, "message received by tu_chat: %s (send to ext: %d)\n", message, tu->peer->ext);

    notify_client(tu->peer->ext, message); // Send the message to the peer

    free(message);

    // do i need this?
    char buffer [15];
    snprintf(buffer, 15, "CONNECTED %d\r\n", tu->peer->ext);
    notify_client(tu->fd, buffer);

    safe_mutex_unlock(tu, tu->peer);

    return 0;
}