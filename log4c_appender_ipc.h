#ifndef LOG4C_APPENDER_IPC_H
#define LOG4C_APPENDER_IPC_H


/**
 * @file log4c_appender_ipc.h
 *
 * @brief The appender supports multiprocess logging.
 *
 * It processes messages in 2 steps:
 *  - receive messages from different processes via message queue
 *  - forward them to standard stream2 appender.
 *
 * By this approach we chain our appender with the existing one with minimal
 * intervention.
 *
 * Each process will use its own instance of appender that will only push messages
 * into queue. There will be one instance that will also take over messages from queue
 * to rollingfile appender. This instance will start background thread for this purpose.
 * This instance will be aslo in charge for queue creation and destroy. For this reason,
 * it is called "master" instance.
 *
 * The crusial part is recovering after crash. As message queue and semaphores
 * has kernel persistance, they remain in file system after abnormal termination.
 * At the same time we need to differentiate master instance from non-master to
 * avoid doubling commonly used data. File system objects can't be an
 * indicator of the number of instances, as files may remain after a crash.
 *
 * We use "handshake" messages to check if the master instance available.
 * For this, we send one request message (ping) and waits will response ("pong").
 * These service messages are skipped from message flow.  If crashed, we can
 * stably determine that master instance is dead regardless of file system state.
 *
 * See the documentation in the appender_type_stream2.h file for
 * more details on the standard stream2 appender.
 *
 *
*/

#include <log4c/defs.h>
#include <log4c/appender.h>
#include <log4c/rollingpolicy.h>

__LOG4C_BEGIN_DECLS

/**
 * IPC appender type definition.
 *
 * This should be used as a parameter to the log4c_appender_set_type()
 * routine to set the type of the appender.
 *
 **/
extern const log4c_appender_type_t log4c_appender_type_appender_ipc;

/**
 * Undecorated layout.
 *
 * Used to propagate messages w/o any modifications to save original timestamps
 * and logging levels
 */
extern const log4c_layout_type_t log4c_layout_type_raw;

__LOG4C_END_DECLS


#endif // LOG4C_APPENDER_IPC_H
