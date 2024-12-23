/*
 * PBX: simulates a Private Branch Exchange.
 */
#include <stdlib.h>
#include <pthread.h>
#include <sys/socket.h>

#include "pbx.h" // includes tu.h already
#include "debug.h"

// Definition of the PBX structure
struct pbx {
    TU *extensions[PBX_MAX_EXTENSIONS]; // Array mapping extensions to TUs
    pthread_mutex_t lock;              // Mutex for thread safety to try my best to avoid race conditions
    int active_tus;                    // Counter for active TUs
    pthread_cond_t shutdown_cond;      // Condition variable for shutdown synchronization
};

/*
 * Initialize a new PBX.
 *
 * @return the newly initialized PBX, or NULL if initialization fails.
 */
PBX *pbx_init() {
    PBX *pbx = malloc(sizeof(PBX)); // Allocate memory for PBX object
    if (!pbx) return NULL; // Return NULL if allocation fails

    // attempting to add lock to prevent re entrancy issues

    for (int i = 0; i < PBX_MAX_EXTENSIONS; i++) {
        pbx->extensions[i] = NULL; // Initialize all extensions to NULL
    }

    if (pthread_mutex_init(&pbx->lock, NULL) != 0) { // Initialize mutex
        free(pbx);
        return NULL;
    }

    if (pthread_cond_init(&pbx->shutdown_cond, NULL) != 0) { // Initialize condition variable
        free(pbx);
        return NULL;
    }

    pbx->active_tus = 0; // Initialize active TU count to 0

    return pbx; // Return initialized PBX
}


/*
 * Shut down a pbx, shutting down all network connections, waiting for all server
 * threads to terminate, and freeing all associated resources.
 * If there are any registered extensions, the associated network connections are
 * shut down, which will cause the server threads to terminate.
 * Once all the server threads have terminated, any remaining resources associated
 * with the PBX are freed.  The PBX object itself is freed, and should not be used again.
 *
 * @param pbx  The PBX to be shut down.
 */
void pbx_shutdown(PBX *pbx) {
    if (!pbx) return;

    pthread_mutex_lock(&pbx->lock);

    // Shut down all TUs and increment references to delay cleanup
    for (int i = 0; i < PBX_MAX_EXTENSIONS; i++) {
        if (pbx->extensions[i]) { // extensions exists
            shutdown(tu_fileno(pbx->extensions[i]), SHUT_RDWR); // shutdown associated network connection
            tu_ref(pbx->extensions[i], "Shutdown in progress"); // increment tu reference count to delay cleanup
        }
    }

    // Wait for all active TUs to unregister
    while (pbx->active_tus > 0) {
        pthread_cond_wait(&pbx->shutdown_cond, &pbx->lock);
    }

    // Release references incremented during shutdown
    for (int i = 0; i < PBX_MAX_EXTENSIONS; i++) {
        if (pbx->extensions[i]) {
            tu_unref(pbx->extensions[i], "Shutdown complete");
            pbx->extensions[i] = NULL;
        }
    }

    pthread_mutex_unlock(&pbx->lock);

    pthread_mutex_destroy(&pbx->lock); // clean up mutex
    pthread_cond_destroy(&pbx->shutdown_cond); // clean up condition variable

    free(pbx);
}

/*
 * Register a telephone unit with a PBX at a specified extension number.
 * This amounts to "plugging a telephone unit into the PBX".
 * The TU is initialized to the TU_ON_HOOK state.
 * The reference count of the TU is increased and the PBX retains this reference
 *for as long as the TU remains registered.
 * A notification of the assigned extension number is sent to the underlying network
 * client.
 *
 * @param pbx  The PBX registry.
 * @param tu  The TU to be registered.
 * @param ext  The extension number on which the TU is to be registered.
 * @return 0 if registration succeeds, otherwise -1.
 */
int pbx_register(PBX *pbx, TU *tu, int ext) {
    if (!pbx || !tu || ext < 0 || ext >= PBX_MAX_EXTENSIONS) {
        fprintf(stderr, "ERROR pbx_register: Invalid parameters\n");
        return -1; // Return error for invalid inputs
    }

    pthread_mutex_lock(&pbx->lock); // lock on registration

    if (pbx->extensions[ext]) { // Check if extension is already in use
        fprintf(stderr, "ERROR pbx_register: Extension %d is already in use\n", ext);
        pthread_mutex_unlock(&pbx->lock); // unlock after registration
        return -1;
    }

    pbx->extensions[ext] = tu; // Register TU
    pbx->active_tus++; // Increment active TU count
    tu_set_extension(tu, ext); // Assign extension to TU
    tu_ref(tu, "Registering TU"); // Increment TU reference count

    // fprintf(stderr, "pbx_register: TU registered on extension %d\n", ext);

    pthread_mutex_unlock(&pbx->lock); // unlock after registration

    return 0;
}

/*
 * Unregister a TU from a PBX.
 * This amounts to "unplugging a telephone unit from the PBX".
 * The TU is disassociated from its extension number.
 * Then a hangup operation is performed on the TU to cancel any
 * call that might be in progress.
 * Finally, the reference held by the PBX to the TU is released.
 *
 * @param pbx  The PBX.
 * @param tu  The TU to be unregistered.
 * @return 0 if unregistration succeeds, otherwise -1.
 */
int pbx_unregister(PBX *pbx, TU *tu) {
    pthread_mutex_lock(&pbx->lock); // lock at unregistration

    if (!pbx || !tu) {
        fprintf(stderr, "ERROR pbx_unregister: Invalid parameters\n");
        pthread_mutex_unlock(&pbx->lock);
        return -1;
    }

    int ext = tu_extension(tu); // retrieve tu assigned extension number
    if (ext < 0 || ext >= PBX_MAX_EXTENSIONS || pbx->extensions[ext] != tu) {
        fprintf(stderr, "ERROR pbx_unregister: TU not registered or invalid extension %d\n", ext);
        pthread_mutex_unlock(&pbx->lock);
        return -1;
    }

    // Increment reference to prevent cleanup during operation
    tu_ref(tu, "Unregistering TU safely");

    pbx->extensions[ext] = NULL; // Remove TU from registry
    pbx->active_tus--; // Decrement active TU count

    tu_hangup(tu); // Terminate ongoing calls
    tu_unref(tu, "TU unregistered"); // Release TU reference for removal

    // Notify shutdown if no active TUs remain
    if (pbx->active_tus == 0) {
        pthread_cond_signal(&pbx->shutdown_cond);
    }

    // Release additional reference held during operation
    tu_unref(tu, "Unregister operation complete");

    // fprintf(stderr, "pbx_unregister: TU unregistered from extension %d\n", ext);
    pthread_mutex_unlock(&pbx->lock);

    return 0;
}

/*
 * Use the PBX to initiate a call from a specified TU to a specified extension.
 *
 * @param pbx  The PBX registry.
 * @param tu  The TU that is initiating the call.
 * @param ext  The extension number to be called.
 * @return 0 if dialing succeeds, otherwise -1.
 */
int pbx_dial(PBX *pbx, TU *tu, int ext) {

    if (!pbx || !tu || ext < 0 || ext >= PBX_MAX_EXTENSIONS) {
        fprintf(stderr, "ERROR pbx_dial: Invalid parameters\n");
        return -1;
    }

    pthread_mutex_lock(&pbx->lock);

    TU *target_tu = pbx->extensions[ext]; // retrieve target tu for specified extension
    if (!target_tu) { // Check if target exists
        fprintf(stderr, "ERROR pbx_dial: No TU registered on extension %d\n", ext);
        pthread_mutex_unlock(&pbx->lock);
        return -1;
    }

    // Increment references to ensure both TUs remain valid during dialing
    tu_ref(tu, "Dialing TU"); // increment reference count for originating tu
    tu_ref(target_tu, "Dialing target TU"); // increment reference count for target tu

    // Perform dialing operation
    int result = tu_dial(tu, target_tu);

    // Release references after dialing
    tu_unref(target_tu, "Dialing target complete");
    tu_unref(tu, "Dialing TU complete");

    pthread_mutex_unlock(&pbx->lock);

    return result;
}